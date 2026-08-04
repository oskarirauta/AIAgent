#pragma once

#include <cctype>
#include <map>
#include <string>
#include <vector>

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

// "cool" — a dark-terminal palette built from blues and greens (the coolest end of
// the spectrum). Blue speaker + aquamarine assistant, teal-gray chrome, aqua accent;
// warnings stay amber so they still stand out against the cool tones.
inline Theme theme_cool() {
    Theme t;
    t.name    = "cool";
    t.user    = "\033[38;5;75m";  // sky blue
    t.ai      = "\033[38;5;79m";  // aquamarine
    t.command = "\033[38;5;68m";  // steel blue
    t.dim     = "\033[38;5;66m";  // slate teal-gray
    t.accent  = "\033[38;5;80m";  // bright aqua
    t.danger  = "\033[38;5;174m"; // muted salmon (kept red-ish for recognisability)
    t.warn    = "\033[38;5;179m"; // amber (stands out against the blues)
    t.kw      = "\033[38;5;69m";  // periwinkle
    t.str     = "\033[38;5;72m";  // sea green
    t.num     = "\033[38;5;80m";  // aqua
    t.type    = "\033[38;5;111m"; // light blue
    return t;
}

// "rose" — a dark-terminal palette of muted mauves, dusty pinks and orchid, distinct
// from the green/amber/blue themes. Green strings keep code readable.
inline Theme theme_rose() {
    Theme t;
    t.name    = "rose";
    t.user    = "\033[38;5;175m"; // dusty rose
    t.ai      = "\033[38;5;139m"; // mauve
    t.command = "\033[38;5;96m";  // muted plum
    t.dim     = "\033[38;5;103m"; // lavender-gray
    t.accent  = "\033[38;5;176m"; // orchid
    t.danger  = "\033[38;5;167m"; // soft red
    t.warn    = "\033[38;5;179m"; // amber
    t.kw      = "\033[38;5;133m"; // magenta-purple
    t.str     = "\033[38;5;108m"; // green (readable strings)
    t.num     = "\033[38;5;173m"; // coral
    t.type    = "\033[38;5;175m"; // rose
    return t;
}

inline Theme theme_by_name(const std::string& name) {
    if ( name == "light" ) return theme_light();
    if ( name == "warm" )  return theme_warm();
    if ( name == "cool" )  return theme_cool();
    if ( name == "rose" )  return theme_rose();
    return theme_dark(); // default
}

// ── custom theme ─────────────────────────────────────────────────────────
// The five built-in palettes get most of the way there, but "everything fits
// except one colour" is a real complaint — so a custom theme starts from a base
// palette and overrides individual ROLES. A colour is written as a 256-colour
// index (0-255), a hex triplet (#7aa2f7 → truecolour), or a basic colour name;
// only the foreground is ever set, so the terminal background still shows through.

// Translate a user-written colour into an SGR foreground sequence.
// Returns "" when the spec is not understood (the caller keeps the base colour).
inline std::string theme_color_sgr(const std::string& spec) {
    // trim
    size_t b = 0, e = spec.size();
    while ( b < e && std::isspace(static_cast<unsigned char>(spec[b]))) ++b;
    while ( e > b && std::isspace(static_cast<unsigned char>(spec[e - 1]))) --e;
    std::string s = spec.substr(b, e - b);
    if ( s.empty())
        return "";

    // #rrggbb / rrggbb (6 hex digits) -> truecolour
    std::string hex = ( s[0] == '#' ) ? s.substr(1) : s;
    if ( hex.size() == 6 ) {
        bool all_hex = true;
        for ( char c : hex )
            if ( !std::isxdigit(static_cast<unsigned char>(c))) { all_hex = false; break; }
        if ( all_hex ) {
            auto byte = [&hex](size_t i) {
                return std::stoi(hex.substr(i, 2), nullptr, 16);
            };
            return "\033[38;2;" + std::to_string(byte(0)) + ";" +
                   std::to_string(byte(2)) + ";" + std::to_string(byte(4)) + "m";
        }
    }

    // A plain number -> 256-colour index.
    bool all_digits = true;
    for ( char c : s )
        if ( !std::isdigit(static_cast<unsigned char>(c))) { all_digits = false; break; }
    if ( all_digits ) {
        int n = 0;
        try { n = std::stoi(s); } catch ( ... ) { return ""; }
        if ( n < 0 || n > 255 )
            return "";
        return "\033[38;5;" + std::to_string(n) + "m";
    }

    // Basic colour names, mapped onto the 256-colour cube so they land on the
    // same muted register as the built-in palettes rather than raw ANSI brights.
    std::string lo;
    for ( char c : s ) lo += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::map<std::string, int> names = {
        { "black", 0 }, { "red", 167 }, { "green", 108 }, { "yellow", 179 },
        { "amber", 179 }, { "blue", 75 }, { "magenta", 139 }, { "purple", 140 },
        { "mauve", 139 }, { "cyan", 109 }, { "teal", 72 }, { "aqua", 80 },
        { "orange", 173 }, { "pink", 175 }, { "rose", 175 }, { "gray", 242 },
        { "grey", 242 }, { "white", 252 }
    };
    auto it = names.find(lo);
    if ( it != names.end())
        return "\033[38;5;" + std::to_string(it->second) + "m";
    return "";
}

// Every overridable role name, in the order the /theme help lists them.
inline const std::vector<std::string>& theme_roles() {
    static const std::vector<std::string> roles = {
        "user", "ai", "command", "dim", "accent", "danger", "warn",
        "kw", "str", "num", "type"
    };
    return roles;
}

// Is `role` an overridable role name? (For validating config keys.)
inline bool theme_role_name_valid(const std::string& role) {
    for ( const auto& r : theme_roles())
        if ( r == role )
            return true;
    return false;
}

// The role names as one comma-separated string, for help/error text.
inline std::string theme_role_list() {
    std::string s;
    for ( const auto& r : theme_roles())
        s += ( s.empty() ? "" : ", " ) + r;
    return s;
}

// Point at a role's slot in a Theme, or nullptr for an unknown role.
inline std::string* theme_role_slot(Theme& t, const std::string& role) {
    if ( role == "user" )    return &t.user;
    if ( role == "ai" )      return &t.ai;
    if ( role == "command" ) return &t.command;
    if ( role == "dim" )     return &t.dim;
    if ( role == "accent" )  return &t.accent;
    if ( role == "danger" )  return &t.danger;
    if ( role == "warn" )    return &t.warn;
    if ( role == "kw" )      return &t.kw;
    if ( role == "str" )     return &t.str;
    if ( role == "num" )     return &t.num;
    if ( role == "type" )    return &t.type;
    return nullptr;
}

// Build the custom theme: `base` palette with `overrides` (role -> colour spec)
// applied on top. An unknown role or unparseable colour is skipped, so a typo
// costs one colour rather than the whole theme.
inline Theme theme_custom(const std::string& base,
                          const std::map<std::string, std::string>& overrides) {
    Theme t = theme_by_name(base == "custom" ? "dark" : base);
    for ( const auto& [role, spec] : overrides ) {
        std::string* slot = theme_role_slot(t, role);
        if ( !slot )
            continue;
        std::string sgr = theme_color_sgr(spec);
        if ( !sgr.empty())
            *slot = sgr;
    }
    t.name = "custom";
    return t;
}

} // namespace agent
