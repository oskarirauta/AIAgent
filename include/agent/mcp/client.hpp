#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <sys/types.h>
#include "json.hpp"

namespace agent::mcp {

// A tool discovered on an MCP server.
struct ToolDef {
    std::string server;       // owning server name
    std::string tool;         // raw tool name on the server
    std::string registered;   // namespaced name exposed to the model (mcp__server__tool)
    std::string description;
    JSON input_schema;
};

// Connects to MCP servers over stdio (JSON-RPC 2.0) and exposes their tools.
// v1: stdio transport, tools only, synchronous request/response.
class Client {
public:
    ~Client();

    // Parse a config file ({"mcpServers": { name: {command,args,env} }}). Returns
    // the number of servers configured, or 0 if the file is absent/empty.
    int load_config(const std::string& path);

    // Spawn every configured server, run the initialize handshake and tools/list.
    // Failures are recorded per-server (see status()), never thrown.
    void connect_all();

    // All tools discovered across connected servers.
    std::vector<ToolDef> tools() const;

    // Call a tool on a server; returns its text result, or "error: ..." on failure.
    std::string call_tool(const std::string& server, const std::string& tool, const JSON& args);

    struct ServerInfo {
        std::string name;
        bool connected = false;
        std::string error;
        std::string command;
        std::vector<std::string> tool_names;
    };
    std::vector<ServerInfo> status() const;

    bool any_configured() const { return !_servers.empty(); }

    // Kill every server subprocess and close its pipes. Safe to call twice.
    void shutdown();

private:
    struct Server {
        std::string name;
        std::string command;
        std::vector<std::string> args;
        std::map<std::string, std::string> env;
        pid_t pid = -1;
        int in_fd = -1;   // we write here (server's stdin)
        int out_fd = -1;  // we read here (server's stdout)
        bool connected = false;
        std::string error;
        std::vector<ToolDef> tools;
        std::string rbuf;   // partial-line read buffer
        int next_id = 1;
        std::mutex mx;      // serialise requests on this server
    };

    std::vector<std::unique_ptr<Server>> _servers;

    bool spawn(Server& s);
    void handshake(Server& s);           // initialize + initialized + tools/list
    JSON request(Server& s, const std::string& method, const JSON& params, int timeout_ms);
    void notify(Server& s, const std::string& method, const JSON& params);
    bool read_line(Server& s, std::string& line, int timeout_ms);
    bool write_all(Server& s, const std::string& data);
};

} // namespace agent::mcp
