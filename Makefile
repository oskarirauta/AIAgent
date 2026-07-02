all: world

CXX?=g++
CXXFLAGS?=--std=c++17 -Wall -fPIC -I./include -I./src
LDFLAGS?=-L/usr/lib

OBJS:= \
	objs/main.o \
	objs/config.o \
	objs/conversation.o \
	objs/repl.o \
	objs/repl_ncurses.o \
	objs/signal_handler.o \
	objs/api_client.o \
	objs/provider.o \
	objs/openai.o \
	objs/ollama.o \
	objs/tools_registry.o \
	objs/tools_read_file.o \
	objs/tools_write_file.o \
	objs/tools_run_command.o \
	objs/tools_list_directory.o \
	objs/tools_grep.o

USAGECPP_DIR:=usage
COMMON_DIR:=common
LOGGER_DIR:=logger
THROWS_DIR:=throws
JSON_DIR:=json
SIGNAL_DIR:=signal
PROCESS_DIR:=process

include $(USAGECPP_DIR)/Makefile.inc
include $(COMMON_DIR)/Makefile.inc
include $(LOGGER_DIR)/Makefile.inc
include $(THROWS_DIR)/Makefile.inc
include $(JSON_DIR)/Makefile.inc
include $(SIGNAL_DIR)/Makefile.inc
include $(PROCESS_DIR)/Makefile.inc

LDFLAGS += -lcurl -lncurses

world: agent

$(shell mkdir -p objs)

objs/main.o: src/main.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/config.o: src/config.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/conversation.o: src/conversation.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/repl.o: src/repl.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/repl_ncurses.o: src/repl_ncurses.cpp
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
objs/tools_registry.o: src/tools/registry.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_read_file.o: src/tools/read_file.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_write_file.o: src/tools/write_file.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_run_command.o: src/tools/run_command.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_list_directory.o: src/tools/list_directory.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
objs/tools_grep.o: src/tools/grep.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

agent: $(USAGE_OBJS) $(COMMON_OBJS) $(LOGGER_OBJS) $(JSON_OBJS) $(THROWS_OBJS) $(SIGNAL_OBJS) $(PROCESS_OBJS) $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@;

.PHONY: clean
clean:
	@rm -rf objs agent
