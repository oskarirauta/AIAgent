#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <filesystem>

namespace agent::tools {

// Remembers the on-disk stamp (mtime + size) of files the model has read, so a
// later write/edit can detect that the file changed underneath it and refuse to
// silently clobber those changes. Shared between read_file, write_file and
// edit_file. Thread-safe (read-only tool batches run concurrently).
class FileTracker {
public:
    // Record the current stamp of `path` (after a successful read or write).
    void note(const std::string& path) {
        std::error_code ec;
        std::string key = canon(path);
        auto t = std::filesystem::last_write_time(path, ec);
        if ( ec ) return;
        std::error_code ec2;
        auto sz = std::filesystem::file_size(path, ec2);
        if ( ec2 ) return;
        std::lock_guard<std::mutex> lk(_mx);
        _seen[key] = { t.time_since_epoch().count(), sz };
    }

    // Non-empty reason if the file was read before AND has since changed on disk
    // (a lost-update risk). Empty when it was never read or is unchanged.
    std::string stale_reason(const std::string& path) const {
        std::string key = canon(path);
        std::lock_guard<std::mutex> lk(_mx);
        auto it = _seen.find(key);
        if ( it == _seen.end())
            return ""; // never read — not a lost-update case, allow the write
        std::error_code ec;
        auto t = std::filesystem::last_write_time(path, ec);
        if ( ec ) return "";
        std::error_code ec2;
        auto sz = std::filesystem::file_size(path, ec2);
        if ( ec2 ) return "";
        if ( t.time_since_epoch().count() != it->second.mtime || sz != it->second.size )
            return "the file changed on disk since you last read it — re-read it before "
                   "editing so your change is based on the current version (this avoids "
                   "silently overwriting edits made in the meantime)";
        return "";
    }

    // Forget a file (e.g. after it was reverted) so the next read re-stamps it.
    void forget(const std::string& path) {
        std::lock_guard<std::mutex> lk(_mx);
        _seen.erase(canon(path));
    }

private:
    struct Stamp { long long mtime; std::uintmax_t size; };

    static std::string canon(const std::string& path) {
        std::error_code ec;
        auto p = std::filesystem::weakly_canonical(path, ec);
        return ec ? path : p.string();
    }

    mutable std::mutex _mx;
    std::map<std::string, Stamp> _seen;
};

} // namespace agent::tools
