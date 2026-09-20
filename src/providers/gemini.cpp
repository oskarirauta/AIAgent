#include "agent/providers/gemini.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <mutex>

#include "agent/text_utils.hpp"
#include "common.hpp"
#include "logger.hpp"
#include "throws.hpp"

namespace agent::providers {

namespace {

long json_long(const JSON& v) {
    if ( v == JSON::TYPE::INT ) return static_cast<long>(static_cast<long long>(v));
    if ( v == JSON::TYPE::FLOAT ) return static_cast<long>(static_cast<long double>(v));
    return 0;
}

std::string get_env_var(const char* name) {
    const char* val = std::getenv(name);
    return (val && *val) ? std::string(val) : "";
}

} // namespace

Gemini::Gemini(const Config& cfg) : Provider(cfg) {
    if ( _config.api_url.empty() || _config.api_url == "https://api.openai.com/v1" ) {
        _config.api_url = "https://generativelanguage.googleapis.com";
    }

    // If no API key was explicitly passed in config, check environment
    if ( _config.api_key.empty() ) {
        std::string env_key = get_env_var("GEMINI_API_KEY");
        if ( env_key.empty() )
            env_key = get_env_var("GOOGLE_API_KEY");
        if ( !env_key.empty() )
            _config.api_key = env_key;
    }

    // Try loading OAuth subscription token (from ~/.gemini/oauth_creds.json or agent credentials)
    _token = auth::load_gemini_token(_config.home_dir);

    // If still no API key, check saved credentials in _token
    if ( _config.api_key.empty() && _token && !_token->api_key.empty() ) {
        _config.api_key = _token->api_key;
    }

    // Project ID from environment or ~/.gemini/projects.json
    _project_id = get_env_var("GOOGLE_CLOUD_PROJECT");
    if ( _project_id.empty() )
        _project_id = get_env_var("GOOGLE_CLOUD_PROJECT_ID");
    if ( _project_id.empty() ) {
        const char* home = std::getenv("HOME");
        if ( home && *home ) {
            std::string proj_file = std::string(home) + "/.gemini/projects.json";
            std::ifstream f(proj_file);
            if ( f.is_open() ) {
                std::stringstream ss;
                ss << f.rdbuf();
                try {
                    JSON pj = JSON::parse(ss.str());
                    if ( pj.contains("projects") && pj["projects"] == JSON::TYPE::OBJECT ) {
                        const auto& proj_map = std::get<JSON::Object>(pj["projects"]);
                        for ( const auto& [k, v] : proj_map ) {
                            if ( v == JSON::TYPE::STRING ) {
                                _project_id = v.to_string();
                                break;
                            }
                        }
                    }
                } catch ( ... ) {}
            }
        }
    }

    if ( !_config.thinking.empty() )
        apply_provider_options(JSON::Object{ { "thinking", _config.thinking } });
}

std::string Gemini::endpoint() const {
    return build_endpoint("/v1beta/models/" + request_model() + ":generateContent");
}

std::string Gemini::stream_endpoint() const {
    return build_endpoint("/v1beta/models/" + request_model() + ":streamGenerateContent?alt=sse");
}

std::vector<std::string> Gemini::list_models(api::Client& client) {
    std::string url = build_endpoint("/v1beta/models");
    std::string resp_str;
    try {
        std::vector<std::pair<std::string, std::string>> headers = extra_headers();
        if ( !auth_header().empty() && !auth_value().empty() )
            headers.push_back({ auth_header(), auth_value() });
        resp_str = client.get(url, headers);
    } catch ( ... ) {
        return {};
    }
    if ( resp_str.empty() ) return {};
    try {
        JSON j = JSON::parse(resp_str);
        if ( !j.contains("models") || j["models"] != JSON::TYPE::ARRAY ) return {};
        std::vector<std::string> out;
        for ( size_t i = 0; i < j["models"].size(); ++i ) {
            const JSON& m = j["models"][i];
            if ( m.contains("supportedGenerationMethods") && m["supportedGenerationMethods"] == JSON::TYPE::ARRAY ) {
                bool can_generate = false;
                for ( size_t k = 0; k < m["supportedGenerationMethods"].size(); ++k ) {
                    if ( m["supportedGenerationMethods"][k].to_string() == "generateContent" ) {
                        can_generate = true;
                        break;
                    }
                }
                if ( !can_generate ) continue;
            }
            if ( m.contains("name") && m["name"] == JSON::TYPE::STRING ) {
                std::string n = m["name"].to_string();
                if ( n.rfind("models/", 0) == 0 ) n = n.substr(7);
                out.push_back(n);
            }
        }
        return out;
    } catch ( ... ) {
        return {};
    }
}

std::string Gemini::auth_header() const {
    if ( !_config.api_key.empty() )
        return "x-goog-api-key";
    if ( _token && !_token->access_token.empty() )
        return "Authorization";
    return "";
}

std::string Gemini::auth_value() const {
    if ( !_config.api_key.empty() )
        return _config.api_key;
    if ( _token && !_token->access_token.empty() )
        return "Bearer " + _token->access_token;
    return "";
}

std::vector<std::pair<std::string, std::string>> Gemini::extra_headers() const {
    std::vector<std::pair<std::string, std::string>> h;
    if ( _config.api_key.empty() && !_project_id.empty() )
        h.push_back({ "x-goog-user-project", _project_id });
    return h;
}

bool Gemini::refresh_now(api::Client& client) {
    static std::mutex refresh_mx;
    std::lock_guard<std::mutex> lock(refresh_mx);
    auto tok = auth::load_gemini_token(_config.home_dir);
    if ( !tok || tok->refresh_token.empty() ) return false;
    if ( !auth::gemini_token_needs_refresh(*tok) ) {
        _token = tok;
        return true;
    }
    try {
        _token = auth::refresh_gemini_token(client, *tok);
        return _token.has_value() && !_token->access_token.empty();
    } catch ( const std::exception& e ) {
        logger::warning["gemini"] << "silent token refresh failed: " << e.what() << std::endl;
        return false;
    }
}

void Gemini::prepare_request(api::Client& client) {
    if ( _config.api_key.empty() && _token && auth::gemini_token_needs_refresh(*_token) ) {
        refresh_now(client);
    }
}

bool Gemini::authenticate(api::Client& client, bool force) {
    if ( !_config.api_key.empty() && !force )
        return true;

    if ( !force ) {
        _token = auth::load_gemini_token(_config.home_dir);
        if ( _token && !_token->api_key.empty() ) {
            _config.api_key = _token->api_key;
            return true;
        }
        if ( _token && !_token->access_token.empty() ) {
            if ( _token->scope.find("generative-language") != std::string::npos ) {
                if ( auth::gemini_token_needs_refresh(*_token) )
                    return refresh_now(client);
                return true;
            }
        }
    }

    // If force (e.g. --login), prompt interactively for API key if TTY
    if ( isatty(STDIN_FILENO) ) {
        std::cout << "\nTo use Google Gemini, get a free API key from Google AI Studio:\n"
                  << "  https://aistudio.google.com/apikey\n\n";
        if ( !_config.api_key.empty() ) {
            std::cout << "Current API key: " << _config.api_key.substr(0, 6) << "..." << "\n"
                      << "Enter new Gemini API key (or press Enter to keep current): ";
        } else {
            std::cout << "Enter Gemini API key: ";
        }
        std::cout.flush();

        std::string input;
        std::getline(std::cin, input);
        input = common::trim_ws(input);
        if ( input.empty() && !_config.api_key.empty() ) {
            return true;
        }
        if ( !input.empty() ) {
            _config.api_key = input;
            auth::save_gemini_api_key(_config.home_dir, _config.api_key);
            _token = auth::load_gemini_token(_config.home_dir);
            return true;
        }
    }

    if ( _config.api_key.empty() ) {
        logger::error["gemini"] << "Gemini API requires an API key (Google One / Gemini subscription does not include API access).\n"
                                << "You can get a free Gemini API key at: https://aistudio.google.com/apikey\n"
                                << "Then pass -k <key>, set `api_key:` in config, or export GEMINI_API_KEY." << std::endl;
        return false;
    }
    return true;
}

bool Gemini::ready_noninteractive(api::Client& client) {
    if ( !_config.api_key.empty() )
        return true;
    _token = auth::load_gemini_token(_config.home_dir);
    if ( _token && !_token->api_key.empty() ) {
        _config.api_key = _token->api_key;
        return true;
    }
    if ( !_token || _token->access_token.empty() )
        return false;
    if ( _token->scope.find("generative-language") == std::string::npos )
        return false;
    if ( auth::gemini_token_needs_refresh(*_token) )
        return refresh_now(client);
    return true;
}

bool Gemini::reauthenticate(api::Client& client) {
    return refresh_now(client);
}

void Gemini::prepare_stream_request(JSON& req) {
    if ( req.contains("stream") )
        req.erase("stream");
}

long Gemini::output_cap_for(const std::string& model) {
    std::string m = common::to_lower(Config::base_model_name(model));
    if ( m.find("3.") != std::string::npos ) return 65536;
    if ( m.find("2.5") != std::string::npos ) return 65536;
    if ( m.find("2.0") != std::string::npos ) return 65536;
    if ( m.find("1.5-pro") != std::string::npos ) return 65536;
    return 32768;
}

long Gemini::thinking_budget_for(const std::string& effort, const std::string& /*model*/) {
    if ( effort == "off" || effort == "none" || effort == "0" ) return 0;
    if ( effort == "low" ) return 1024;
    if ( effort == "medium" ) return 2048;
    if ( effort == "high" ) return 8192;
    if ( effort == "xhigh" || effort == "max" ) return 16384;
    if ( effort == "auto" || effort == "dynamic" ) return -1;
    // If effort is a number, parse it
    try {
        long val = std::stol(effort);
        return val;
    } catch ( ... ) {}
    return 2048;
}

void Gemini::apply_provider_options(const JSON& options) {
    if ( options != JSON::TYPE::OBJECT ) return;
    if ( options.contains("model") && options["model"] == JSON::TYPE::STRING )
        _config.model = options["model"].to_string();
    if ( options.contains("project_id") && options["project_id"] == JSON::TYPE::STRING )
        _project_id = options["project_id"].to_string();

    if ( options.contains("thinking") ) {
        std::string v;
        if ( options["thinking"] == JSON::TYPE::STRING )
            v = options["thinking"].to_string();
        else if ( options["thinking"] == JSON::TYPE::INT )
            v = std::to_string(static_cast<long long>(options["thinking"]));
        else if ( options["thinking"] == JSON::TYPE::BOOL )
            v = static_cast<bool>(options["thinking"]) ? "on" : "off";

        std::string lower = common::to_lower(common::trim_ws(v));
        if ( lower == "off" || lower == "false" || lower == "disabled" || lower == "0" ) {
            _thinking_enabled = false;
            _thinking_budget = 0;
            _thinking_effort = "off";
        } else if ( lower == "on" || lower == "true" || lower == "enabled" || lower == "1" || lower.empty() ) {
            _thinking_enabled = true;
            _thinking_budget = 2048;
            _thinking_effort = "medium";
        } else {
            _thinking_enabled = true;
            _thinking_effort = lower;
            _thinking_budget = thinking_budget_for(lower, _config.model);
        }
    }
}

JSON Gemini::message_to_parts(const Message& msg) const {
    JSON parts = JSON::Array{};

    if ( msg.role == agent::Role::TOOL ) {
        std::string fn_name = msg.name.value_or("tool");
        JSON fn_resp = JSON::Object{
            { "name", fn_name },
            { "response", JSON::Object{ { "output", msg.content } } }
        };
        parts.append(JSON::Object{ { "functionResponse", fn_resp } });
        return parts;
    }

    if ( !msg.content.empty() ) {
        parts.append(JSON::Object{ { "text", msg.content } });
    }

    if ( msg.role == agent::Role::ASSISTANT ) {
        for ( const auto& tc : msg.tool_calls ) {
            JSON args = JSON::Object{};
            try {
                if ( !tc.arguments.empty() )
                    args = JSON::parse(tc.arguments);
            } catch ( ... ) {
                args = JSON::Object{};
            }
            parts.append(JSON::Object{
                { "functionCall", JSON::Object{
                    { "name", tc.name },
                    { "args", args }
                }}
            });
        }
    }

    return parts;
}

JSON Gemini::build_request(const Conversation& conv, const JSON& tools_schema) {
    JSON req = JSON::Object{};

    std::string system_text;
    JSON contents = JSON::Array{};

    auto msgs = request_messages(conv);

    for ( const auto& msg : msgs ) {
        if ( msg.role == agent::Role::SYSTEM ) {
            if ( !system_text.empty() ) system_text += "\n\n";
            system_text += msg.content;
            continue;
        }

        std::string role_str = (msg.role == agent::Role::ASSISTANT) ? "model" : "user";
        JSON msg_parts = message_to_parts(msg);
        if ( msg_parts.empty() ) continue;

        // Gemini enforces role alternation: combine consecutive messages with same role
        if ( contents.size() > 0 && contents[contents.size() - 1]["role"].to_string() == role_str ) {
            JSON& last_parts = contents[contents.size() - 1]["parts"];
            for ( size_t i = 0; i < msg_parts.size(); ++i )
                last_parts.append(msg_parts[i]);
        } else {
            contents.append(JSON::Object{
                { "role", role_str },
                { "parts", msg_parts }
            });
        }
    }

    if ( system_text.empty() && !_config.system_prompt.empty() )
        system_text = _config.system_prompt;

    if ( !system_text.empty() ) {
        req["systemInstruction"] = JSON::Object{
            { "parts", JSON::Array{ JSON::Object{ { "text", system_text } } } }
        };
    }

    // Ensure at least one content entry
    if ( contents.empty() ) {
        contents.append(JSON::Object{
            { "role", "user" },
            { "parts", JSON::Array{ JSON::Object{ { "text", "Hello" } } } }
        });
    }
    req["contents"] = contents;

    // Convert tools schema to Gemini functionDeclarations
    if ( tools_schema == JSON::TYPE::ARRAY && !tools_schema.empty() ) {
        JSON decls = JSON::Array{};
        for ( size_t i = 0; i < tools_schema.size(); ++i ) {
            const JSON& item = tools_schema[i];
            if ( item.contains("function") && item["function"] == JSON::TYPE::OBJECT ) {
                const JSON& fn = item["function"];
                JSON decl = JSON::Object{
                    { "name", fn.contains("name") ? fn["name"].to_string() : "" },
                    { "description", fn.contains("description") ? fn["description"].to_string() : "" }
                };
                if ( fn.contains("parameters") )
                    decl["parameters"] = fn["parameters"];
                decls.append(decl);
            }
        }
        if ( !decls.empty() ) {
            req["tools"] = JSON::Array{
                JSON::Object{ { "functionDeclarations", decls } }
            };
        }
    }

    // generationConfig
    JSON gen_cfg = JSON::Object{
        { "temperature", 0.7 }
    };
    if ( _config.max_tokens > 0 ) {
        long cap = output_cap_for(_config.model);
        long max_out = static_cast<long>(std::min<size_t>(_config.max_tokens, static_cast<size_t>(cap)));
        gen_cfg["maxOutputTokens"] = max_out;
    }

    if ( _thinking_enabled ) {
        JSON thinking_cfg = JSON::Object{};
        if ( _thinking_budget >= 0 )
            thinking_cfg["thinkingBudget"] = static_cast<long long>(_thinking_budget);
        gen_cfg["thinkingConfig"] = thinking_cfg;
    } else {
        gen_cfg["thinkingConfig"] = JSON::Object{ { "thinkingBudget", 0 } };
    }
    req["generationConfig"] = gen_cfg;

    return req;
}

Response Gemini::parse_response(const JSON& response) {
    Response out;

    if ( response.contains("error") && response["error"] == JSON::TYPE::OBJECT ) {
        out.success = false;
        out.message = response["error"].contains("message")
            ? response["error"]["message"].to_string() : response["error"].dump();
        return out;
    }

    if ( !response.contains("candidates") || response["candidates"] != JSON::TYPE::ARRAY || response["candidates"].empty() ) {
        out.success = false;
        out.message = "no candidates returned in Gemini response";
        return out;
    }

    const JSON& cand = response["candidates"][0];
    if ( cand.contains("finishReason") && cand["finishReason"] == JSON::TYPE::STRING ) {
        std::string fr = cand["finishReason"].to_string();
        if ( fr == "MAX_TOKENS" )
            out.truncated = true;
    }

    if ( cand.contains("content") && cand["content"] == JSON::TYPE::OBJECT &&
         cand["content"].contains("parts") && cand["content"]["parts"] == JSON::TYPE::ARRAY ) {
        const JSON& parts = cand["content"]["parts"];
        for ( size_t i = 0; i < parts.size(); ++i ) {
            const JSON& part = parts[i];
            bool is_thought = part.contains("thought") && part["thought"] == JSON::TYPE::BOOL && static_cast<bool>(part["thought"]);
            if ( is_thought && part.contains("text") && part["text"] == JSON::TYPE::STRING ) {
                out.thinking += part["text"].to_string();
            } else if ( part.contains("text") && part["text"] == JSON::TYPE::STRING ) {
                out.message += part["text"].to_string();
            } else if ( part.contains("functionCall") && part["functionCall"] == JSON::TYPE::OBJECT ) {
                const JSON& fc = part["functionCall"];
                ToolCall tc;
                tc.name = fc.contains("name") ? fc["name"].to_string() : "";
                tc.arguments = fc.contains("args") ? fc["args"] : JSON::Object{};
                tc.id = "call_" + tc.name + "_" + std::to_string(out.tool_calls.size() + 1);
                out.tool_calls.push_back(tc);
            }
        }
    }

    if ( response.contains("usageMetadata") && response["usageMetadata"] == JSON::TYPE::OBJECT ) {
        const JSON& u = response["usageMetadata"];
        if ( u.contains("promptTokenCount") ) out.input_tokens = json_long(u["promptTokenCount"]);
        if ( u.contains("candidatesTokenCount") ) out.output_tokens = json_long(u["candidatesTokenCount"]);
        if ( u.contains("cachedContentTokenCount") ) out.cached_input_tokens = json_long(u["cachedContentTokenCount"]);
        if ( u.contains("thoughtsTokenCount") ) out.reasoning_tokens = json_long(u["thoughtsTokenCount"]);
    }
    if ( out.reasoning_tokens == 0 && !out.thinking.empty() )
        out.reasoning_tokens = static_cast<long>(out.thinking.size() / 4);

    return out;
}

JSON Gemini::make_tool_result(const std::string& tool_call_id, const std::string& result) {
    return JSON::Object{
        { "role", "tool" },
        { "tool_call_id", tool_call_id },
        { "content", result }
    };
}

void Gemini::stream_reset() {
    _s_content.clear();
    _s_reasoning.clear();
    _s_tools.clear();
    _s_input_tokens = 0;
    _s_output_tokens = 0;
    _s_cached_tokens = 0;
    _s_reasoning_tokens = 0;
    _s_truncated = false;
    _s_success = true;
    _s_error.clear();
}

StreamChunk Gemini::parse_stream(const std::string& chunk, std::string& buffer, bool& done) {
    buffer += chunk;
    StreamChunk sc;

    size_t start = 0;
    size_t pos = 0;
    while ( (pos = buffer.find('\n', start)) != std::string::npos ) {
        std::string line = buffer.substr(start, pos - start);
        start = pos + 1;

        line = common::trim_ws(line);
        if ( line.empty() || line[0] == ':' ) continue;

        if ( line.rfind("data:", 0) == 0 ) {
            std::string data = common::trim_ws(line.substr(5));
            if ( data == "[DONE]" ) {
                done = true;
                continue;
            }
            try {
                JSON j = JSON::parse(data);
                if ( j.contains("error") ) {
                    _s_success = false;
                    _s_error = j["error"].contains("message") ? j["error"]["message"].to_string() : data;
                    continue;
                }

                if ( j.contains("usageMetadata") && j["usageMetadata"] == JSON::TYPE::OBJECT ) {
                    const JSON& u = j["usageMetadata"];
                    if ( u.contains("promptTokenCount") ) _s_input_tokens = json_long(u["promptTokenCount"]);
                    if ( u.contains("candidatesTokenCount") ) _s_output_tokens = json_long(u["candidatesTokenCount"]);
                    if ( u.contains("cachedContentTokenCount") ) _s_cached_tokens = json_long(u["cachedContentTokenCount"]);
                    if ( u.contains("thoughtsTokenCount") ) _s_reasoning_tokens = json_long(u["thoughtsTokenCount"]);
                }

                if ( j.contains("candidates") && j["candidates"] == JSON::TYPE::ARRAY && !j["candidates"].empty() ) {
                    const JSON& cand = j["candidates"][0];
                    if ( cand.contains("finishReason") && cand["finishReason"] == JSON::TYPE::STRING ) {
                        std::string fr = cand["finishReason"].to_string();
                        if ( fr == "MAX_TOKENS" ) _s_truncated = true;
                        if ( fr == "STOP" ) done = true;
                    }

                    if ( cand.contains("content") && cand["content"] == JSON::TYPE::OBJECT &&
                         cand["content"].contains("parts") && cand["content"]["parts"] == JSON::TYPE::ARRAY ) {
                        const JSON& parts = cand["content"]["parts"];
                        for ( size_t i = 0; i < parts.size(); ++i ) {
                            const JSON& part = parts[i];
                            bool is_thought = part.contains("thought") && part["thought"] == JSON::TYPE::BOOL && static_cast<bool>(part["thought"]);
                            if ( is_thought && part.contains("text") && part["text"] == JSON::TYPE::STRING ) {
                                std::string delta = part["text"].to_string();
                                _s_reasoning += delta;
                                sc.reasoning += delta;
                            } else if ( part.contains("text") && part["text"] == JSON::TYPE::STRING ) {
                                std::string delta = part["text"].to_string();
                                _s_content += delta;
                                sc.content += delta;
                            } else if ( part.contains("functionCall") && part["functionCall"] == JSON::TYPE::OBJECT ) {
                                const JSON& fc = part["functionCall"];
                                ToolCall tc;
                                tc.name = fc.contains("name") ? fc["name"].to_string() : "";
                                tc.arguments = fc.contains("args") ? fc["args"] : JSON::Object{};
                                tc.id = "call_" + tc.name + "_" + std::to_string(_s_tools.size() + 1);
                                _s_tools.push_back(tc);
                            }
                        }
                    }
                }
            } catch ( ... ) {
                // Ignore malformed intermediate chunk
            }
        }
    }
    if ( start > 0 )
        buffer.erase(0, start);

    return sc;
}

Response Gemini::stream_result() {
    Response out;
    out.success = _s_success;
    if ( !_s_success ) {
        out.message = _s_error.empty() ? "Gemini stream failed" : _s_error;
        return out;
    }
    out.message = _s_content;
    out.thinking = _s_reasoning;
    out.tool_calls = _s_tools;
    out.input_tokens = _s_input_tokens;
    out.output_tokens = _s_output_tokens;
    out.cached_input_tokens = _s_cached_tokens;
    out.reasoning_tokens = _s_reasoning_tokens > 0 ? _s_reasoning_tokens
                          : static_cast<long>(_s_reasoning.size() / 4);
    out.truncated = _s_truncated;
    return out;
}

} // namespace agent::providers
