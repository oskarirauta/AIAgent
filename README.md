# AI Agent

C++17 Linux CLI AI assistant — a local, minimal but capable alternative to tools like Kimi Code, Claude Code, or OpenCode.

Philosophy: **Support enough — not everything.**

## Features

- Chat with LLMs from the command line
- Supports OpenAI-compatible APIs and Ollama
- Built-in tools the model can call:
  - `read_file`
  - `write_file`
  - `run_command`
  - `list_directory`
  - `grep`
- Single-prompt mode and interactive REPL (ncurses UI with line editing and history)
- Graceful shutdown on SIGINT / SIGTERM
- Config file for default settings
- Debug logging levels

## Build

Requirements:
- C++17 compiler
- libcurl development files
- libncurses development files
- Make

```bash
make
```

If `common/tsl` or `common/rva` submodules are empty, initialize them first:

```bash
cd common && git submodule update --init --recursive
```

## Configuration

Create `~/.config/ai-agent/config`:

```text
provider: openai
model: gpt-4o-mini
api_url: https://api.openai.com/v1/chat/completions
api_key: sk-your-key-here
log_level: info
system_prompt: "You are a helpful Linux CLI assistant."
home_dir: "~/.local/share/ai-agent"
```

All values can be overridden with command-line options.

## Usage

```bash
# Interactive REPL
./agent

# Single prompt
./agent "explain this code"

# Use Ollama
./agent --provider ollama --model llama3.1 --api-url http://localhost:11434/api/chat

# Override config values
./agent --provider openai --api-key $OPENAI_API_KEY --model gpt-4o "write a bash script"

# Change agent home directory
./agent --home ~/.ai-agent "remember my name is Kimi"

# Debug logging
./agent --log-level debug "hello"
```

## Project layout

```text
src/
  main.cpp              # CLI entry point
  config.cpp/hpp        # Configuration loading
  conversation.cpp/hpp  # Message history
  repl.cpp/hpp          # Interactive loop
  api/client.cpp/hpp    # libcurl HTTP client
  providers/            # OpenAI and Ollama adapters
  tools/                # Built-in tools (read, write, run, list, grep)
```

## Development

This project uses Git. Make changes in focused commits and keep the README up to date when adding features or changing behavior.

```bash
make clean && make
./agent --help
```

## License

MIT
