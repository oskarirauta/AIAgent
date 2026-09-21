#include "agent/commands.hpp"

#include <sstream>
#include <algorithm>
#include "common.hpp"

namespace agent {

const std::vector<CommandDoc>& command_catalog() {
    static const std::vector<CommandDoc> cat = {
    // ── Conversation ─────────────────────────────────────────────────────
    { "/retry", "", "", "Conversation", "re-run your last message",
      "Removes your last message and its reply, then sends the message again — "
      "useful after an error or an unsatisfying answer." },
    { "/undo", "", "", "Conversation", "drop the last exchange from history",
      "Removes the most recent user message and everything after it (the reply and "
      "any tool results) from the context. The removed message text is returned so "
      "you can edit and resend it." },
    { "/history", "", "[n|all|search <text>]", "Conversation", "browse messages in the current context",
      "With no argument, lists each non-system message with an index, role and preview. "
      "In the interactive UI, press Enter on a row to open the full message. "
      "`/history <n>` opens one full message; `/history all` dumps the whole visible context; "
      "`/history search <text>` lists matching messages." },
    { "/clear", "/reset", "", "Conversation", "clear the conversation history",
      "Wipes the conversation for this provider and project, starting fresh. The "
      "system prompt, memories and skills are rebuilt; pins are kept." },
    { "/export", "", "[file]", "Conversation", "write the conversation to a Markdown file",
      "Saves the transcript as Markdown. With no argument a timestamped file is "
      "written under the home directory; the path must stay inside your home or the "
      "working directory." },
    { "/btw", "/note", "<note>", "Conversation", "add a note to the context, no reply",
      "Injects a note as a user message without triggering a model turn — for facts "
      "or constraints you want in context before your next real prompt." },
    { "/steer", "", "<prompt>", "Conversation", "steer active work at the next checkpoint",
      "Guides the active turn without interrupting immediately. In `checkpoint` mode, "
      "guidance is delivered at the next tool boundary before the model begins its next action. "
      "When idle, queues guidance for the next turn. For background sub-agents, use "
      "`/steer workflow <id> <prompt>`. (For persistent guidance across all sessions, "
      "use `/settings steer <prompt>`)." },
    { "/steer!", "", "<prompt>", "Conversation", "interrupt and redirect active turn immediately",
      "Immediately aborts the active in-flight model request/stream, injects your guidance "
      "as a user turn, and restarts generation right now. Output generated so far is kept." },
    { "/pin", "/pins, /unpin", "[text]", "Conversation", "keep a note in context through /compact",
      "`/pin <text>` (or `/pin` alone to pin the last reply) keeps a note in the "
      "system prompt so it survives /compact and auto-compact. `/pins` lists them, "
      "`/unpin <n|all>` removes." },

    // ── Context & cost ───────────────────────────────────────────────────
    { "/context", "", "", "Context & cost", "show context usage",
      "A visual breakdown of the current context: system prompt vs conversation vs "
      "the limit, with an estimated token total." },
    { "/compact", "", "[keep <n>|all]", "Context & cost", "summarise older history",
      "Summarises the older part of the conversation into a briefing, keeping the "
      "last N user exchanges verbatim (default 2). `/compact all` summarises "
      "everything; `/compact keep <n>` sets how many recent exchanges to keep. Your "
      "task list and change ledger are carried into the summary." },
    { "/cost", "", "[budget <usd>|tokens <n>]", "Context & cost", "token usage + estimated cost",
      "Shows session input/output (and cached) tokens and, when the model is priced "
      "(`price.<model>:` in config), the estimated cost. `/cost budget <usd>` and "
      "`/cost tokens <n>` set a one-shot 80%/100% warning threshold." },
    { "/status", "", "", "Session", "show the active provider and runtime status",
      "Shows the provider, model, reasoning/stream settings, context budget, tool mode "
      "and latest token counts in one compact diagnostic view." },
    { "/stats", "", "", "Session", "show usage, cost and provider limits",
      "Shows session token usage, estimated cost and any rate-limit/reset information "
      "reported by the latest provider response." },
    { "/diagnose", "", "", "Session", "show a combined diagnostic report",
      "Combines runtime status, usage, context and provider-limit information. "
      "Use `/raw` for the exact latest request and response." },
    { "/stop", "/interrupt", "", "Session", "request the active turn to stop",
      "Interrupts the current model request or tool operation at its next safe "
      "boundary. Work and tool results already collected are kept in the context." },
    { "/memories", "", "[name]", "Context & cost", "list or view this provider's memories",
      "Long-term memory is per provider. With no argument, lists the memory files; "
      "with a name, prints that file's content." },
    { "/roadmap", "", "", "Context & cost", "show the project's roadmap",
      "Displays ROADMAP.md from the current project when present. It is loaded only "
      "on request, so planning notes do not consume context on every turn." },
    { "/tasks", "", "", "Context & cost", "show the agent's todo list",
      "Shows the todo list the model maintains for multi-step work (via the "
      "update_tasks tool), with ✓/▸/○ status glyphs." },
    { "/session", "", "[name]", "Conversation", "switch between parallel sessions in this project",
      "One project directory can hold several independent conversations — e.g. one "
      "building a feature and one reviewing it — each with its own history file and "
      "lock. With no argument shows the active session and this project's others. "
      "`/session <name>` saves the current conversation and switches (creating the "
      "session if it is new); `/session default` returns to the main one. To open a "
      "second window directly on a named session: `agent -n <name>`." },
    { "/sessions", "", "[delete <key>]", "Context & cost", "list saved sessions (size, age); delete to free space",
      "Lists every saved session across providers and projects with its file size "
      "and last-used time. In the menu `d` deletes the selected session; "
      "`/sessions delete <key>` does the same directly. The active session cannot "
      "be deleted (use /clear to empty it)." },
    { "/shell", "", "", "Session", "visit an interactive shell, then return",
      "Hands the whole terminal to your $SHELL (for `git push` with a password, "
      "a quick editor visit, …); `exit` returns to the agent with the transcript "
      "intact. For one-off commands `!<command>` is faster and lets the model see "
      "the output. Not available while the AI is answering — wait and retry." },
    { "/queue", "", "[drop <n|all>|edit <n|live:n> <text>|promote n]", "Context & cost", "pending work behind the running turn",
      "Lists messages and commands typed while a turn was running, including their "
      "queue type (`message`, `command`, `shell`, or `live note`). They run when "
      "the current turn finishes. `/queue drop <n|all>` removes entries; `/queue edit "
      "<n|live:n> <text>` changes one before it runs; `/queue promote n` moves a "
      "queued message to the front." },

    // ── Providers & models ───────────────────────────────────────────────
    { "/provider", "", "[name]", "Providers & models", "switch provider mid-session",
      "Switches the active provider (openai, codex, ollama, anthropic, moonshot, "
      "openrouter, kimi, claude), carrying the conversation over and restoring that "
      "provider's remembered model. Subscription providers must already be logged in." },
    { "/model", "", "[name]", "Providers & models", "show or change the model",
      "With no argument shows the current model. With a name switches to it and "
      "remembers it for this provider across sessions. The name is forgiving: a "
      "family shorthand or a small typo is resolved to the provider's real model "
      "(`fable` → `claude-fable-5`, `sonet` → `claude-sonnet-4-6`), while a name "
      "that matches nothing is sent to the API unchanged." },
    { "/thinking", "/effort", "<off|on|low|medium|high|xhigh|max>", "Providers & models",
      "set the reasoning/thinking level",
      "Controls extended thinking / reasoning effort. Honoured by codex, claude, anthropic, "
      "kimi, openai and openrouter (mapped to each API's field). Persisted across "
      "sessions." },
    { "/stream", "", "<off|on|collapse>", "Providers & models", "live reasoning display",
      "Whether to stream the model's reasoning live. `collapse` streams it then "
      "hides it once the answer is done, leaving only the answer in the transcript." },

    // ── Tools & safety ───────────────────────────────────────────────────
    { "/tools", "", "[<confirm|auto|insecure> | list | group <name> [on|off] | profile <name>]", "Tools & safety",
      "tool mode, profiles and group toggles",
      "With no argument, shows active tool confirmation mode, profile, and enabled groups. "
      "`/tools <confirm|auto|insecure>` sets confirmation mode. `/tools list` lists all tools "
      "with estimated schema tokens. `/tools group <name> <on|off>` enables/disables a group. "
      "`/tools profile <name>` switches profile." },
    { "/profile", "", "[code|full|research|review|minimal]", "Tools & safety", "select active tool profile",
      "Switches tool profile to restrict active tools and reduce schema tokens sent on "
      "every turn. code (default): disables web and workflow tools for coding tasks. "
      "full: all tools active. research: read-only exploration with web search. "
      "review: read-only audit (no web/mutations). minimal: read, edit, and run command only." },
    { "/plan", "", "[on|off]", "Tools & safety", "read-only planning mode",
      "Blocks every mutating tool (write_file, edit_file, run_command, non-read-only "
      "MCP tools) so the model investigates and proposes a plan instead of acting. "
      "`!command` still works (it's user-driven). Bare /plan toggles." },
    { "/strict", "", "<on|off>", "Tools & safety", "also confirm safe read-only commands",
      "In confirm mode, ask before even the safe read-only shell commands (ls, cat, "
      "git log, …) that would otherwise run without asking." },
    { "/trust", "", "[drop <n|all>]", "Tools & safety", "review/revoke session tool grants",
      "Lists the tool-safety state: mode, any active turn grant, config safe/danger "
      "lists, and each standing allow-session/allow-similar grant with a use count. "
      "`/trust drop <n|all>` revokes them." },
    { "/changes", "", "[diff|revert <path|all>]", "Tools & safety", "files the agent changed",
      "Lists files created or modified this session. `/changes diff <path>` shows the "
      "diff vs the session-start version; `/changes revert <path|all>` restores it." },
    { "/mcp", "", "[refresh|prompt <server> <name> [k=v]|enable <server>|disable <server>]", "Tools & safety",
      "MCP servers, tools, resources, prompts",
      "Shows configured MCP servers and their tools/resources/prompts. `refresh` "
      "reconnects; `prompt <server> <name> [k=v]` loads a server prompt into context. "
      "`enable <server>` and `disable <server>` dynamically toggle servers." },
    { "/advisor", "", "<on|off|model N>", "Tools & safety", "(claude) consult a stronger model",
      "Exposes a consult_advisor tool letting the model ask a stronger advisor model "
      "(default claude-opus) for a second opinion. `model <name>` sets which." },

    // ── Skills & workflows ───────────────────────────────────────────────
    { "/skills", "", "", "Skills & workflows", "list available skills",
      "Lists skills discovered in <home>/skills and ./.agent/skills, with an active "
      "(●) / inactive (○) marker, source and description." },
    { "/skill", "", "<name> | off <name>", "Skills & workflows", "activate/deactivate a skill",
      "`/skill <name>` activates a skill's instructions for the session (injected "
      "into the system prompt, surviving /compact). `/skill off <name>` deactivates. "
      "The model can also load one itself via the use_skill tool." },
    { "/autoresume", "", "[on|off]", "Skills & workflows",
      "auto-feed a finished workflow's results to the model",
      "When a background workflow finishes, automatically resume the conversation so "
      "the model reads its results and continues — instead of the results only folding "
      "in on your next message. Bounded to 2 auto-turns per real message. Also the "
      "\"workflow resume\" row in /settings." },
    { "/workflows", "", "[id | cancel <id> | retry <id> | steer <id> <prompt>]", "Skills & workflows",
      "(claude) background workflow runs",
      "Lists background workflow runs the model started (via run_workflow). With an "
      "id, shows its steps; `cancel <id>` stops a run, `retry <id>` relaunches a "
      "finished one keeping succeeded steps, and `steer <id> <prompt>` injects guidance." },

    // ── Session & UI ─────────────────────────────────────────────────────
    { "/settings", "", "[<key> <value>]", "Session & UI", "open or set settings",
      "With no argument opens the interactive settings menu. With `<key> <value>` "
      "sets one directly (profile, plan, steering_mode, steer, model, tools, thinking, "
      "context, auto_compact, autoresume, redact_secrets, max_tokens, tool_call_limit, …)." },
    { "/bell", "", "[never|ask_user|question|attention|always]", "Session & UI",
      "when the terminal bell rings",
      "Controls the terminal bell. always: on every answer plus anything needing you. "
      "attention: only a workflow finishing, a tool-permission prompt, or an answer that "
      "is a question. question: only when the answer is a question. never: silent. At any "
      "level except never, a DANGEROUS command's confirmation always rings. Bare /bell "
      "opens a picker; also the \"bell\" row in /settings." },
    { "/jobs", "", "[id | stop <id|all>]", "Session & UI", "background commands the agent started",
      "Lists background jobs started with run_command(background:true) — a dev server, "
      "watcher, or tail -f — with running/exited status and runtime. `/jobs <id>` shows "
      "a job's captured output (scrollable); `/jobs stop <id|all>` stops jobs. The model "
      "inspects the same jobs via the check_job tool." },
    { "/limits", "", "", "Session & UI", "rate-limit / quota headers from the last response",
      "Shows the rate-limit and quota headers the provider returned on the last "
      "request (e.g. requests/tokens remaining and reset times). Providers vary; "
      "subscription providers often don't expose quota this way, in which case it "
      "reports none were seen." },
    { "/raw", "", "[request|response]", "Session & UI", "inspect the last model request/response",
      "Shows the exact JSON request last sent to the provider and the response "
      "received (assembled from the stream when streaming), in a scrollable view — "
      "for debugging prompts, tools and provider quirks. `/raw request` or `/raw "
      "response` shows just one. Auth headers are not part of the body shown." },
    { "/theme", "", "<dark|light|warm|cool|rose|custom>", "Session & UI", "switch the colour theme",
      "Changes the terminal colour theme (dark, light, warm, cool, rose). Persisted "
      "across sessions. Never sets the terminal background.\n\n"
      "`custom` is your own palette: in the config file set a base with "
      "`theme_base: cool` and override individual roles with `theme.<role>: <colour>` "
      "— e.g. `theme.ai: #7aa2f7`, `theme.dim: 244`, `theme.warn: amber`. Roles: "
      "user, ai, command, dim, accent, danger, warn, kw, str, num, type. Colours: a "
      "256-colour index (0-255), a hex triplet (#rrggbb), or a colour name. `custom` "
      "is offered only once at least one override exists; bare `/theme` prints the "
      "active overrides in their own colours." },
    { "/help", "", "[command]", "Session & UI", "list commands, or help for one",
      "With no argument lists all commands grouped by area. With a command name "
      "(with or without the leading /) shows detailed help for just that command." },
    { "/about", "/info", "", "Session & UI", "app version, provider/model",
      "Shows the version, a one-line description, and the current provider/model and "
      "any loaded project-instructions file." },
    { "/exit", "/quit", "", "Session & UI", "leave the app",
      "Exits the REPL. Settings and the conversation are saved on the way out." },

    // ── Not a slash command, but part of the input language ──────────────
    { "!<command>", "", "", "Input shortcuts", "run a shell command yourself",
      "A line starting with `!` runs the rest as a shell command directly — no model "
      "turn, no confirmation (you typed it). The output prints locally AND is "
      "recorded, so `!make test` then \"fix those\" works. Not blocked by /plan." },
    { "/paste", "", "[n]", "Input shortcuts", "review large pastes sent this session",
      "A large paste collapses to a placeholder and its transcript echo is trimmed to "
      "the paste-preview length. /paste lists the large pastes you've sent this session "
      "(Enter opens the full text, scrollable and wrapped); /paste <n> jumps to one." },
    { "@path", "", "", "Input shortcuts", "attach a file to your message",
      "`@src/foo.cpp` inside a message expands the file inline (when the token starts "
      "with @ and the path exists — emails and @decorators are left alone). Size-"
      "capped; Tab completes @-paths." },
    };
    return cat;
}

std::string commands_overview() {
    const auto& cat = command_catalog();
    std::string out = "commands (use /help <command> for details):\n";
    std::string group;
    for ( const auto& c : cat ) {
        if ( c.group != group ) {
            group = c.group;
            out += "\n" + group + ":\n";
        }
        std::string left = c.name + ( c.usage.empty() ? "" : " " + c.usage );
        // Pad to a column for readability (best-effort).
        if ( left.size() < 26 )
            left += std::string(26 - left.size(), ' ');
        out += "  " + left + c.summary + "\n";
    }
    out += "\n/exit or /quit to leave. `!cmd` runs a shell command; `@path` attaches a file.";
    return out;
}

std::string command_help(const std::string& query) {
    std::string q = common::trim_ws(query);
    if ( q.empty())
        return "";
    if ( q[0] != '/' && q[0] != '!' && q[0] != '@' )
        q = "/" + q;
    q = common::to_lower(q);

    for ( const auto& c : command_catalog()) {
        bool match = common::to_lower(c.name) == q;
        if ( !match && !c.aliases.empty()) {
            std::istringstream as(c.aliases);
            std::string a;
            while ( std::getline(as, a, ',')) {
                if ( common::to_lower(common::trim_ws(a)) == q ) { match = true; break; }
            }
        }
        if ( !match )
            continue;
        std::string out = c.name + ( c.usage.empty() ? "" : " " + c.usage );
        if ( !c.aliases.empty())
            out += "   (alias: " + c.aliases + ")";
        out += "\n\n" + c.detail;
        return out;
    }
    return "";
}

std::string commands_markdown() {
    const auto& cat = command_catalog();
    std::string out =
        "# Commands\n\n"
        "Slash commands available in the interactive REPL. In-app, `/help` lists these "
        "and `/help <command>` shows the detail for one. This file is generated from the "
        "same catalogue (`src/commands.cpp`).\n";
    std::string group;
    for ( const auto& c : cat ) {
        if ( c.group != group ) {
            group = c.group;
            out += "\n## " + group + "\n\n";
        }
        out += "### `" + c.name + ( c.usage.empty() ? "" : " " + c.usage ) + "`";
        if ( !c.aliases.empty())
            out += "  — alias `" + c.aliases + "`";
        out += "\n\n" + c.detail + "\n\n";
    }
    return out;
}

} // namespace agent
