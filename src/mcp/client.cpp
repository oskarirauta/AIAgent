#include "agent/mcp/client.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include "logger.hpp"

namespace agent::mcp {

namespace {

constexpr int HANDSHAKE_TIMEOUT_MS = 10000;
constexpr int CALL_TIMEOUT_MS      = 60000;

// A registered tool name must be a valid function name for the providers
// (^[a-zA-Z0-9_-]{1,64}$). Namespace as mcp__<server>__<tool>, sanitising.
std::string sanitise(const std::string& s) {
    std::string o;
    for ( char c : s )
        o += ( std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ) ? c : '_';
    return o;
}

std::string registered_name(const std::string& server, const std::string& tool) {
    std::string n = "mcp__" + sanitise(server) + "__" + sanitise(tool);
    if ( n.size() > 64 )
        n = n.substr(0, 64);
    return n;
}

} // namespace

Client::~Client() {
    shutdown();
}

int Client::load_config(const std::string& path) {
    std::ifstream ifd(path, std::ios::in);
    if ( !ifd.is_open())
        return 0;
    std::stringstream ss;
    ss << ifd.rdbuf();
    std::string raw = ss.str();
    if ( raw.empty())
        return 0;

    JSON root;
    try {
        root = JSON::parse(raw);
    } catch ( const std::exception& e ) {
        logger::warning["mcp"] << "failed to parse " << path << ": " << e.what() << std::endl;
        return 0;
    }
    if ( !root.contains("mcpServers") || root["mcpServers"] != JSON::TYPE::OBJECT )
        return 0;

    JSON servers = root["mcpServers"];
    servers.for_each([this](JSON::fe_iterator& it, JSON& def) {
        if ( !it.is_object() || def != JSON::TYPE::OBJECT )
            return;
        if ( !def.contains("command") || def["command"] != JSON::TYPE::STRING )
            return;
        auto s = std::make_unique<Server>();
        s->name = it.name();
        s->command = def["command"].to_string();
        if ( def.contains("args") && def["args"] == JSON::TYPE::ARRAY ) {
            JSON a = def["args"];
            for ( size_t i = 0; i < a.size(); ++i )
                s->args.push_back(a[i].to_string());
        }
        if ( def.contains("env") && def["env"] == JSON::TYPE::OBJECT ) {
            def["env"].for_each([&s](JSON::fe_iterator& eit, JSON& val) {
                if ( eit.is_object())
                    s->env[eit.name()] = val.to_string();
            });
        }
        _servers.push_back(std::move(s));
    });
    return static_cast<int>(_servers.size());
}

bool Client::spawn(Server& s) {
    int to_child[2];   // parent writes -> child stdin
    int from_child[2]; // child stdout -> parent reads
    if ( pipe(to_child) != 0 || pipe(from_child) != 0 ) {
        s.error = "pipe() failed";
        return false;
    }

    pid_t pid = fork();
    if ( pid < 0 ) {
        s.error = "fork() failed";
        return false;
    }

    if ( pid == 0 ) {
        // Child: wire up stdio, silence stderr, exec the server.
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if ( devnull >= 0 ) { dup2(devnull, STDERR_FILENO); close(devnull); }
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);

        for ( const auto& [k, v] : s.env )
            setenv(k.c_str(), v.c_str(), 1);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(s.command.c_str()));
        for ( auto& a : s.args )
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(s.command.c_str(), argv.data());
        _exit(127); // exec failed
    }

    // Parent.
    close(to_child[0]);
    close(from_child[1]);
    s.pid = pid;
    s.in_fd = to_child[1];
    s.out_fd = from_child[0];
    return true;
}

bool Client::write_all(Server& s, const std::string& data) {
    size_t off = 0;
    while ( off < data.size()) {
        ssize_t n = ::write(s.in_fd, data.data() + off, data.size() - off);
        if ( n < 0 ) {
            if ( errno == EINTR ) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool Client::read_line(Server& s, std::string& line, int timeout_ms) {
    // Return a complete line (without the trailing newline) from the server,
    // buffering partial reads. Bounded by timeout_ms.
    for ( ;; ) {
        size_t nl = s.rbuf.find('\n');
        if ( nl != std::string::npos ) {
            line = s.rbuf.substr(0, nl);
            s.rbuf.erase(0, nl + 1);
            return true;
        }
        struct pollfd pfd { s.out_fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, timeout_ms);
        if ( pr <= 0 )
            return false; // timeout or error
        char buf[8192];
        ssize_t n = ::read(s.out_fd, buf, sizeof(buf));
        if ( n <= 0 )
            return false; // EOF / error (server died)
        s.rbuf.append(buf, static_cast<size_t>(n));
    }
}

JSON Client::request(Server& s, const std::string& method, const JSON& params, int timeout_ms) {
    std::lock_guard<std::mutex> lk(s.mx);
    int id = s.next_id++;
    JSON req = JSON::Object{
        { "jsonrpc", "2.0" },
        { "id", static_cast<long long>(id) },
        { "method", method },
        { "params", params }
    };
    if ( !write_all(s, req.dump_minified() + "\n"))
        throw std::runtime_error("write to server failed");

    // Read until the response matching our id arrives (skipping notifications).
    for ( ;; ) {
        std::string line;
        if ( !read_line(s, line, timeout_ms))
            throw std::runtime_error("timed out waiting for response");
        if ( line.empty())
            continue;
        JSON msg;
        try { msg = JSON::parse(line); } catch ( ... ) { continue; }
        if ( !msg.contains("id") || msg["id"] != JSON::TYPE::INT )
            continue; // a notification or unrelated message
        if ( static_cast<long long>(msg["id"]) != id )
            continue;
        if ( msg.contains("error")) {
            std::string m = "server error";
            if ( msg["error"] == JSON::TYPE::OBJECT && msg["error"].contains("message"))
                m = msg["error"]["message"].to_string();
            throw std::runtime_error(m);
        }
        return msg.contains("result") ? msg["result"] : JSON::Object{};
    }
}

void Client::notify(Server& s, const std::string& method, const JSON& params) {
    std::lock_guard<std::mutex> lk(s.mx);
    JSON n = JSON::Object{
        { "jsonrpc", "2.0" },
        { "method", method },
        { "params", params }
    };
    write_all(s, n.dump_minified() + "\n");
}

void Client::handshake(Server& s) {
    JSON init_params = JSON::Object{
        { "protocolVersion", "2024-11-05" },
        { "capabilities", JSON::Object{} },
        { "clientInfo", JSON::Object{ { "name", "ai-agent" }, { "version", "0.1.0" } } }
    };
    request(s, "initialize", init_params, HANDSHAKE_TIMEOUT_MS);
    notify(s, "notifications/initialized", JSON::Object{});

    JSON result = request(s, "tools/list", JSON::Object{}, HANDSHAKE_TIMEOUT_MS);
    if ( result.contains("tools") && result["tools"] == JSON::TYPE::ARRAY ) {
        JSON arr = result["tools"];
        for ( size_t i = 0; i < arr.size(); ++i ) {
            JSON t = arr[i];
            if ( !t.contains("name") || t["name"] != JSON::TYPE::STRING )
                continue;
            ToolDef td;
            td.server = s.name;
            td.tool = t["name"].to_string();
            td.registered = registered_name(s.name, td.tool);
            td.description = t.contains("description") ? t["description"].to_string() : "";
            td.input_schema = t.contains("inputSchema") ? t["inputSchema"]
                                                        : JSON::Object{ { "type", "object" } };
            s.tools.push_back(td);
        }
    }
}

void Client::connect_all() {
    signal(SIGPIPE, SIG_IGN); // writing to a dead server must not kill us
    for ( auto& sp : _servers ) {
        Server& s = *sp;
        if ( !spawn(s)) {
            logger::warning["mcp"] << s.name << ": " << s.error << std::endl;
            continue;
        }
        try {
            handshake(s);
            s.connected = true;
            logger::info["mcp"] << "connected " << s.name << " ("
                                << s.tools.size() << " tool(s))" << std::endl;
        } catch ( const std::exception& e ) {
            s.error = e.what();
            logger::warning["mcp"] << s.name << " handshake failed: " << e.what() << std::endl;
            // Reap the misbehaving child.
            if ( s.pid > 0 ) { kill(s.pid, SIGTERM); }
        }
    }
}

std::vector<ToolDef> Client::tools() const {
    std::vector<ToolDef> out;
    for ( const auto& sp : _servers )
        if ( sp->connected )
            for ( const auto& t : sp->tools )
                out.push_back(t);
    return out;
}

std::string Client::call_tool(const std::string& server, const std::string& tool, const JSON& args) {
    for ( auto& sp : _servers ) {
        if ( sp->name != server )
            continue;
        if ( !sp->connected )
            return "error: MCP server '" + server + "' is not connected";
        try {
            JSON params = JSON::Object{
                { "name", tool },
                { "arguments", args == JSON::TYPE::OBJECT ? args : JSON::Object{} }
            };
            JSON result = request(*sp, "tools/call", params, CALL_TIMEOUT_MS);

            // Concatenate text content blocks; note tool-side errors.
            std::string text;
            if ( result.contains("content") && result["content"] == JSON::TYPE::ARRAY ) {
                JSON content = result["content"];
                for ( size_t i = 0; i < content.size(); ++i ) {
                    JSON block = content[i];
                    if ( block.contains("type") && block["type"].to_string() == "text" &&
                         block.contains("text"))
                        text += block["text"].to_string();
                }
            }
            bool is_error = result.contains("isError") && result["isError"] == JSON::TYPE::BOOL &&
                            result["isError"].to_bool();
            if ( text.empty())
                text = "(no textual content returned)";
            return is_error ? ("tool reported an error: " + text) : text;
        } catch ( const std::exception& e ) {
            return std::string("error: MCP call failed: ") + e.what();
        }
    }
    return "error: no MCP server named '" + server + "'";
}

std::vector<Client::ServerInfo> Client::status() const {
    std::vector<ServerInfo> out;
    for ( const auto& sp : _servers ) {
        ServerInfo si;
        si.name = sp->name;
        si.connected = sp->connected;
        si.error = sp->error;
        si.command = sp->command;
        for ( const auto& t : sp->tools )
            si.tool_names.push_back(t.tool);
        out.push_back(si);
    }
    return out;
}

void Client::shutdown() {
    for ( auto& sp : _servers ) {
        Server& s = *sp;
        if ( s.in_fd >= 0 ) { close(s.in_fd); s.in_fd = -1; }
        if ( s.out_fd >= 0 ) { close(s.out_fd); s.out_fd = -1; }
        if ( s.pid > 0 ) {
            kill(s.pid, SIGTERM);
            // Brief grace, then reap.
            for ( int i = 0; i < 20; ++i ) {
                int st;
                pid_t r = waitpid(s.pid, &st, WNOHANG);
                if ( r == s.pid ) { s.pid = -1; break; }
                usleep(5000);
            }
            if ( s.pid > 0 ) { kill(s.pid, SIGKILL); waitpid(s.pid, nullptr, 0); s.pid = -1; }
        }
        s.connected = false;
    }
}

} // namespace agent::mcp
