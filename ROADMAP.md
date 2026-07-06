# Roadmap

Ideas and planned work for AIAgent. Done items are kept for context; the top of
each list is roughly the current priority order.

## Done

- **Providers**: OpenAI, Ollama, Anthropic, Moonshot, Kimi, Claude — with
  subscription OAuth (Bearer token) for Kimi and Claude, not pay-as-you-go API keys.
- **Streaming everywhere**: every turn streams (even with tools). Content,
  reasoning and tool calls are assembled from the stream (`stream_reset` /
  `parse_stream` → `StreamChunk` / `stream_result`).
- **Thinking / reasoning**: captured for Kimi (`reasoning_content`) and Claude
  (thinking block); Claude extended-thinking wired (effort → `budget_tokens`,
  `max` = model ceiling); Kimi `xhigh`/`max` normalised to `high`. Reasoning
  streams live, dimmed, behind a 💭 marker; `/stream off|on|collapse` toggles it —
  collapse shows the reasoning live then hides it once the answer is done.
  (Note: Claude's OAuth API redacts thinking text, so only Kimi shows a transcript.)
- **Current date** injected into the system prompt (so the model needn't call `date`).
- **UI**: inline REPL (native scrollback, no ncurses), colour themes
  (dark/light/warm), multi-line prompt mode, interactive `/settings` menu,
  visual `/context`, arrow-select tool confirmation.
- **Context management**: `context_limit` trimming, `context: auto` (per-model window).
- **Tool safety**: confirm / auto / insecure modes, danger list, chain-aware safe
  list, `strict` mode.
- **Persistence**: per-provider + per-project history and memories; UI settings
  (theme, multiline, thinking, thinking_stream/collapse, context, auto_compact,
  paste_preview) saved to `state.json`.
- **Paste preview**: `paste_preview` trims a framed paste's echo to the first N
  lines + "… N more lines" (full text still sent to the model).
- **Slash commands**: `/about /settings /model /tools /strict /thinking /theme
  /stream /history /retry /undo /memories /context /compact /clear (/reset) /help`.
- **`/compact`**: one LLM call summarises the history and replaces it; runs async
  with a "compacting" spinner (Ctrl-C cancels).
- **Auto-compact**: when a turn leaves the context at/above `auto_compact_pct`
  (default 80%) of the context budget, the history is summarised automatically
  (same async path as `/compact`, labelled "auto-compact" in the transcript).
  Off by default; toggle with `/settings auto_compact on|off`. Inactive unless a
  context budget is known (`context: auto` or an explicit limit).
- **SIGWINCH**: the live block redraws at the new width on terminal resize.
- **Line editing**: the inline REPL supports Ctrl-A/E (line ends) and Ctrl-W/U/K
  (delete word / kill to line start / kill to line end), respecting logical line
  boundaries and pruning removed paste placeholders.
- **`/advisor`** (claude only): exposes a `consult_advisor` tool so the main model
  can ask a stronger sibling model (e.g. sonnet → opus) for a second opinion on a
  hard problem. `/advisor <on|off|model <name>>`; the consult is a one-shot,
  non-streaming call on the same OAuth session with the model temporarily swapped.
  Gated by the `advisor` provider capability; the tool is registered only while
  on and removed on a switch away from claude.
- **`/btw <note>`** (alias `/note`): add a note to the context as a user message
  without asking for a reply — the model sees it on your next turn. Consecutive
  same-role messages are merged in the Anthropic request so this can't break
  Claude's strict user/assistant alternation.
- **`/provider <name>`**: switch provider mid-session, carrying the current
  conversation over (system prompt refreshed to the new provider's identity).
  Re-auth is non-interactive (`ready_noninteractive`): a subscription provider
  with no cached token is refused with a hint to relaunch with `-p <name>`,
  rather than blocking the raw-mode REPL on an OAuth prompt.
- **`/workflows`** (claude only): a `run_workflow(name, steps[])` tool lets the
  model launch a background workflow — each step is an independent read-only
  sub-agent (own client/provider/conversation, read/list/grep tools, no re-auth).
  Runs execute on background threads (`WorkflowManager`); the tool returns a run
  id immediately. `/workflows` lists runs + progress, `/workflows <id>` shows step
  results. Finished runs are folded into the conversation at the next turn's start.
  NOTE: lightweight single-machine version — steps run sequentially and results
  are pull-viewed; live push notifications and parallel steps are follow-ups.
- **Project instructions**: a project-local `AGENTS.md` (or `.ai-agent.md` /
  `AGENT.md`) in the working directory is read into the system prompt (after the
  long-term memories), so a project can pin coding style, testing conventions and
  constraints without repeating them each session. Read fresh from the cwd (size-
  capped); `/about` shows which file is loaded.
- **`fetch_url`**: fetch an http/https page and return its text — HTML reduced to
  readable text (`html_to_text`: drop script/style/comments, block tags → line
  breaks, tags stripped, named + numeric entities decoded, whitespace tidied).
  Complements `web_search` (find a link, then read it) and is the offline-model's
  way to read docs. Uses `Client::get`; gated by the same network toggle as
  `web_search`; only http/https allowed.
- **`run_command` options**: per-call `timeout` (seconds, clamped; default 120,
  max 600), `cwd` (runs inside `cd <dir> && ( ... )`), and `env` (extra vars for
  that command only, applied via `env_cpp`'s RAII `env_scope` so the parent
  environment is restored afterwards). Pulls in **`env_cpp`** (oskarirauta/env_cpp
  v1.0.0) as a submodule — a small read/write environment library with a
  json_cpp-style subscript API.
- **`edit_file`**: targeted edit tool — replace an exact `old_string` with
  `new_string` in an existing file (`replace_all` optional; unique-match enforced
  otherwise). Cheaper/safer than rewriting the whole file; danger-classified and
  change-tracked like `write_file`. (Prefer over write_file for edits.)
- **`/export [file]`**: write the whole conversation to a Markdown file (system /
  you / assistant / tool-result sections, assistant tool calls noted) for bug
  reports, sharing and docs. Defaults to `agent-export-<timestamp>.md`.
- **Change tracking (`/changes` + revert)**: a Registry pre-run hook snapshots a
  file's pre-write content (session-start state, size-capped) before `write_file`
  overwrites it. `/changes` lists created/modified/reverted files with a line
  delta; `/changes diff <path>` shows a block diff; `/changes revert <path|all>`
  restores the original (or removes a session-new file). The useful core of the
  "snapshot" idea without full filesystem snapshots.
- **MCP (Model Context Protocol) support**: connect to MCP servers defined in
  `mcp.json` / `.mcp.json` — **stdio** (`{command,args,env}`, own fork/exec +
  persistent-pipe transport) and **HTTP** (`{url,headers}`, Streamable HTTP with
  `Mcp-Session-Id` + json-or-SSE responses). JSON-RPC 2.0 initialize handshake,
  then `tools/list` + (by capability) `resources/list` + `prompts/list`. Each
  server tool is exposed as `mcp__<server>__<tool>` (schema passed through);
  **resources** get a per-server `mcp__<server>__read_resource(uri)` tool;
  **prompts** load via `/mcp prompt <server> <name> [k=v]` (injected into context).
  Servers connect **in parallel** at startup (bounded by the slowest handshake),
  killed/reaped on exit. `/mcp` lists servers/tools/resources/prompts + status;
  `/mcp refresh` re-lists a server whose offerings changed. Follow-up left:
  automatic push refresh on `notifications/tools/list_changed` (needs a background
  reader thread) — `/mcp refresh` covers it manually.
- **`web_search` tool**: queries DuckDuckGo (html endpoint) and returns the top
  results (title, URL, snippet) for the model to read and cite. Especially useful
  for **local models (Ollama/llama.cpp)** with no MCP search server — it gives an
  otherwise-offline model a way to fetch facts beyond its training data and the
  project files. On by default; `web_search: false` (config) or `/settings
  web_search off` disables it; `web_search_url` overrides the endpoint. Added
  `Client::get` (follows redirects) for it; parser is HTML-scrape based (decodes
  the `uddg=` redirect, strips tags/entities). (Once MCP lands, an MCP search
  server can also provide this.)
- **`find_symbol` tool** (light codebase search): a definition-aware, tree-wide
  symbol search — give an identifier and it returns where it is *defined*
  (class/struct/enum/def/fn/type or a C-family `NAME(...)` opening), skipping call
  sites (lines ending in `;`) and build/vendor dirs. Always fresh (no persistent
  index to go stale), a precise complement to `grep`. The heavier options
  (embeddings / vector search) stay out of scope — grep + this cover the need.
- **Cost / token budget**: per-model pricing from config (`price.<model>: <in>/<out>`
  USD per million tokens) drives an estimated session cost shown on the status line
  and via `/cost`. `budget_usd` / `budget_tokens` (config or `/cost budget|tokens`)
  emit a one-shot warning at 80% and 100%. Unpriced models (flat-rate
  subscriptions) show token usage only. Built on `TokenStats`; no built-in prices
  so nothing goes stale.

## Planned

### From the second Kimi review (assessed, top picks)

- **Quick wins**: bulk/array `read_file` (line ranges already done via
  offset/limit). (`run_command` timeout/cwd/env and `fetch_url` shipped — see Done.)
- **Prompt caching** (Anthropic + Moonshot/Kimi): `cache_control: ephemeral` on the
  system prompt + tools + stable history prefix — cuts input cost ~90% and latency
  on cache hits. High value for heavy interactive use.
- **Parallel tool calls**: run independent read-only tool calls concurrently in
  `process_turn` (writes/commands stay serial).
- **DeepSeek + OpenRouter providers**: both OpenAI-compatible → small.
- **Native git tools** (`git_status/diff/log/show`, commit-with-confirm) — partly
  redundant with the auto-safe `run_command`, but gives structured output.
- Assessed low/covered: format/lint (→ run_command/MCP), code interpreters
  (→ run_command), file-watch, image input (CLI out of scope), command macros.

- **Autocomplete**: slash commands and file paths.
- **Settable paste thresholds** in `/settings` (the char/line/ms thresholds exist in
  config but are not yet exposed in the menu).
- **Config-extensible danger/safe command lists**.
- **`/tasks`** (agent todo list) — Claude Code-style harness feature; provider-
  agnostic if built as an app feature, sizeable. (`/btw` shipped, see Done.)
- **paste-block expand**: the preview (first N lines + "… N more lines") ships via
  `paste_preview`; interactive expand/collapse of a previewed block is still open
  and under evaluation.
- **`/workflows` enhancements**: parallel steps, live push notifications on
  completion, and cancel/retry of a run.
- **`/rc` command** for claude only
- **More proviers** We don't aim to support every provider, we aim to support enough, most
  common and most useful ones.
- **word splitting to prompt** when multi-line mode is used, try to avoid cutting
  to next line mid-word
- **skills** support skills

## Considered / out of scope

- **Multi-agent orchestration** (cloud code review, hundreds of agents) — the local
  `/workflows` is the lightweight take; full cloud orchestration is a large
  separate project, out of scope.
- **Internal commands** replacing busybox coreutils — high effort, uncertain model
  uptake; prefer clean errors + date-in-context instead.
- **Filesystem snapshots before dangerous ops** — overkill; per-session change
  tracking (`/changes` + revert, above) covers the useful part far more cheaply.
- **Custom tools defined in config** — largely covered by `run_command` (the model
  can already run `make test`, `build`, etc.), so a config `tools:` section is
  mostly sugar; low marginal value.
- **Automated test run after edits** — better expressed as a project instruction
  in `AGENTS.md` ("run `make test` before finishing") which the model then does via
  `run_command`; not worth a hard-coded hook.
- **Named session save/load** — history is already auto-saved per provider *and*
  per project directory; named parallel sessions would be a minor enhancement, low
  priority.
- **Image input** — provider-dependent and of little use in a text CLI.
