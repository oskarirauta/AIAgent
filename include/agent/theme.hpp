#pragma once

#include <string>

namespace agent {

// A colour theme is just a set of foreground SGR sequences per UI role. The
// background is never set — the terminal's own background is respected — so
// "dark" is a palette tuned for dark terminals and "light" for light ones.
//
// Palettes favour muted 256-colour tones (no pure white, no over-saturated
// brights) to stay comfortable over long sessions; "warm" additionally avoids
// blue/cyan, which is the harshest part of the spectrum for tired eyes.
struct Theme {
    std::string name;

    // Speaker / message markers.
    std::string user;     // › (you)
    std::string ai;       // ● (assistant)
    std::string command;  // ⚙ (slash command / system)

    // Chrome.
    std::string dim;      // separators, status line, glyphs, paste frame, menu, hints
    std::string accent;   // spinner / activity indicator

    // Alerts.
    std::string danger;   // dangerous-command warnings
    std::string warn;     // confirmation prompts, queued count

    // Syntax highlighting.
    std::string kw;       // keywords
    std::string str;      // strings
    std::string num;      // numbers
    std::string type;     // types
    // comments/fences reuse `dim`.

    static constexpr const char* reset = "\033[0m";
};

inline Theme theme_dark() {
    Theme t;
    t.name    = "dark";
    t.user    = "\033[38;5;109m"; // soft cyan-gray
    t.ai      = "\033[38;5;108m"; // muted green
    t.command = "\033[38;5;139m"; // muted mauve
    t.dim     = "\033[38;5;242m"; // mid gray
    t.accent  = "\033[38;5;114m"; // gentle green-cyan
    t.danger  = "\033[38;5;174m"; // muted salmon-red
    t.warn    = "\033[38;5;179m"; // soft amber
    t.kw      = "\033[38;5;140m"; // muted purple
    t.str     = "\033[38;5;108m"; // green
    t.num     = "\033[38;5;179m"; // amber
    t.type    = "\033[38;5;109m"; // cyan
    return t;
}

inline Theme theme_light() {
    Theme t;
    t.name    = "light";
    t.user    = "\033[38;5;30m";  // dark teal
    t.ai      = "\033[38;5;28m";  // dark green
    t.command = "\033[38;5;90m";  // dark purple
    t.dim     = "\033[38;5;244m"; // gray that still reads on white
    t.accent  = "\033[38;5;31m";  // teal-blue
    t.danger  = "\033[38;5;124m"; // dark red
    t.warn    = "\033[38;5;130m"; // dark amber
    t.kw      = "\033[38;5;90m";  // purple
    t.str     = "\033[38;5;28m";  // green
    t.num     = "\033[38;5;130m"; // amber
    t.type    = "\033[38;5;30m";  // teal
    return t;
}

inline Theme theme_warm() {
    Theme t;
    t.name    = "warm";
    t.user    = "\033[38;5;179m"; // amber
    t.ai      = "\033[38;5;108m"; // soft green
    t.command = "\033[38;5;173m"; // terracotta
    t.dim     = "\033[38;5;242m"; // gray (neutral)
    t.accent  = "\033[38;5;179m"; // amber
    t.danger  = "\033[38;5;167m"; // soft red
    t.warn    = "\033[38;5;179m"; // amber
    t.kw      = "\033[38;5;173m"; // orange
    t.str     = "\033[38;5;108m"; // green
    t.num     = "\033[38;5;179m"; // amber
    t.type    = "\033[38;5;144m"; // warm khaki
    return t;
}

inline Theme theme_by_name(const std::string& name) {
    if ( name == "light" ) return theme_light();
    if ( name == "warm" )  return theme_warm();
    return theme_dark(); // default
}

} // namespace agent
