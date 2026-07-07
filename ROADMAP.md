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
- **`project_map` tool**: a high-level project overview on demand — build/manifest
  files parsed (package.json name/scripts/deps, Cargo/pyproject name+deps, go.mod
  module, Makefile targets [object/phony filtered], CMake project), the top-level
  directory layout with per-dir file counts, and a source-language histogram.
  Skips build/vendor dirs; cheaper than exploring with many list/read calls.
- **OpenRouter provider**: OpenAI-compatible gateway to many models
  (`vendor/model`, incl. `:free` variants). A thin `OpenAI` subclass — default
  endpoint `https://openrouter.ai/api/v1`, Bearer auth, plus OpenRouter's optional
  `HTTP-Referer`/`X-Title` attribution headers. (DeepSeek/Gemini still deferred —
  no key here to test against.)
- **Diff preview in the confirmation dialog**: `write_file`/`edit_file` confirms
  show what would change — a block diff of the file vs the new content (write_file)
  or the -old/+new snippets (edit_file, each multi-edit entry), coloured like a
  diff — instead of a raw args dump. `block_diff` moved to `text_utils`, shared
  with `/changes`.
- **`/tasks`** (agent todo list): an `update_tasks` tool the model calls to keep a
  short plan (title + status pending/in_progress/done) for multi-step work; `/tasks`
  renders it with ✓/▸/○. Session-scoped, provider-agnostic.
- **Autocomplete**: Tab in the inline REPL completes a leading slash command or a
  file-path token — unique match completes (dirs get `/`, else a space), a shared
  longer prefix extends, and an ambiguous prefix lists the candidates.
- **Config-extensible danger/safe command lists**: `tools_safe:` / `tools_danger:`
  config keys (comma-separated). Extra safe commands skip confirmation (pipes and
  chains included); extra danger programs always warn — checked on the real
  program (wrapper unwrapping applies), danger wins if a name is on both lists.
  Config file only, never persisted state or project files.
- **`/workflows` enhancements**: parallel steps, per-run cancel + retry (succeeded
  steps kept), completion notice (bell + ●-line), and opt-in autoresume — a
  finished workflow starts a turn by itself via the normal pending queue, bounded
  to 2 consecutive auto-turns per real user message.
- **Context pin (`/pin`)**: `/pin <text>` (or `/pin` alone to pin the last reply)
  keeps a note in the system prompt, so it stays in context every turn **and
  survives `/compact` and auto-compact** verbatim (compaction rebuilds the system
  prompt, which carries the pins). `/pins` lists them, `/unpin <n|all>` removes.
  Session-scoped. For "don't forget this decision / constraint" that must outlast
  summarisation.
- **`/plan` (read-only planning mode)**: `/plan on` (or bare `/plan` to toggle)
  blocks every mutating tool — write_file, edit_file, run_command, non-read-only
  MCP tools — so the model investigates with read-only tools and proposes a plan
  instead of acting; a refused call tells it to present the plan. A system-prompt
  note primes the model, the status line shows "· plan", and `/plan off` restores
  writes. `!command` passthrough still works (it is user-driven). Session-only.
- **Attention cues**: when a confirm dialog appears after the turn has already
  run unattended (≥4s), the terminal bell rings once — the agent is blocked on
  you. And a turn that ran ≥8s ends with a one-line digest ("● done in Ns · N
  tool calls") plus a bell, so a long unattended turn announces its completion.
- **Per-turn tool-call budget**: `tool_call_limit` (default 50; config +
  /settings "tool budget"; 0 = unlimited) caps how many tools a single turn runs
  before asking to continue — a runaway-loop guard so `/tools auto` can be left
  unattended. On the cap it raises a continue/stop confirm (via the existing
  callback); a stop ends the turn keeping the work so far, and non-interactive
  runs stop automatically. Insecure mode never interrupts.
- **Skills**: reusable, named instruction sets — a markdown file per skill (with
  optional name/description frontmatter) in `<home>/skills/` (user) or
  `./.agent/skills/` (project, overrides same-named user skill). `/skills` lists,
  `/skill <name>` activates one for the session (injected into the system prompt,
  so it survives compaction), `/skill off <name>` removes it; and a model-
  invocable `use_skill` tool whose description enumerates the available skills so
  the model can load the fitting one itself.
- **Word-boundary wrapping in multi-line input**: the multi-line prompt now wraps
  at spaces (like the transcript) instead of hard-breaking mid-word at the column
  limit; a single over-long word (URL/token) still hard-breaks. Byte ranges stay
  contiguous so cursor movement/editing are unaffected.
- **`/compact` progress bar**: the summarisation now streams, and the status line
  shows a live bar + rough percentage (`compacting [██░░░░] ~34%`) alongside the
  elapsed seconds — a rough estimate (summary ≈ 1/12 of the input, bounded) so you
  can gauge when the session will resume, not just that it isn't frozen.
- **Parallel tool calls**: when a model turn batches several read-only tools
  (read_file / grep / find_symbol / list_directory) they run concurrently on a
  bounded thread pool; results are recorded in the model's original order. Any
  write / command / side-effecting call in the batch forces the whole batch back
  to serial. `parallel_tools` config, default on.
- **Prompt caching**: Anthropic/Claude requests mark `cache_control: ephemeral`
  breakpoints on the stable prefix — the tools, the system prompt and the last
  message (incremental multi-turn caching) — so repeat context is cheap (~10%) and
  faster on cache hits. Claude keeps the CLI-identity system block first and caches
  the rest. OpenAI/Kimi/DeepSeek cache automatically server-side, so no change
  there. `prompt_cache` config / `/settings prompt_cache on|off`, default on.
- **Bulk `read_file`**: a `paths` array reads several files in one call (each
  under a `===== path =====` header, sharing one output budget; a missing file is
  noted and the rest still read), cutting tool round-trips. Single-file
  `path` + `offset`/`limit` windowing unchanged.
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
  otherwise), or pass an **`edits` array** to apply several replacements in one
  atomic call (sequential; if any fails the file is left unchanged). Cheaper/safer
  than rewriting the whole file; danger-classified and change-tracked like
  `write_file`. (Prefer over write_file for edits.)
- **`find_references`**: tree-wide whole-identifier usage search — where a symbol
  is *used* (call sites + definition), the counterpart to `find_symbol` (which
  finds the definition). More precise than `grep` (whole-word, so `foo` skips
  `foobar`); skips build/vendor dirs and binaries; reports the reference and line
  counts.
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

Roughly in priority order for real coding use. Kept only what is genuinely useful
*and* buildable well; the rest is dropped below.

Batches 1 (edit-build-test loop: @path mentions, edit_file near-miss + verify
snippet, run_command head+tail, !command passthrough) and 2 (trust & flow: MCP
annotation gating, allow-for-this-turn, /trust, deny-with-note) are **done** —
see the Done section above.

Batch 3 — context & provider economy: **done**. max_tokens 8192 + truncation
guard, cached-token accounting (+ OpenAI include_usage), /thinking on openai/
openrouter, auto-retry with backoff, cache-stable hysteresis trimming, tool-
result supersession, and rolling compact (keep-tail + verbatim tasks/changes).

Batch 4 — remaining:
- **`outline_file` tool**: symbols of one file with line numbers (find_symbol
  logic on a single file).
- **`.gitignore`-aware traversal** (90% semantics) for the search tools.
- **UI polish**: settable paste thresholds in `/settings`, `/paste <n>` to
  expand a collapsed paste block.
- **more providers** (enough, not every one).

## Considered / dropped

Deliberately out — either covered by something we already have, or low return for
a coding tool (it's a tool, not a toy).

- **Native git tools** — the auto-safe `run_command` already runs read-only git
  (`status/diff/log/show`); a commit-with-confirm wrapper is marginal over that.
- **Auto-fix loop** (build/test fail → fix → retry) — already emergent: the model
  sees the failing tool output and iterates. Better nudged via `AGENTS.md` than a
  hard-coded loop.
- **Linter / formatter / test-runner tools** — `run_command` + an `AGENTS.md` line
  ("run `clang-format` / `make test` after edits") covers it; or an MCP server.
- **Code eval** (`python -c`, `node -e`, regex tester) — `run_command` already does
  this.
- **Refactoring tools** (rename / extract / move) — `edit_file` (+ multi-edit) +
  `find_references` + `grep` cover the practical need; full semantic refactors are
  high-effort and low-reliability in plain text.
- **File watch / auto-run on change** — niche for an interactive CLI.
- **Command macros / snippets / templates** — prompt history + `AGENTS.md` conventions
  cover most of it.
- **Multi-agent cloud orchestration** — the local `/workflows` is the lightweight
  take; full cloud orchestration is a separate project.
- **Internal commands** replacing busybox coreutils — high effort, uncertain uptake;
  prefer clean errors + date-in-context.
- **Filesystem snapshots before dangerous ops** — `/changes` + revert covers the
  useful part far more cheaply.
- **Custom tools defined in config** — largely `run_command` sugar.
- **Named session save/load** — history is already auto-saved per provider *and*
  per project dir; named parallel sessions are a minor add.
- **Image input** — provider-dependent, little use in a text CLI.
