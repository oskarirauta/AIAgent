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
    { "/history", "", "", "Conversation", "list the messages in the current context",
      "Shows each non-system message as `you/ai/tool: <first line>`, so you can see "
      "what the model currently has in context." },
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
    { "/memories", "", "[name]", "Context & cost", "list or view this provider's memories",
      "Long-term memory is per provider. With no argument, lists the memory files; "
      "with a name, prints that file's content." },
    { "/tasks", "", "", "Context & cost", "show the agent's todo list",
      "Shows the todo list the model maintains for multi-step work (via the "
      "update_tasks tool), with ✓/▸/○ status glyphs." },
    { "/queue", "", "[drop <n|all>]", "Context & cost", "messages queued behind the running turn",
      "Lists messages you typed while a turn was running (they auto-send when it "
      "finishes). `/queue drop <n|all>` removes queued entries." },

    // ── Providers & models ───────────────────────────────────────────────
    { "/provider", "", "[name]", "Providers & models", "switch provider mid-session",
      "Switches the active provider (openai, ollama, anthropic, moonshot, "
      "openrouter, kimi, claude), carrying the conversation over and restoring that "
      "provider's remembered model. Subscription providers must already be logged in." },
    { "/model", "", "[name]", "Providers & models", "show or change the model",
      "With no argument shows the current model. With a name switches to it and "
      "remembers it for this provider across sessions." },
    { "/thinking", "/effort", "<off|on|low|medium|high|xhigh|max>", "Providers & models",
      "set the reasoning/thinking level",
      "Controls extended thinking / reasoning effort. Honoured by claude, anthropic, "
      "kimi, openai and openrouter (mapped to each API's field). Persisted across "
      "sessions." },
    { "/stream", "", "<off|on|collapse>", "Providers & models", "live reasoning display",
      "Whether to stream the model's reasoning live. `collapse` streams it then "
      "hides it once the answer is done, leaving only the answer in the transcript." },

    // ── Tools & safety ───────────────────────────────────────────────────
    { "/tools", "", "<confirm|auto|insecure>", "Tools & safety", "tool confirmation mode",
      "confirm: ask before mutating tools (read-only run freely). auto: run "
      "ordinary tools without asking; danger-listed commands still warn. insecure: "
      "run everything without asking. Not persisted — resets each session." },
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
    { "/mcp", "", "[refresh|prompt <server> <name> [k=v]]", "Tools & safety",
      "MCP servers, tools, resources, prompts",
      "Shows configured MCP servers and their tools/resources/prompts. `refresh` "
      "reconnects; `prompt <server> <name> [k=v]` loads a server prompt into context." },
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
    { "/workflows", "", "[id | cancel <id> | retry <id>]", "Skills & workflows",
      "(claude) background workflow runs",
      "Lists background workflow runs the model started (via run_workflow). With an "
      "id, shows its steps; `cancel <id>` stops a run, `retry <id>` relaunches a "
      "finished one keeping succeeded steps." },

    // ── Session & UI ─────────────────────────────────────────────────────
    { "/settings", "", "[<key> <value>]", "Session & UI", "open or set settings",
      "With no argument opens the interactive settings menu. With `<key> <value>` "
      "sets one directly (model, tools, thinking, theme, context, multiline, "
      "auto_compact, autoresume, max_tokens, tool_call_limit, paste_preview, …)." },
    { "/theme", "", "<dark|light|warm>", "Session & UI", "switch the colour theme",
      "Changes the terminal colour theme. Persisted across sessions. Never sets the "
      "terminal background." },
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
