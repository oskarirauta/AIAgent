#pragma once

#include <string>
#include <sys/stat.h>
#include "logger.hpp"

namespace agent::auth {

// A credential file must be readable only by its owner. If it is group- or
// other-accessible (someone loosened it, or a bad umask created it), tighten it
// to 0600 on read and warn — closing the exposure window instead of silently
// trusting whatever mode it happens to have.
inline void ensure_owner_only(const std::string& path, const char* who) {
    struct stat st{};
    if ( ::stat(path.c_str(), &st) != 0 )
        return;
    if ( st.st_mode & 0077 ) {
        logger::warning[who] << "credential file " << path
            << " was group/other-accessible; tightening to 0600" << std::endl;
        ::chmod(path.c_str(), 0600);
    }
}

} // namespace agent::auth
