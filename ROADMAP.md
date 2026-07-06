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
  (theme, multiline, thinking, thinking_stream, context) saved to `state.json`.
- **Slash commands**: `/about /settings /model /tools /strict /thinking /theme
  /stream /history /retry /undo /memories /context /clear /help`.

## Planned

- **`/compact`**: summarise history when it grows (trimming is done; summarisation
  is the remaining piece).
- **Autocomplete**: slash commands and file paths.
- **Settable paste thresholds** in `/settings`; **collapsed paste preview** (show N
  dimmed lines + "X more…", with a settable preview line count).
- **`/provider` switching** mid-session (needs re-auth + a fresh conversation).
- **`SIGWINCH`** handling — redraw the live block on terminal resize.
- **Config-extensible danger/safe command lists**.
- **`/btw`** (mid-turn note) and **`/tasks`** (agent todo list) — Claude Code-style
  harness features; provider-agnostic if built as app features, sizeable.
- **paste-block settings** Currently whole block is displayed, we should also support
  a setting that suppresses paste-block to certain amount of lines in chat history,
  giving a preview of X lines and dimming to end with note of X lines more..
  Possibly with expand support, but this is still under valuation if expansion will
  be supported.
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
