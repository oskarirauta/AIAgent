[![License:MIT](https://img.shields.io/badge/License-MIT-blue?style=plastic)](LICENSE) [![CI build](https://img.shields.io/github/actions/workflow/status/oskarirauta/AIAgent/build.yml?style=plastic&label=build)](https://github.com/oskarirauta/AIAgent/actions/workflows/build.yml)

# AI Agent

C++17 Linux CLI AI assistant — a local, minimal but capable alternative to tools like Kimi Code, Claude Code, or OpenCode.

Philosophy: **Support enough — not everything.**

## Features

- Chat with LLMs from the command line
- Providers: OpenAI, Ollama, Anthropic, Moonshot, **OpenRouter**, and native **Kimi** and **Claude** (subscription) providers
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

# Fallback providers: if a request fails hard (persistent 429/5xx or a network
# error) before anything streamed, retry the turn on the next one that's logged in.
# failover: kimi, openai

# For plain API-key providers (openai / anthropic / moonshot / openrouter)
# api_url: https://api.openai.com/v1
# api_key: sk-your-key-here

# Extend the command-safety lists (comma-separated). tools_safe commands run
# without confirmation (read-only tools of your own, e.g. jq); tools_danger
# programs always warn first. Danger wins if a name is on both lists, and
# wrappers (env/timeout/xargs/…) cannot hide a listed program.
# tools_safe: jq, rg
# tools_danger: deploy, terraform
```

The API key for a plain key provider can be given three ways (in this order of
precedence): `-k <key>`, `api_key:` in the config, or the environment variable
`OPENAI_API_KEY` / `ANTHROPIC_API_KEY` / `MOONSHOT_API_KEY` / `OPENROUTER_API_KEY`
(or a generic `AI_AGENT_API_KEY`).

### Background workflows

On the Claude provider the model can fan work out with the `run_workflow` tool:
independent read-only sub-agents run the steps in the background (serially, or
concurrently with `parallel`), and `/workflows` shows progress (`cancel <id>` /
`retry <id>` to manage). When a run finishes, a bell + notice appear, and the
results fold into the conversation on your next message. With the **autoresume**
setting on (`/settings autoresume on`), a finished workflow instead starts a
turn by itself so the model picks its results up immediately — bounded to two
consecutive auto-turns per real user message, so a model that launches workflows
from an auto-turn can never run away on its own.

### .gitignore-aware search

The tree-walking tools (`find_symbol`, `find_references`, `project_map`) honour
the project's `.gitignore` (root file + `.git/info/exclude`) on top of a built-in
skip list, so generated/vendored files don't add noise or cost — supporting the
common patterns (`*.log`, `build/`, `/dist`, `node_modules`, `**/tmp`, and `!`
negation). `list_directory` doesn't hide anything but marks ignored entries
`(gitignored)`, and `grep` is unaffected (it searches a single file you name).

### Skills

Skills are reusable, named instruction sets beyond `AGENTS.md` — a markdown file
per skill with optional frontmatter:

```markdown
---
name: code-review
description: a thorough, security-aware review pass
---
When reviewing, check for … (the instructions)
```

Put them in `~/.local/share/ai-agent/skills/` (user, all projects) or
`./.agent/skills/` (project-local, overrides a user skill of the same name).
`/skills` lists them; `/skill <name>` activates one for the session (its
instructions are injected into the system prompt, so they survive `/compact`),
and `/skill off <name>` removes it. When tools are enabled the model can also
load one itself via the `use_skill` tool — its description lists the available
skills, so the model picks the one that fits the task.

### Commands

The interactive REPL has ~35 slash commands. Type **`/help`** for the grouped
list and **`/help <command>`** for the detail of one (e.g. `/help compact`).
They are catalogued in **[COMMANDS.md](COMMANDS.md)** (generated from
`src/commands.cpp`, the single source of truth shared with in-app help).

### Prompt shortcuts

- **`@path`** in a message expands the file inline — `look at @src/main.cpp and
  fix the leak` — when the token starts with `@` and the path exists (emails and
  `@decorators` are left alone). Size-capped; Tab completes `@`-paths too.
- **`!command`** runs a shell command directly, with no model turn and no
  confirmation (you typed it yourself). The output prints locally **and** is
  recorded in the conversation, so `!make test` followed by "fix those" just
  works.

### Large pastes

A large paste collapses into a placeholder in the input (`[paste #1: 500 lines]`)
and, on send, echoes into the transcript as a framed box showing only the first
few lines dimmed plus `… N more lines` — the model always receives the full text;
only the on-screen echo is trimmed. The preview length is the **preview** row in
`/settings` (default 8; `0` echoes everything), persisted across sessions as
`paste_preview`.

If `model` is not set (via config or `-m`), each provider falls back to a sensible default (e.g. `claude-opus-4-8`, `kimi-for-coding`, `gpt-4o-mini`, `llama3`, `openrouter/free`).

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

### Extended thinking on the Claude provider

`/thinking on|low|…|max` enables extended thinking, and `/stream on` shows the reasoning live (dimmed, `💭`). Whether the reasoning **text** is visible depends on the model — the subscription API redacts it per model:

- **`claude-sonnet-4-6`** — full thinking text streams and is shown. Use this if you want to watch the reasoning: `/model claude-sonnet-4-6`.
- **`claude-opus-4-8`** — the thinking happens (and improves answers), but the API returns the block with empty text, so there is nothing to show. No client-side setting unlocks it.

Tool use works with thinking on either model (the signed thinking blocks are replayed across tool calls as the API requires). The prompt keywords `ultracode` / `ultrathink` raise the effort to `max` for that single turn.

Neither provider opens a browser for you — the URL is printed so you can open it yourself (handy over SSH / on headless machines). Use `--login` to force a fresh login (e.g. to switch accounts).

## OpenRouter provider

[OpenRouter](https://openrouter.ai) is an OpenAI-compatible gateway to many models. Get a key from `openrouter.ai/settings/keys`, then:

```bash
export OPENROUTER_API_KEY=sk-or-...
./agent -p openrouter                       # uses openrouter/free by default
./agent -p openrouter -m anthropic/claude-3.5-sonnet "…"   # a specific model
```

Models use `vendor/model` slugs. **`openrouter/free`** (the default) auto-routes to whatever free model is currently available — handy for trying it without credits, though free models are small and heavily rate-limited (a `429 Provider returned error` means an upstream free model is throttled — retry, switch model, or add credits for [higher free limits](https://openrouter.ai/docs/api-reference/limits) and cheap paid models). For real coding work, pick a specific model with `-m`.

## Any OpenAI-compatible provider

Most providers today expose an **OpenAI-compatible** endpoint, so they work with
`provider: openai` and a custom `api_url` — no special support needed. Several
have a free tier or free trial you can try immediately:

| Provider | `api_url` | Notes |
|----------|-----------|-------|
| **Groq** | `https://api.groq.com/openai/v1` | free, very fast (Llama/Mixtral) |
| **Mistral** | `https://api.mistral.ai/v1` | free tier |
| **DeepSeek** | `https://api.deepseek.com` | cheap; reasoning models |
| **Google Gemini** | `https://generativelanguage.googleapis.com/v1beta/openai` | free key from Google AI Studio |
| **xAI Grok** | `https://api.x.ai/v1` | — |
| **Together** | `https://api.together.xyz/v1` | free trial credits |

```bash
./agent -p openai -u https://api.groq.com/openai/v1 -k gsk_... -m llama-3.3-70b-versatile
```

`/model` fetches the provider's live model list where available (and Ollama's
locally-installed models), so you can pick from a menu after connecting.

## Usage

```bash
# Interactive REPL (resumes your last provider + model)
./agent

# Pick a provider / model
./agent -p kimi
./agent -p claude -m claude-opus-4-8
./agent -p openrouter                 # free via openrouter/free (export OPENROUTER_API_KEY)

# Single prompt, then exit
./agent "explain this code"

# Scriptable: one JSON object on stdout (response + provider/model + token usage)
./agent -l quiet -o json -P "list 3 git aliases"
#   {"model":"…","provider":"…","response":"…","usage":{…}}

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
| `/settings <key> <value>` | Set a value directly: `context` (`auto`, `64K`, or `0` = unlimited), `multiline` (`on`/`off`), `model`, `tools`, `strict`, `thinking`. |
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

### Input

Enter (CR) submits; **Ctrl-J** or **Alt+Enter** inserts a newline. Turn on
`multiline` (config `multiline: on` or `/settings multiline on`) to see long or
multi-line prompts wrapped across several lines instead of one horizontally
scrolling line — ↑/↓ then move between the input lines (and fall back to history
at the top/bottom). Large pastes stay collapsed into `[paste #N]` placeholders in
either mode.

### Thinking

Reasoning models expose their thinking: Kimi/OpenAI-compatible via
`reasoning_content`, Claude via a thinking block. Set the level with `/thinking`
(`off`/`on`/`low`/`medium`/`high`/`xhigh`/`max`) — for Kimi it maps to the
`thinking.effort` field (xhigh/max clamp to high), for Claude to a `budget_tokens`
budget (`max` = the model's ceiling). Every turn streams (even with tools), so the
reasoning and the answer flow in live and tool calls are assembled from the
stream. `thinking_stream` (on by default, `/settings thinking_stream off`) toggles
whether the reasoning is shown live (prefixed 💭).

### Persisted settings

Theme, multi-line mode, thinking level and the context limit are remembered
between sessions (stored in `<home>/state.json` alongside the last provider and
model) and restored on the next launch. Security settings — the tool
confirmation mode and strict mode — are **not** persisted; they reset to the safe
default each session and must be set explicitly (config file or a flag) when you
want them relaxed.

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
