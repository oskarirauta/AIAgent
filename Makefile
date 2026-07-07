all: world

CXX?=g++
CXXFLAGS?=--std=c++17 -Wall -fPIC -I./include -g
# Emit header dependency files (.d) so changing a header recompiles every source
# that includes it — mixing objects built against different struct layouts causes
# memory corruption at runtime.
CXXFLAGS+=-MMD -MP
LDFLAGS?=-L/usr/lib

# Prefer pkg-config for curl when available, fallback to plain flags. The inline
# REPL uses raw ANSI/termios, so no curses dependency is needed.
CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl)
LIBS?=$(CURL_LIBS)

OBJS:= \
	objs/main.o \
	objs/config.o \
	objs/conversation.o \
	objs/memory.o \
	objs/repl.o \
	objs/repl_inline.o \
	objs/workflow.o \
	objs/skills.o \
	objs/gitignore.o \
	objs/mcp_client.o \
	objs/syntax_highlighter.o \
	objs/text_utils.o \
	objs/signal_handler.o \
	objs/api_client.o \
	objs/provider.o \
	objs/openai.o \
	objs/ollama.o \
	objs/anthropic.o \
	objs/moonshot.o \
	objs/openrouter.o \
	objs/kimi_token.o \
	objs/kimi_oauth.o \
	objs/kimi_provider.o \
	objs/claude_oauth.o \
	objs/claude_provider.o \
	objs/tools_registry.o \
	objs/tools_read_file.o \
	objs/tools_write_file.o \
	objs/tools_edit_file.o \
	objs/tools_run_command.o \
	objs/tools_list_directory.o \
	objs/tools_grep.o \
	objs/tools_find_symbol.o \
	objs/tools_find_references.o \
	objs/tools_project_map.o \
	objs/tools_outline_file.o \
	objs/tools_web_search.o \
	objs/tools_fetch_url.o

USAGECPP_DIR:=usage
COMMON_DIR:=common
LOGGER_DIR:=logger
THROWS_DIR:=throws
JSON_DIR:=json
SIGNAL_DIR:=signal
PROCESS_DIR:=process
ENV_DIR:=env

include $(USAGECPP_DIR)/Makefile.inc
include $(COMMON_DIR)/Makefile.inc
include $(LOGGER_DIR)/Makefile.inc
include $(THROWS_DIR)/Makefile.inc
include $(JSON_DIR)/Makefile.inc
include $(SIGNAL_DIR)/Makefile.inc
include $(PROCESS_DIR)/Makefile.inc
include $(ENV_DIR)/Makefile.inc

#LDFLAGS += -lcurl -lncurses

world: agent

$(shell mkdir -p objs)

objs/main.o: src/main.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/config.o: src/config.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/conversation.o: src/conversation.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/memory.o: src/memory.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/repl.o: src/repl.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/repl_inline.o: src/repl_inline.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/syntax_highlighter.o: src/syntax_highlighter.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/text_utils.o: src/text_utils.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/signal_handler.o: src/signal_handler.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/api_client.o: src/api/client.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/provider.o: src/providers/provider.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/openai.o: src/providers/openai.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/ollama.o: src/providers/ollama.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/anthropic.o: src/providers/anthropic.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/moonshot.o: src/providers/moonshot.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/openrouter.o: src/providers/openrouter.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/kimi_token.o: src/auth/kimi_token.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/kimi_oauth.o: src/auth/kimi_oauth.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/kimi_provider.o: src/providers/kimi.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/claude_oauth.o: src/auth/claude_oauth.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/claude_provider.o: src/providers/claude.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/workflow.o: src/workflow.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/skills.o: src/skills.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/gitignore.o: src/gitignore.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/mcp_client.o: src/mcp/client.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_registry.o: src/tools/registry.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_read_file.o: src/tools/read_file.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_write_file.o: src/tools/write_file.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_edit_file.o: src/tools/edit_file.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_run_command.o: src/tools/run_command.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_list_directory.o: src/tools/list_directory.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_grep.o: src/tools/grep.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_find_symbol.o: src/tools/find_symbol.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_find_references.o: src/tools/find_references.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_project_map.o: src/tools/project_map.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_outline_file.o: src/tools/outline_file.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_web_search.o: src/tools/web_search.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_fetch_url.o: src/tools/fetch_url.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/test_suite.o: src/test_suite.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

agent: $(USAGE_OBJS) $(COMMON_OBJS) $(LOGGER_OBJS) $(JSON_OBJS) $(THROWS_OBJS) $(SIGNAL_OBJS) $(PROCESS_OBJS) $(ENV_OBJS) $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LIBS);

TEST_OBJS:= $(filter-out objs/main.o,$(OBJS)) objs/test_suite.o

test: $(USAGE_OBJS) $(COMMON_OBJS) $(LOGGER_OBJS) $(JSON_OBJS) $(THROWS_OBJS) $(SIGNAL_OBJS) $(PROCESS_OBJS) $(ENV_OBJS) $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o test_runner $(LIBS);
	./test_runner

.PHONY: clean
clean:
	@rm -rf objs agent test_runner

# Pull in the generated per-object header dependencies (see -MMD above).
-include $(wildcard objs/*.d)
