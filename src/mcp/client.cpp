#include "agent/mcp/client.hpp"
#include "agent/version.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <curl/curl.h>
#include "logger.hpp"

namespace agent::mcp {

namespace {

constexpr int HANDSHAKE_TIMEOUT_MS = 10000;
constexpr int CALL_TIMEOUT_MS      = 60000;
constexpr const char* PROTOCOL_VERSION = "2024-11-05";

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

std::string lower(std::string s) {
    for ( char& c : s ) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if ( a == std::string::npos ) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Concatenate text from an MCP content array (blocks of {type:"text",text:...}).
std::string content_text(const JSON& content) {
    std::string text;
    if ( content == JSON::TYPE::ARRAY ) {
        for ( size_t i = 0; i < content.size(); ++i ) {
            JSON b = content[i];
            if ( b.contains("text") && b["text"] == JSON::TYPE::STRING )
                text += b["text"].to_string();
        }
    }
    return text;
}

size_t curl_write(char* p, size_t sz, size_t nm, void* ud) {
    static_cast<std::string*>(ud)->append(p, sz * nm);
    return sz * nm;
}

struct HeaderCap { std::string content_type; std::string session_id; };

size_t curl_header(char* p, size_t sz, size_t nm, void* ud) {
    auto* h = static_cast<HeaderCap*>(ud);
    std::string line(p, sz * nm);
    size_t colon = line.find(':');
    if ( colon != std::string::npos ) {
        std::string key = lower(trim(line.substr(0, colon)));
        std::string val = trim(line.substr(colon + 1));
        if ( key == "content-type" ) h->content_type = lower(val);
        else if ( key == "mcp-session-id" ) h->session_id = val;
    }
    return sz * nm;
}

} // namespace

Client::~Client() { shutdown(); }

int Client::load_config(const std::string& path, bool trusted) {
    std::ifstream ifd(path, std::ios::in);
    if ( !ifd.is_open())
        return 0;
    std::stringstream ss;
    ss << ifd.rdbuf();
    std::string raw = ss.str();
    if ( raw.empty())
        return 0;

    JSON root;
    try { root = JSON::parse(raw); }
    catch ( const std::exception& e ) {
        logger::warning["mcp"] << "failed to parse " << path << ": " << e.what() << std::endl;
        return 0;
    }
    if ( !root.contains("mcpServers") || root["mcpServers"] != JSON::TYPE::OBJECT )
        return 0;

    JSON servers = root["mcpServers"];
    servers.for_each([this, trusted](JSON::fe_iterator& it, JSON& def) {
        if ( !it.is_object() || def != JSON::TYPE::OBJECT )
            return;
        auto s = std::make_unique<Server>();
        s->name = it.name();
        s->needs_approval = !trusted; // an untrusted config waits for the user
        if ( def.contains("url") && def["url"] == JSON::TYPE::STRING ) {
            s->transport = Transport::http;
            s->url = def["url"].to_string();
            if ( def.contains("headers") && def["headers"] == JSON::TYPE::OBJECT ) {
                def["headers"].for_each([&s](JSON::fe_iterator& hit, JSON& val) {
                    if ( hit.is_object())
                        s->headers.push_back({ hit.name(), val.to_string() });
                });
            }
        } else if ( def.contains("command") && def["command"] == JSON::TYPE::STRING ) {
            s->transport = Transport::stdio;
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
        } else {
            return; // neither url nor command
        }
        _servers.push_back(std::move(s));
    });
    return static_cast<int>(_servers.size());
}

// ── stdio transport ───────────────────────────────────────────────────────

bool Client::spawn(Server& s) {
    int to_child[2], from_child[2];
    if ( pipe(to_child) != 0 || pipe(from_child) != 0 ) { s.error = "pipe() failed"; return false; }
    pid_t pid = fork();
    if ( pid < 0 ) { s.error = "fork() failed"; return false; }
    if ( pid == 0 ) {
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
        for ( auto& a : s.args ) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(s.command.c_str(), argv.data());
        _exit(127);
    }
    close(to_child[0]); close(from_child[1]);
    s.pid = pid; s.in_fd = to_child[1]; s.out_fd = from_child[0];
    return true;
}

bool Client::write_all(Server& s, const std::string& data) {
    size_t off = 0;
    while ( off < data.size()) {
        ssize_t n = ::write(s.in_fd, data.data() + off, data.size() - off);
        if ( n < 0 ) { if ( errno == EINTR ) continue; return false; }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool Client::read_line(Server& s, std::string& line, int timeout_ms) {
    for ( ;; ) {
        size_t nl = s.rbuf.find('\n');
        if ( nl != std::string::npos ) {
            line = s.rbuf.substr(0, nl);
            s.rbuf.erase(0, nl + 1);
            return true;
        }
        struct pollfd pfd { s.out_fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, timeout_ms);
        if ( pr <= 0 ) return false;
        char buf[8192];
        ssize_t n = ::read(s.out_fd, buf, sizeof(buf));
        if ( n <= 0 ) return false;
        s.rbuf.append(buf, static_cast<size_t>(n));
    }
}

// ── http transport (Streamable HTTP) ────────────────────────────────────────

JSON Client::http_request(Server& s, const std::string& message, bool expect_response, int timeout_ms) {
    CURL* c = curl_easy_init();
    if ( !c )
        throw std::runtime_error("curl init failed");

    std::string body;
    HeaderCap hc;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");
    headers = curl_slist_append(headers, ( std::string("MCP-Protocol-Version: ") + PROTOCOL_VERSION ).c_str());
    if ( !s.session_id.empty())
        headers = curl_slist_append(headers, ( "Mcp-Session-Id: " + s.session_id ).c_str());
    for ( const auto& h : s.headers )
        headers = curl_slist_append(headers, ( h.first + ": " + h.second ).c_str());

    curl_easy_setopt(c, CURLOPT_URL, s.url.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, message.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(message.size()));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &hc);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if ( res != CURLE_OK )
        throw std::runtime_error(std::string("http: ") + curl_easy_strerror(res));
    if ( !hc.session_id.empty())
        s.session_id = hc.session_id;
    if ( code < 200 || code >= 300 )
        throw std::runtime_error("http status " + std::to_string(code));
    if ( !expect_response )
        return JSON::Object{};

    // Collect JSON-RPC response objects from either a plain JSON body or an SSE
    // stream (data: lines), and return the one carrying a result/error.
    auto pick_response = [](const std::string& text) -> JSON {
        auto consider = [](const std::string& raw, JSON& out) -> bool {
            try {
                JSON j = JSON::parse(raw);
                if ( j == JSON::TYPE::OBJECT && ( j.contains("result") || j.contains("error"))) {
                    out = j; return true;
                }
            } catch ( ... ) {}
            return false;
        };
        JSON found;
        // SSE frames?
        if ( text.find("data:") != std::string::npos ) {
            size_t pos = 0;
            while ( pos < text.size()) {
                size_t nl = text.find('\n', pos);
                std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                pos = ( nl == std::string::npos ) ? text.size() : nl + 1;
                size_t d = line.find("data:");
                if ( d != std::string::npos ) {
                    std::string data = trim(line.substr(d + 5));
                    if ( consider(data, found)) return found;
                }
            }
        }
        JSON j;
        if ( consider(text, j)) return j;
        throw std::runtime_error("no JSON-RPC response in body");
    };

    return pick_response(body);
}

// ── request / notify (transport-agnostic) ──────────────────────────────────

JSON Client::request(Server& s, const std::string& method, const JSON& params, int timeout_ms) {
    std::lock_guard<std::mutex> lk(s.mx);
    int id = s.next_id++;
    JSON req = JSON::Object{
        { "jsonrpc", "2.0" }, { "id", static_cast<long long>(id) },
        { "method", method }, { "params", params }
    };

    if ( s.transport == Transport::http ) {
        JSON resp = http_request(s, req.dump_minified(), true, timeout_ms);
        if ( resp.contains("error")) {
            std::string m = "server error";
            if ( resp["error"] == JSON::TYPE::OBJECT && resp["error"].contains("message"))
                m = resp["error"]["message"].to_string();
            throw std::runtime_error(m);
        }
        return resp.contains("result") ? resp["result"] : JSON::Object{};
    }

    // stdio
    if ( !write_all(s, req.dump_minified() + "\n"))
        throw std::runtime_error("write to server failed");
    for ( ;; ) {
        std::string line;
        if ( !read_line(s, line, timeout_ms))
            throw std::runtime_error("timed out waiting for response");
        if ( line.empty()) continue;
        JSON msg;
        try { msg = JSON::parse(line); } catch ( ... ) { continue; }
        if ( !msg.contains("id") || msg["id"] != JSON::TYPE::INT ) continue;
        if ( static_cast<long long>(msg["id"]) != id ) continue;
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
    JSON n = JSON::Object{ { "jsonrpc", "2.0" }, { "method", method }, { "params", params } };
    if ( s.transport == Transport::http ) {
        try { http_request(s, n.dump_minified(), false, HANDSHAKE_TIMEOUT_MS); } catch ( ... ) {}
    } else {
        write_all(s, n.dump_minified() + "\n");
    }
}

// ── handshake / discovery ───────────────────────────────────────────────────

void Client::handshake(Server& s) {
    JSON init_params = JSON::Object{
        { "protocolVersion", PROTOCOL_VERSION },
        { "capabilities", JSON::Object{} },
        { "clientInfo", JSON::Object{ { "name", "ai-agent" }, { "version", agent::VERSION } } }
    };
    JSON result = request(s, "initialize", init_params, HANDSHAKE_TIMEOUT_MS);
    if ( result.contains("capabilities") && result["capabilities"] == JSON::TYPE::OBJECT ) {
        JSON caps = result["capabilities"];
        s.has_resources = caps.contains("resources");
        s.has_prompts = caps.contains("prompts");
    }
    notify(s, "notifications/initialized", JSON::Object{});
}

void Client::discover(Server& s) {
    s.tools.clear();
    s.resources.clear();
    s.prompts.clear();

    JSON tl = request(s, "tools/list", JSON::Object{}, HANDSHAKE_TIMEOUT_MS);
    if ( tl.contains("tools") && tl["tools"] == JSON::TYPE::ARRAY ) {
        JSON arr = tl["tools"];
        for ( size_t i = 0; i < arr.size(); ++i ) {
            JSON t = arr[i];
            if ( !t.contains("name") || t["name"] != JSON::TYPE::STRING ) continue;
            ToolDef td;
            td.server = s.name;
            td.tool = t["name"].to_string();
            td.registered = registered_name(s.name, td.tool);
            td.description = t.contains("description") ? t["description"].to_string() : "";
            td.input_schema = t.contains("inputSchema") ? t["inputSchema"]
                                                        : JSON::Object{ { "type", "object" } };
            // Optional annotations: the server's own safety/display hints.
            if ( t.contains("annotations") && t["annotations"] == JSON::TYPE::OBJECT ) {
                JSON a = t["annotations"];
                if ( a.contains("title") && a["title"] == JSON::TYPE::STRING )
                    td.title = a["title"].to_string();
                if ( a.contains("readOnlyHint") && a["readOnlyHint"] == JSON::TYPE::BOOL )
                    td.read_only = a["readOnlyHint"].to_bool();
                if ( a.contains("destructiveHint") && a["destructiveHint"] == JSON::TYPE::BOOL )
                    td.destructive = a["destructiveHint"].to_bool();
            }
            s.tools.push_back(td);
        }
    }

    if ( s.has_resources ) {
        try {
            JSON rl = request(s, "resources/list", JSON::Object{}, HANDSHAKE_TIMEOUT_MS);
            if ( rl.contains("resources") && rl["resources"] == JSON::TYPE::ARRAY ) {
                JSON arr = rl["resources"];
                for ( size_t i = 0; i < arr.size(); ++i ) {
                    JSON r = arr[i];
                    if ( !r.contains("uri")) continue;
                    ResourceDef rd;
                    rd.server = s.name;
                    rd.uri = r["uri"].to_string();
                    rd.name = r.contains("name") ? r["name"].to_string() : rd.uri;
                    rd.description = r.contains("description") ? r["description"].to_string() : "";
                    rd.mime = r.contains("mimeType") ? r["mimeType"].to_string() : "";
                    s.resources.push_back(rd);
                }
            }
        } catch ( const std::exception& e ) {
            logger::verbose["mcp"] << s.name << " resources/list failed: " << e.what() << std::endl;
        }
    }

    if ( s.has_prompts ) {
        try {
            JSON pl = request(s, "prompts/list", JSON::Object{}, HANDSHAKE_TIMEOUT_MS);
            if ( pl.contains("prompts") && pl["prompts"] == JSON::TYPE::ARRAY ) {
                JSON arr = pl["prompts"];
                for ( size_t i = 0; i < arr.size(); ++i ) {
                    JSON p = arr[i];
                    if ( !p.contains("name")) continue;
                    PromptDef pd;
                    pd.server = s.name;
                    pd.name = p["name"].to_string();
                    pd.description = p.contains("description") ? p["description"].to_string() : "";
                    if ( p.contains("arguments") && p["arguments"] == JSON::TYPE::ARRAY ) {
                        JSON a = p["arguments"];
                        for ( size_t j = 0; j < a.size(); ++j )
                            if ( a[j].contains("name"))
                                pd.arguments.push_back(a[j]["name"].to_string());
                    }
                    s.prompts.push_back(pd);
                }
            }
        } catch ( const std::exception& e ) {
            logger::verbose["mcp"] << s.name << " prompts/list failed: " << e.what() << std::endl;
        }
    }
}

void Client::connect_one(Server& s) {
    if ( s.transport == Transport::stdio ) {
        if ( !spawn(s)) {
            logger::warning["mcp"] << s.name << ": " << s.error << std::endl;
            return;
        }
    }
    try {
        handshake(s);
        discover(s);
        s.connected = true;
        logger::info["mcp"] << "connected " << s.name << " (" << s.tools.size() << " tool(s), "
                            << s.resources.size() << " resource(s), " << s.prompts.size()
                            << " prompt(s))" << std::endl;
    } catch ( const std::exception& e ) {
        s.error = e.what();
        logger::warning["mcp"] << s.name << " handshake failed: " << e.what() << std::endl;
        if ( s.transport == Transport::stdio && s.pid > 0 )
            kill(s.pid, SIGTERM);
    }
}

void Client::connect_all() {
    signal(SIGPIPE, SIG_IGN);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Connect all servers in parallel so startup is bounded by the slowest one,
    // not the sum of every handshake. Servers from an untrusted config wait for
    // the user's approval (pending_approvals / approve_connect) and are skipped.
    std::vector<std::thread> threads;
    for ( auto& sp : _servers ) {
        if ( sp->needs_approval && !sp->connected )
            continue;
        threads.emplace_back([this, s = sp.get()]() { connect_one(*s); });
    }
    for ( auto& t : threads )
        if ( t.joinable()) t.join();
}

std::vector<std::pair<std::string, std::string>> Client::pending_approvals() const {
    std::vector<std::pair<std::string, std::string>> out;
    for ( const auto& sp : _servers ) {
        if ( !sp->needs_approval || sp->connected )
            continue;
        std::string what = sp->command;
        for ( const auto& a : sp->args )
            what += " " + a;
        if ( sp->transport == Transport::http )
            what = sp->url;
        out.push_back({ sp->name, what });
    }
    return out;
}

bool Client::approve_connect(const std::string& name) {
    for ( auto& sp : _servers ) {
        if ( sp->name != name || sp->connected )
            continue;
        sp->needs_approval = false; // approved
        connect_one(*sp);
        return sp->connected;
    }
    return false;
}

int Client::refresh() {
    int total = 0;
    for ( auto& sp : _servers ) {
        if ( !sp->connected ) continue;
        try { discover(*sp); }
        catch ( const std::exception& e ) {
            logger::warning["mcp"] << sp->name << " refresh failed: " << e.what() << std::endl;
        }
        total += static_cast<int>(sp->tools.size());
    }
    return total;
}

// ── accessors ───────────────────────────────────────────────────────────────

Client::Server* Client::find(const std::string& name) {
    for ( auto& sp : _servers )
        if ( sp->name == name ) return sp.get();
    return nullptr;
}

std::vector<ToolDef> Client::tools() const {
    std::vector<ToolDef> out;
    for ( const auto& sp : _servers )
        if ( sp->connected )
            for ( const auto& t : sp->tools ) out.push_back(t);
    return out;
}

std::vector<ResourceDef> Client::resources() const {
    std::vector<ResourceDef> out;
    for ( const auto& sp : _servers )
        if ( sp->connected )
            for ( const auto& r : sp->resources ) out.push_back(r);
    return out;
}

std::vector<PromptDef> Client::prompts() const {
    std::vector<PromptDef> out;
    for ( const auto& sp : _servers )
        if ( sp->connected )
            for ( const auto& p : sp->prompts ) out.push_back(p);
    return out;
}

std::string Client::call_tool(const std::string& server, const std::string& tool, const JSON& args) {
    Server* s = find(server);
    if ( !s ) return "error: no MCP server named '" + server + "'";
    if ( !s->connected ) return "error: MCP server '" + server + "' is not connected";
    try {
        JSON params = JSON::Object{
            { "name", tool },
            { "arguments", args == JSON::TYPE::OBJECT ? args : JSON::Object{} }
        };
        JSON result = request(*s, "tools/call", params, CALL_TIMEOUT_MS);
        std::string text = result.contains("content") ? content_text(result["content"]) : "";
        bool is_error = result.contains("isError") && result["isError"] == JSON::TYPE::BOOL &&
                        result["isError"].to_bool();
        if ( text.empty()) text = "(no textual content returned)";
        return is_error ? ( "tool reported an error: " + text ) : text;
    } catch ( const std::exception& e ) {
        return std::string("error: MCP call failed: ") + e.what();
    }
}

std::string Client::read_resource(const std::string& server, const std::string& uri) {
    Server* s = find(server);
    if ( !s ) return "error: no MCP server named '" + server + "'";
    if ( !s->connected ) return "error: MCP server '" + server + "' is not connected";
    if ( uri.empty()) return "error: provide a resource `uri`";
    try {
        JSON result = request(*s, "resources/read", JSON::Object{ { "uri", uri } }, CALL_TIMEOUT_MS);
        std::string text;
        if ( result.contains("contents") && result["contents"] == JSON::TYPE::ARRAY ) {
            JSON arr = result["contents"];
            for ( size_t i = 0; i < arr.size(); ++i ) {
                JSON cc = arr[i];
                if ( cc.contains("text") && cc["text"] == JSON::TYPE::STRING )
                    text += cc["text"].to_string();
                else if ( cc.contains("blob"))
                    text += "[binary resource omitted]";
            }
        }
        return text.empty() ? "(resource has no text content)" : text;
    } catch ( const std::exception& e ) {
        return std::string("error: resources/read failed: ") + e.what();
    }
}

std::string Client::get_prompt(const std::string& server, const std::string& name,
                               const std::map<std::string, std::string>& args) {
    Server* s = find(server);
    if ( !s ) return "error: no MCP server named '" + server + "'";
    if ( !s->connected ) return "error: MCP server '" + server + "' is not connected";
    try {
        JSON jargs = JSON::Object{};
        for ( const auto& [k, v] : args )
            jargs[k] = v;
        JSON result = request(*s, "prompts/get",
                              JSON::Object{ { "name", name }, { "arguments", jargs } }, CALL_TIMEOUT_MS);
        std::string text;
        if ( result.contains("messages") && result["messages"] == JSON::TYPE::ARRAY ) {
            JSON arr = result["messages"];
            for ( size_t i = 0; i < arr.size(); ++i ) {
                JSON m = arr[i];
                std::string role = m.contains("role") ? m["role"].to_string() : "";
                std::string body;
                if ( m.contains("content")) {
                    JSON cc = m["content"];
                    if ( cc == JSON::TYPE::OBJECT && cc.contains("text"))
                        body = cc["text"].to_string();
                    else if ( cc == JSON::TYPE::ARRAY )
                        body = content_text(cc);
                }
                if ( !body.empty())
                    text += ( role.empty() ? "" : ( role + ": " )) + body + "\n";
            }
        }
        return text.empty() ? "(prompt produced no content)" : text;
    } catch ( const std::exception& e ) {
        return std::string("error: prompts/get failed: ") + e.what();
    }
}

std::vector<Client::ServerInfo> Client::status() const {
    std::vector<ServerInfo> out;
    for ( const auto& sp : _servers ) {
        ServerInfo si;
        si.name = sp->name;
        si.transport = ( sp->transport == Transport::http ) ? "http" : "stdio";
        si.connected = sp->connected;
        si.error = sp->error;
        si.command = ( sp->transport == Transport::http ) ? sp->url : sp->command;
        for ( const auto& t : sp->tools ) si.tool_names.push_back(t.tool);
        for ( const auto& r : sp->resources ) si.resource_uris.push_back(r.uri);
        for ( const auto& p : sp->prompts ) si.prompt_names.push_back(p.name);
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
            for ( int i = 0; i < 20; ++i ) {
                if ( waitpid(s.pid, nullptr, WNOHANG) == s.pid ) { s.pid = -1; break; }
                usleep(5000);
            }
            if ( s.pid > 0 ) { kill(s.pid, SIGKILL); waitpid(s.pid, nullptr, 0); s.pid = -1; }
        }
        s.connected = false;
    }
}

} // namespace agent::mcp
