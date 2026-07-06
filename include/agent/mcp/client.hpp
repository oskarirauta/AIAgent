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

// A resource (readable data) exposed by a server.
struct ResourceDef {
    std::string server;
    std::string uri;
    std::string name;
    std::string description;
    std::string mime;
};

// A prompt template exposed by a server.
struct PromptDef {
    std::string server;
    std::string name;
    std::string description;
    std::vector<std::string> arguments; // argument names
};

// Connects to MCP servers (stdio or HTTP) and exposes their tools, resources and
// prompts. Synchronous request/response (JSON-RPC 2.0).
class Client {
public:
    ~Client();

    // Parse a config file ({"mcpServers": { name: {command,args,env} | {url,headers} }}).
    // Returns the number of servers configured, or 0 if the file is absent/empty.
    int load_config(const std::string& path);

    // Spawn/contact every configured server, run the initialize handshake and list
    // tools/resources/prompts. Runs the servers in parallel; failures are recorded
    // per-server (see status()), never thrown.
    void connect_all();

    // Re-list tools/resources/prompts on connected servers (manual refresh for a
    // server whose offerings changed). Returns the total tool count afterwards.
    int refresh();

    std::vector<ToolDef> tools() const;
    std::vector<ResourceDef> resources() const;
    std::vector<PromptDef> prompts() const;

    // Call a tool; returns its text result, or "error: ..." on failure.
    std::string call_tool(const std::string& server, const std::string& tool, const JSON& args);
    // Read a resource; returns its text content, or "error: ...".
    std::string read_resource(const std::string& server, const std::string& uri);
    // Render a prompt to plain text (prompts/get); returns text or "error: ...".
    std::string get_prompt(const std::string& server, const std::string& name,
                           const std::map<std::string, std::string>& args);

    struct ServerInfo {
        std::string name;
        std::string transport; // "stdio" | "http"
        bool connected = false;
        std::string error;
        std::string command;   // command or url
        std::vector<std::string> tool_names;
        std::vector<std::string> resource_uris;
        std::vector<std::string> prompt_names;
    };
    std::vector<ServerInfo> status() const;

    bool any_configured() const { return !_servers.empty(); }

    // Kill/close every server. Safe to call twice.
    void shutdown();

private:
    enum class Transport { stdio, http };

    struct Server {
        std::string name;
        Transport transport = Transport::stdio;

        // stdio
        std::string command;
        std::vector<std::string> args;
        std::map<std::string, std::string> env;
        pid_t pid = -1;
        int in_fd = -1;
        int out_fd = -1;
        std::string rbuf;

        // http
        std::string url;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string session_id;

        bool connected = false;
        std::string error;
        std::vector<ToolDef> tools;
        std::vector<ResourceDef> resources;
        std::vector<PromptDef> prompts;
        bool has_resources = false;
        bool has_prompts = false;
        int next_id = 1;
        std::mutex mx;
    };

    std::vector<std::unique_ptr<Server>> _servers;

    void connect_one(Server& s);
    bool spawn(Server& s);
    void handshake(Server& s);          // initialize + initialized
    void discover(Server& s);           // tools/list, resources/list, prompts/list
    JSON request(Server& s, const std::string& method, const JSON& params, int timeout_ms);
    void notify(Server& s, const std::string& method, const JSON& params);

    // stdio transport
    bool read_line(Server& s, std::string& line, int timeout_ms);
    bool write_all(Server& s, const std::string& data);

    // http transport
    JSON http_request(Server& s, const std::string& message, bool expect_response, int timeout_ms);

    Server* find(const std::string& name);
};

} // namespace agent::mcp
