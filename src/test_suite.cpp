#include <iostream>
#include <fstream>
#include <cassert>
#include <filesystem>

#include "agent/config.hpp"
#include "agent/conversation.hpp"
#include "agent/memory.hpp"
#include "agent/providers/provider.hpp"
#include "agent/providers/openai.hpp"
#include "agent/providers/ollama.hpp"
#include "agent/providers/anthropic.hpp"
#include "agent/providers/moonshot.hpp"
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
    std::filesystem::create_directories(home + "/memories");
    std::ofstream ofd(home + "/memories/notes.md");
    ofd << "User is a developer.";
    ofd.close();

    std::string mem = agent::load_memories(home);
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
    r.set_confirm_callback([](const std::string&) { return true; });

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

int main() {
    std::cout << "Running AI Agent test suite\n" << std::endl;

    test_conversation_save_load();
    test_memory_loading();
    test_openai_request();
    test_ollama_request();
    test_anthropic_request();
    test_stream_parsers();
    test_tools();

    std::cout << "\n" << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
