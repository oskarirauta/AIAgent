#pragma once

namespace agent {

// The single source of truth for the application version. Bump this on a
// release; it flows to `--help`, `/about` and the MCP clientInfo handshake.
inline constexpr const char* VERSION = "2.0.0";

} // namespace agent
