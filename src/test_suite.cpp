#include <iostream>
#include <fstream>
#include <cassert>
#include <filesystem>
#include <thread>
#include <chrono>

#include "agent/signal_handler.hpp"

#include "agent/config.hpp"
#include "agent/conversation.hpp"
#include "agent/memory.hpp"
#include "agent/token_stats.hpp"
#include "agent/tools/run_command.hpp"
#include "agent/tools/read_file.hpp"
#include "agent/tools/grep.hpp"
#include "agent/providers/provider.hpp"
#include "agent/providers/openai.hpp"
#include "agent/providers/ollama.hpp"
#include "agent/providers/anthropic.hpp"
#include "agent/providers/moonshot.hpp"
#include "agent/providers/kimi.hpp"
#include "agent/providers/claude.hpp"
#include "agent/auth/claude_oauth.hpp"
#include "agent/tools/registry.hpp"

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

static void test_provider_capabilities() {
    std::cout << "provider capabilities" << std::endl;
    agent::Config cfg;

    agent::providers::OpenAI openai(cfg);
    check(!openai.supports("model-command"), "openai does not claim model-command");

    agent::providers::Kimi kimi(cfg);
    check(kimi.supports("model-command"), "kimi supports model-command");
    check(!kimi.supports("image-input"), "kimi does not claim image-input");
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
    std::string buf;
    bool done = false;
    std::string out = openai.parse_stream("data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\ndata: [DONE]\n\n", buf, done);
    check(out == "Hello" && done, "openai stream parser");

    agent::providers::Ollama ollama(cfg);
    buf.clear(); done = false;
    out = ollama.parse_stream("data: {\"message\":{\"content\":\"world\"}}\n\ndata: {\"done\":true}\n\n", buf, done);
    check(out == "world" && done, "ollama stream parser");

    agent::providers::Anthropic anthropic(cfg);
    buf.clear(); done = false;
    out = anthropic.parse_stream("event: content_block_delta\ndata: {\"delta\":{\"type\":\"text_delta\",\"text\":\"!\"}}\n\nevent: message_stop\ndata: {}\n\n", buf, done);
    check(out == "!" && done, "anthropic stream parser");
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

    std::filesystem::remove("/tmp/ai_rf.txt");
    std::filesystem::remove("/tmp/ai_rf.bin");
    std::filesystem::remove("/tmp/ai_rf_empty.txt");
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
}

int main() {
    std::cout << "Running AI Agent test suite\n" << std::endl;

    test_conversation_save_load();
    test_memory_loading();
    test_openai_request();
    test_ollama_request();
    test_anthropic_request();
    test_provider_capabilities();
    test_provider_options_config();
    test_claude_provider();
    test_claude_pkce();
    test_stream_parsers();
    test_tools();
    test_run_command_robustness();
    test_read_file_robustness();
    test_grep_robustness();
    test_token_usage();
    test_danger_list();

    std::cout << "\n" << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
