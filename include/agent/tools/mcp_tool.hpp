#pragma once

#include <functional>
#include <string>
#include <utility>
#include "agent/tools/tool.hpp"

namespace agent::tools {

// A proxy for a tool provided by an MCP server. It carries the server's own name,
// description and JSON-Schema, and forwards execute() to the MCP client via an
// injected handler, so the tool layer stays free of MCP internals.
class McpTool : public Tool {
public:
    using call_fn = std::function<std::string(const std::string& server,
                                              const std::string& tool, const JSON& args)>;

    McpTool(std::string registered, std::string description, JSON schema,
            std::string server, std::string tool, call_fn handler)
        : _name(std::move(registered)), _description(std::move(description)),
          _schema(std::move(schema)), _server(std::move(server)),
          _tool(std::move(tool)), _handler(std::move(handler)) {}

    std::string name() const override { return _name; }
    std::string description() const override {
        return _description.empty() ? ( "MCP tool " + _tool + " (server " + _server + ")" )
                                    : _description;
    }
    JSON parameters() const override {
        if ( _schema == JSON::TYPE::OBJECT && _schema.contains("type"))
            return _schema;
        return JSON::Object{ { "type", "object" }, { "properties", JSON::Object{} } };
    }
    std::string execute(const JSON& args) override {
        if ( !_handler )
            return "error: MCP is not available";
        return _handler(_server, _tool, args);
    }

private:
    std::string _name;
    std::string _description;
    JSON _schema;
    std::string _server;
    std::string _tool;
    call_fn _handler;
};

} // namespace agent::tools
