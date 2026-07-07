#include <iostream>
#include <fstream>
#include <cassert>
#include <filesystem>
#include <thread>
#include <chrono>
#include <cstdlib>

#include "agent/signal_handler.hpp"

#include "agent/config.hpp"
#include "agent/conversation.hpp"
#include "agent/repl_inline.hpp"
#include "agent/memory.hpp"
#include "agent/token_stats.hpp"
#include "agent/tools/run_command.hpp"
#include "agent/tools/read_file.hpp"
#include "agent/tools/grep.hpp"
#include "agent/tools/edit_file.hpp"
#include "agent/providers/provider.hpp"
#include "agent/providers/openai.hpp"
#include "agent/providers/ollama.hpp"
#include "agent/providers/anthropic.hpp"
#include "agent/providers/moonshot.hpp"
#include "agent/providers/openrouter.hpp"
#include "agent/providers/kimi.hpp"
#include "agent/providers/claude.hpp"
#include "agent/auth/claude_oauth.hpp"
#include "agent/tools/registry.hpp"
#include "agent/tools/list_directory.hpp"
#include "agent/tools/find_symbol.hpp"
#include "agent/tools/find_references.hpp"
#include "agent/tools/project_map.hpp"
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

    p.apply_provider_options(JSON::Object{ { "thinking", "max" } });
    req = p.build_request(c, JSON::Array{});
    check(static_cast<long>(static_cast<long long>(req["thinking"]["budget_tokens"])) == 32000 - 8192,
          "max = opus ceiling minus answer margin");

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

    {
        std::ofstream ofd(dir + "/AGENTS.md");
        ofd << "Use tabs. Run make test before finishing.\n";
    }
    check(agent::project_instructions_file(dir) == "AGENTS.md", "AGENTS.md detected");
    std::string block = agent::load_project_instructions(dir);
    check(block.find("Use tabs. Run make test") != std::string::npos, "content included");
    check(block.find("Project instructions (from AGENTS.md)") != std::string::npos, "block is labelled");

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
    auto olc = ollama.parse_stream("data: {\"message\":{\"content\":\"world\"}}\n\ndata: {\"done\":true}\n\n", buf, done);
    check(olc.content == "world" && done, "ollama stream parser");

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
    r.set_confirm_callback([](const agent::tools::ConfirmRequest&) { return agent::tools::Decision::once; });

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

static void test_context_auto() {
    std::cout << "context auto budget" << std::endl;
    check(agent::Config::context_window_for("claude-opus-4-8") == 200000, "claude window");
    check(agent::Config::context_window_for("kimi-for-coding") == 256000, "kimi window");
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

    std::string big = rc.execute(JSON::Object{{ "command", "yes aaaaaaaaaa | head -n 20000" }});
    check(big.find("truncated") != std::string::npos, "large output is truncated");

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
    check(std::getenv("AI_TEST_VAR") == nullptr, "env var is restored (unset) after the command");

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

    // Identical old/new.
    std::string id = ef.execute(JSON::Object{{ "path", path }, { "old_string", "y" }, { "new_string", "y" }, { "replace_all", true }});
    check(id.find("identical") != std::string::npos, "identical old/new refused");

    // Missing file.
    std::string mf = ef.execute(JSON::Object{{ "path", "/tmp/does_not_exist_edit.txt" }, { "old_string", "a" }, { "new_string", "b" }});
    check(mf.find("does not exist") != std::string::npos, "editing a missing file refused");

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

static void test_grep_robustness() {
    std::cout << "grep robustness" << std::endl;
    agent::tools::Grep g;
    {
        std::ofstream o("/tmp/ai_grep.txt");
        o << "alpha 1\n" << "beta 2\n" << "a.b\n" << "axb\n";
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

    std::filesystem::remove("/tmp/ai_grep.txt");
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
    test_settings_persistence();
    test_context_auto();
    test_conversation_corrupt();
    test_expand_tilde();
    test_memory_loading();
    test_memory_listing();
    test_openai_request();
    test_reasoning_content();
    test_openrouter_provider();
    test_ollama_request();
    test_anthropic_request();
    test_thinking_block_replay();
    test_prompt_caching();
    test_anthropic_role_merge();
    test_kimi_thinking_effort();
    test_anthropic_thinking();
    test_provider_capabilities();
    test_advisor_tool();
    test_mcp_tool_and_config();
    test_html_to_text();
    test_web_search_parse();
    test_find_symbol();
    test_tasks_tool();
    test_file_mentions();
    test_ultra_keyword();
    test_block_diff();
    test_project_map();
    test_find_references();
    test_pricing_and_cost();
    test_project_instructions();
    test_workflow_manager();
    test_workflow_tool();
    test_workflow_autoresume();
    test_workflow_parallel_cancel_retry();
    test_provider_options_config();
    test_claude_provider();
    test_claude_pkce();
    test_stream_parsers();
    test_stream_tool_calls();
    test_tools();
    test_run_command_robustness();
    test_read_file_robustness();
    test_parallel_tool_safety();
    test_run_command_options();
    test_edit_file();
    test_grep_robustness();
    test_token_usage();
    test_danger_list();
    test_list_directory();
    test_config_booleans();
    test_extra_command_lists();
    test_safe_commands();

    std::cout << "\n" << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
