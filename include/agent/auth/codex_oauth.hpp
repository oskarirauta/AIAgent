#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "agent/api/client.hpp"

namespace agent::auth {

struct CodexToken {
    std::string access_token;
    std::string refresh_token;
    std::string id_token;
    std::string account_id;
    std::int64_t last_refresh = 0;
};

// Codex CLI owns this credential file. AIAgent deliberately shares it so a
// `codex login` is enough for both clients and no second OAuth identity exists.
std::string codex_auth_path();
std::optional<CodexToken> load_codex_token();
bool codex_token_needs_refresh(const CodexToken& token, std::int64_t max_age_seconds = 3000);
CodexToken refresh_codex_token(api::Client& client, const CodexToken& token);
void save_codex_token(const CodexToken& token);
CodexToken login_codex_device(api::Client& client);

} // namespace agent::auth
