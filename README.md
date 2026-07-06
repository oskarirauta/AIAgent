[![License:MIT](https://img.shields.io/badge/License-MIT-blue?style=plastic)](LICENSE) [![CI build](https://img.shields.io/github/actions/workflow/status/oskarirauta/AIAgent/build.yml?style=plastic&label=build)](https://github.com/oskarirauta/AIAgent/actions/workflows/build.yml)

# AI Agent

C++17 Linux CLI AI assistant — a local, minimal but capable alternative to tools like Kimi Code, Claude Code, or OpenCode.

Philosophy: **Support enough — not everything.**

## Features

- Chat with LLMs from the command line
- Providers: OpenAI, Ollama, Anthropic, Moonshot, and native **Kimi** and **Claude** (subscription) providers
- **Kimi** and **Claude** authenticate against the same subscriptions the official CLIs use — no API-key billing and no separate app to install
- Built-in tools the model can call: `read_file`, `write_file`, `run_command`, `list_directory`, `grep`
- **Inline REPL** that prints to the terminal's normal buffer, so native scrollback and mouse copy work across the whole conversation
- Streaming responses with lightweight syntax highlighting
- Layered **tool-call safety**: per-call confirmation with *once / this-session / all-similar* choices, a **danger list** that warns on risky shell commands, and an `--insecure` escape hatch
- Remembers the last provider/model and resumes them on a bare launch
- Per-provider **and** per-project conversation history; per-provider long-term memory
- Full logging to a file (the terminal stays clean)
- Config file for defaults; everything overridable on the command line

## Build

Requirements:
- C++17 compiler
- libcurl development files
- `signal`, `process`, and the `common/*` submodules initialized
- Make

```bash
git submodule update --init --recursive   # if submodules are empty
make
```

The interactive UI is built on raw ANSI/termios — there is **no ncurses dependency**; the binary links only against libcurl.

## Running tests

```bash
make test
```

## Configuration

Create `~/.config/ai-agent/config`:

```text
provider: kimi
# model:   left unset -> a provider-appropriate default is used
log_level: info
tools_enabled: true

# Per-provider options
provider.kimi.model: kimi-for-coding
provider.kimi.thinking: on          # off | on | low | medium | high | xhigh | max
provider.claude.model: claude-opus-4-8

# For plain API-key providers (openai / anthropic / moonshot)
# api_url: https://api.openai.com/v1
# api_key: sk-your-key-here
```

If `model` is not set (via config or `-m`), each provider falls back to a sensible default (e.g. `claude-opus-4-8`, `kimi-for-coding`, `gpt-4o-mini`, `llama3`).

### Data directory

Everything the agent persists lives under `home_dir` (default `~/.local/share/ai-agent`):

```text
credentials/<provider>.json          # OAuth tokens (mode 0600)
conversations/<provider>/<cwd>.json  # history, per provider AND per project directory
memories/<provider>/                 # long-term memory, per provider (shared across models)
state.json                           # last-used provider + per-provider model
logs/agent.log                       # full log
device_id                            # stable Kimi device id
```

Conversation history and memories are **not** tied to the model, so switching e.g. Opus → Fable keeps the same context.

Long-term memory is just text/markdown files:

```text
~/.local/share/ai-agent/memories/claude/preferences.md
```

## Kimi provider

The native Kimi provider logs in with the same device-code OAuth flow and device identity headers as the official Kimi Code CLI, so it uses your Kimi **subscription** (endpoint `https://api.kimi.com/coding/v1`, model `kimi-for-coding`).

```bash
./agent -p kimi
```

On first run it prints an activation URL (e.g. `https://www.kimi.com/code/authorize_device?...`) and a user code — open the URL in a browser and approve. The token is saved and refreshed automatically. The model runs with thinking enabled by default (configurable via `provider.kimi.thinking`).

## Claude provider

The native Claude provider uses the Claude Code OAuth flow (authorization-code + PKCE). It authenticates against your Claude **subscription** and sends the OAuth access token directly as a bearer credential — it does **not** create an Anthropic API key, so it never bills pay-as-you-go API credits.

```bash
./agent -p claude
```

On first run it prints an authorization URL. Open it, log in, and paste the code shown on the confirmation page back into the terminal. The token is saved to `credentials/claude.json` and refreshed automatically. Default model: `claude-opus-4-8`.

Neither provider opens a browser for you — the URL is printed so you can open it yourself (handy over SSH / on headless machines). Use `--login` to force a fresh login (e.g. to switch accounts).

## Usage

```bash
# Interactive REPL (resumes your last provider + model)
./agent

# Pick a provider / model
./agent -p kimi
./agent -p claude -m claude-opus-4-8

# Single prompt, then exit
./agent "explain this code"

# Plain API-key providers
./agent -p openai   -k $OPENAI_API_KEY   -m gpt-4o "write a bash script"
./agent -p anthropic -k $ANTHROPIC_API_KEY -m claude-3-5-sonnet-latest
./agent -p ollama   -u http://localhost:11434 -m llama3.1

# Force a fresh login for the selected provider
./agent -p claude --login

# Tool-call modes
./agent --no-tools "just chat"     # tools disabled
./agent --yes-tools "do the work"  # run ordinary tools without asking (danger list still warns)
./agent --insecure  "do the work"  # run everything without asking (use with care)
```

If you launch without `-p`, the last provider is reused; without `-m`, that provider's last-used model (or its default) is used; passing only `-m` keeps the current provider.

### Interactive REPL

- Transcript is printed to the terminal's normal buffer — scroll and select/copy with your terminal/mouse as usual.
- A dim separator divides the transcript from the input; a status line shows provider · model · directory · tool mode.
- Line editing: Left/Right, Home/End (or Ctrl-A / Ctrl-E), Up/Down for history, Backspace/Delete.
- **Enter** sends; **Alt+Enter** inserts a newline for multi-line prompts (shown inline as a `↵` glyph).
- **Paste** (bracketed): small pastes are inserted inline; large ones collapse into an atomic `[paste #N: L lines]` box in the input, and expand into a framed block in the transcript. Boxes and newline glyphs behave as single units for cursor movement and deletion.
- Messages are marked so speakers are easy to tell apart: `›` (you), `●` (AI), `⚙` (a command).
- **Ctrl-C** interrupts the current turn (or quits when idle); **Ctrl-D** or `/exit` / `/quit` leaves.

Slash commands run locally (never sent to the model):

| Command | Effect |
|---------|--------|
| `/help` | List the commands. |
| `/about` | App description, version and current provider/model (alias `/info`). |
| `/settings` | Open the interactive settings menu (↑/↓ select, ←/→ change, Enter edit/apply, Esc close). |
| `/settings <key> <value>` | Set a value directly: `context` (e.g. `64K`, `0` = unlimited), `model`, `tools`, `strict`, `thinking`. |
| `/model [name]` | Show or change the active model. |
| `/tools <confirm\|auto\|insecure>` | Change the tool confirmation mode. |
| `/thinking <on\|off\|low\|medium\|high\|xhigh\|max>` | Set the thinking level (alias `/effort`; applied by Kimi). |
| `/theme <dark\|light\|warm>` | Switch the colour theme. |
| `/memories [name]` | List this provider's memory files, or view one. |
| `/context` | Visual context usage: a composition bar plus system / conversation / memory token estimates and the limit. |
| `/history` | List the messages in the current context. |
| `/retry` | Re-run your last message. |
| `/undo` | Remove the last exchange from history. |
| `/clear` | Clear the conversation history (context). |
| `/exit`, `/quit` | Leave. |

### Themes

Three eye-comfort colour themes tuned with muted 256-colour tones (never setting
the background, so the terminal's own background is respected): `dark` (default,
for dark terminals), `light` (for light terminals), and `warm` (low-blue, easier
on tired eyes over long evening sessions). Set with `theme: <name>` in the config
file or switch live with `/theme`.

## Security

Tool calling has three modes:

| Mode | Flag | Behaviour |
|------|------|-----------|
| confirm (default) | — | Every confirmation-requiring tool asks first, in the conversation, showing the full command. Choose **[y]** once, **[s]** this session, **[a]** all similar (same program), or **[n]** deny. |
| automatic | `--yes-tools` | Ordinary tools run without asking, but **danger-listed** shell commands still prompt with a warning. |
| insecure | `--insecure` | Everything runs without asking. |

`--no-tools` disables tool calls entirely. Non-interactive runs (single-prompt / piped) fail safe: a tool needing confirmation is denied unless `--yes-tools` / `--insecure` is set.

The **danger list** flags risky shell commands (e.g. `rm -rf`, `sudo`, `passwd`, `dd`, `mkfs`, piping downloads into a shell, or touching sensitive paths like `/etc/passwd` and block devices) and warns before running them. Rules match a program plus optional criteria, so a plain `rm file` is a normal confirmation while `rm -rf …` is flagged.

The **safe list** goes the other way: in confirm mode, known read-only commands run *without* asking — `ls`, `pwd`, `cat` (no redirection), `date`, `df`, `git status`/`log`/`diff`, `gcc -v`, `make -n`, `pkg-config`, any `… --version`, and similar. This keeps everyday commands friction-free while still gating anything that writes, executes, or mutates. Turn it off with `strict: true` in config or the `/strict on` command, which requires confirmation for those too.

## Project layout

```text
src/
  main.cpp              # CLI entry point, provider/model resolution, logging setup
  config.cpp/hpp        # Config file, CLI, defaults, last-used state
  conversation.cpp/hpp  # Message history (per provider + project)
  memory.cpp/hpp        # Per-provider long-term memory
  repl.cpp/hpp          # Turn loop; TTY vs plain dispatch
  repl_inline.cpp/hpp   # Inline (ANSI/termios) REPL renderer + line editor
  syntax_highlighter.*  # Fenced-code / markdown highlighting
  api/client.cpp/hpp    # libcurl HTTP client
  auth/                 # OAuth flows + token storage (Kimi device-code, Claude auth-code)
  providers/            # OpenAI, Ollama, Anthropic, Moonshot, Kimi, Claude adapters
  tools/                # Built-in tools + confirmation/danger-list policy (registry)
```

## Development

```bash
make clean && make
./agent --help
```

Keep this README up to date when adding features or changing behaviour.

## License

MIT
