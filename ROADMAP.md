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
- **`/btw <note>`** (alias `/note`): add a note to the context as a user message
  without asking for a reply — the model sees it on your next turn. Consecutive
  same-role messages are merged in the Anthropic request so this can't break
  Claude's strict user/assistant alternation.
- **`/provider <name>`**: switch provider mid-session, carrying the current
  conversation over (system prompt refreshed to the new provider's identity).
  Re-auth is non-interactive (`ready_noninteractive`): a subscription provider
  with no cached token is refused with a hint to relaunch with `-p <name>`,
  rather than blocking the raw-mode REPL on an OAuth prompt.

## Planned

- **Autocomplete**: slash commands and file paths.
- **Settable paste thresholds** in `/settings` (the char/line/ms thresholds exist in
  config but are not yet exposed in the menu).
- **Config-extensible danger/safe command lists**.
- **`/tasks`** (agent todo list) — Claude Code-style harness feature; provider-
  agnostic if built as an app feature, sizeable. (`/btw` shipped, see Done.)
- **paste-block expand**: the preview (first N lines + "… N more lines") ships via
  `paste_preview`; interactive expand/collapse of a previewed block is still open
  and under evaluation.
- **`/workflows` command** for claude only
- **`/rc` command** for claude only
- **`/advisor` command** setting for claude only
- **More proviers** We don't aim to support every provider, we aim to support enough, most
  common and most useful ones.

## Considered / out of scope

- **Multi-agent orchestration** ("ultra"/workflows, cloud code review) — these are
  harness features, not provider capabilities; building them here would be a large
  separate project.
- **Internal commands** replacing busybox coreutils — high effort, uncertain model
  uptake; prefer clean errors + date-in-context instead.
