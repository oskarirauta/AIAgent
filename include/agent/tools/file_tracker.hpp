#pragma once

#include <cstdint>
#include <fstream>
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
        auto h = hash_file(path);
        std::lock_guard<std::mutex> lk(_mx);
        _seen[key] = { static_cast<long long>(t.time_since_epoch().count()), sz, h.first, h.second };
    }

    // Record the version just written by this session. The content hash makes the
    // stale-read guard tolerant of harmless mtime-only changes/noise after our own
    // atomic rename while still detecting real content changes before a later edit.
    void note_content(const std::string& path, const std::string& content) {
        std::error_code ec;
        std::string key = canon(path);
        auto t = std::filesystem::last_write_time(path, ec);
        long long mt = ec ? 0 : static_cast<long long>(t.time_since_epoch().count());
        std::lock_guard<std::mutex> lk(_mx);
        _seen[key] = { mt, static_cast<std::uintmax_t>(content.size()), hash_string(content), true };
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
        long long mt = static_cast<long long>(t.time_since_epoch().count());
        bool stamp_differs = ( mt != it->second.mtime || sz != it->second.size );
        if ( it->second.has_hash ) {
            auto h = hash_file(path);
            if ( h.second ) {
                if ( h.first == it->second.hash )
                    return ""; // content is unchanged; tolerate mtime/stat noise
                return "the file changed on disk since you last read it — re-read it before "
                       "editing so your change is based on the current version (this avoids "
                       "silently overwriting edits made in the meantime)";
            }
        }
        if ( stamp_differs )
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
    struct Stamp { long long mtime; std::uintmax_t size; std::uint64_t hash; bool has_hash; };

    static std::uint64_t hash_string(const std::string& s) {
        std::uint64_t h = 1469598103934665603ull; // FNV-1a
        for ( unsigned char c : s ) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }

    static std::pair<std::uint64_t, bool> hash_file(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if ( !in.is_open())
            return { 0, false };
        std::uint64_t h = 1469598103934665603ull;
        char buf[8192];
        while ( in ) {
            in.read(buf, sizeof(buf));
            std::streamsize n = in.gcount();
            for ( std::streamsize i = 0; i < n; ++i ) {
                h ^= static_cast<unsigned char>(buf[i]);
                h *= 1099511628211ull;
            }
        }
        return { h, true };
    }

    static std::string canon(const std::string& path) {
        std::error_code ec;
        auto p = std::filesystem::weakly_canonical(path, ec);
        return ec ? path : p.string();
    }

    mutable std::mutex _mx;
    std::map<std::string, Stamp> _seen;
};

} // namespace agent::tools
