# Commands

Slash commands available in the interactive REPL. In-app, `/help` lists these and `/help <command>` shows the detail for one. This file is generated from the same catalogue (`src/commands.cpp`).

## Conversation

### `/retry`

Removes your last message and its reply, then sends the message again — useful after an error or an unsatisfying answer.

### `/undo`

Removes the most recent user message and everything after it (the reply and any tool results) from the context. The removed message text is returned so you can edit and resend it.

### `/history`

Shows each non-system message as `you/ai/tool: <first line>`, so you can see what the model currently has in context.

### `/clear`  — alias `/reset`

Wipes the conversation for this provider and project, starting fresh. The system prompt, memories and skills are rebuilt; pins are kept.

### `/export [file]`

Saves the transcript as Markdown. With no argument a timestamped file is written under the home directory; the path must stay inside your home or the working directory.

### `/btw <note>`  — alias `/note`

Injects a note as a user message without triggering a model turn — for facts or constraints you want in context before your next real prompt.

### `/pin [text]`  — alias `/pins, /unpin`

`/pin <text>` (or `/pin` alone to pin the last reply) keeps a note in the system prompt so it survives /compact and auto-compact. `/pins` lists them, `/unpin <n|all>` removes.


## Context & cost

### `/context`

A visual breakdown of the current context: system prompt vs conversation vs the limit, with an estimated token total.

### `/compact [keep <n>|all]`

Summarises the older part of the conversation into a briefing, keeping the last N user exchanges verbatim (default 2). `/compact all` summarises everything; `/compact keep <n>` sets how many recent exchanges to keep. Your task list and change ledger are carried into the summary.

### `/cost [budget <usd>|tokens <n>]`

Shows session input/output (and cached) tokens and, when the model is priced (`price.<model>:` in config), the estimated cost. `/cost budget <usd>` and `/cost tokens <n>` set a one-shot 80%/100% warning threshold.

### `/memories [name]`

Long-term memory is per provider. With no argument, lists the memory files; with a name, prints that file's content.

### `/tasks`

Shows the todo list the model maintains for multi-step work (via the update_tasks tool), with ✓/▸/○ status glyphs.

### `/queue [drop <n|all>]`

Lists messages you typed while a turn was running (they auto-send when it finishes). `/queue drop <n|all>` removes queued entries.


## Providers & models

### `/provider [name]`

Switches the active provider (openai, ollama, anthropic, moonshot, openrouter, kimi, claude), carrying the conversation over and restoring that provider's remembered model. Subscription providers must already be logged in.

### `/model [name]`

With no argument shows the current model. With a name switches to it and remembers it for this provider across sessions.

### `/thinking <off|on|low|medium|high|xhigh|max>`  — alias `/effort`

Controls extended thinking / reasoning effort. Honoured by claude, anthropic, kimi, openai and openrouter (mapped to each API's field). Persisted across sessions.

### `/stream <off|on|collapse>`

Whether to stream the model's reasoning live. `collapse` streams it then hides it once the answer is done, leaving only the answer in the transcript.


## Tools & safety

### `/tools <confirm|auto|insecure>`

confirm: ask before mutating tools (read-only run freely). auto: run ordinary tools without asking; danger-listed commands still warn. insecure: run everything without asking. Not persisted — resets each session.

### `/plan [on|off]`

Blocks every mutating tool (write_file, edit_file, run_command, non-read-only MCP tools) so the model investigates and proposes a plan instead of acting. `!command` still works (it's user-driven). Bare /plan toggles.

### `/strict <on|off>`

In confirm mode, ask before even the safe read-only shell commands (ls, cat, git log, …) that would otherwise run without asking.

### `/trust [drop <n|all>]`

Lists the tool-safety state: mode, any active turn grant, config safe/danger lists, and each standing allow-session/allow-similar grant with a use count. `/trust drop <n|all>` revokes them.

### `/changes [diff|revert <path|all>]`

Lists files created or modified this session. `/changes diff <path>` shows the diff vs the session-start version; `/changes revert <path|all>` restores it.

### `/mcp [refresh|prompt <server> <name> [k=v]]`

Shows configured MCP servers and their tools/resources/prompts. `refresh` reconnects; `prompt <server> <name> [k=v]` loads a server prompt into context.

### `/advisor <on|off|model N>`

Exposes a consult_advisor tool letting the model ask a stronger advisor model (default claude-opus) for a second opinion. `model <name>` sets which.


## Skills & workflows

### `/skills`

Lists skills discovered in <home>/skills and ./.agent/skills, with an active (●) / inactive (○) marker, source and description.

### `/skill <name> | off <name>`

`/skill <name>` activates a skill's instructions for the session (injected into the system prompt, surviving /compact). `/skill off <name>` deactivates. The model can also load one itself via the use_skill tool.

### `/autoresume [on|off]`

When a background workflow finishes, automatically resume the conversation so the model reads its results and continues — instead of the results only folding in on your next message. Bounded to 2 auto-turns per real message. Also the "workflow resume" row in /settings.

### `/workflows [id | cancel <id> | retry <id>]`

Lists background workflow runs the model started (via run_workflow). With an id, shows its steps; `cancel <id>` stops a run, `retry <id>` relaunches a finished one keeping succeeded steps.


## Session & UI

### `/settings [<key> <value>]`

With no argument opens the interactive settings menu. With `<key> <value>` sets one directly (model, tools, thinking, theme, context, multiline, auto_compact, autoresume, redact_secrets, max_tokens, tool_call_limit, …).

### `/bell [never|ask_user|question|attention|always]`

Controls the terminal bell. always: on every answer plus anything needing you. attention: only a workflow finishing, a tool-permission prompt, or an answer that is a question. question: only when the answer is a question. never: silent. At any level except never, a DANGEROUS command's confirmation always rings. Bare /bell opens a picker; also the "bell" row in /settings.

### `/jobs [id | stop <id|all>]`

Lists background jobs started with run_command(background:true) — a dev server, watcher, or tail -f — with running/exited status and runtime. `/jobs <id>` shows a job's captured output (scrollable); `/jobs stop <id|all>` stops jobs. The model inspects the same jobs via the check_job tool.

### `/limits`

Shows the rate-limit and quota headers the provider returned on the last request (e.g. requests/tokens remaining and reset times). Providers vary; subscription providers often don't expose quota this way, in which case it reports none were seen.

### `/raw [request|response]`

Shows the exact JSON request last sent to the provider and the response received (assembled from the stream when streaming), in a scrollable view — for debugging prompts, tools and provider quirks. `/raw request` or `/raw response` shows just one. Auth headers are not part of the body shown.

### `/theme <dark|light|warm|cool|rose>`

Changes the terminal colour theme (dark, light, warm, cool, rose). Persisted across sessions. Never sets the terminal background.

### `/help [command]`

With no argument lists all commands grouped by area. With a command name (with or without the leading /) shows detailed help for just that command.

### `/about`  — alias `/info`

Shows the version, a one-line description, and the current provider/model and any loaded project-instructions file.

### `/exit`  — alias `/quit`

Exits the REPL. Settings and the conversation are saved on the way out.


## Input shortcuts

### `!<command>`

A line starting with `!` runs the rest as a shell command directly — no model turn, no confirmation (you typed it). The output prints locally AND is recorded, so `!make test` then "fix those" works. Not blocked by /plan.

### `/paste [n]`

A large paste collapses to a placeholder and its transcript echo is trimmed to the paste-preview length. /paste lists the large pastes you've sent this session (Enter opens the full text, scrollable and wrapped); /paste <n> jumps to one.

### `@path`

`@src/foo.cpp` inside a message expands the file inline (when the token starts with @ and the path exists — emails and @decorators are left alone). Size-capped; Tab completes @-paths.

