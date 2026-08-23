#include "Paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace delr::paths {
namespace {

// An environment variable that is SET BUT EMPTY is not an answer. `DELR_CASES=`
// in a shell profile would otherwise resolve to the empty path and every save
// would fail at open() with no clue why.
const char* env_or_null(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return nullptr;
    return v;
}

std::string join(const std::string& dir, const char* leaf) {
    if (dir.empty()) return {};
    return (fs::path(dir) / leaf).string();
}

}  // namespace

// ── assets ──────────────────────────────────────────────────────────────────

std::string asset_dir() {
    if (const char* e = env_or_null("DELR_ASSETS")) return e;
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    // A working directory we cannot read is not a reason to invent one; a
    // relative path still resolves against wherever the process actually is.
    if (ec) return "data";
    return (cwd / "data").string();
}

std::string roster_file() {
    if (const char* e = env_or_null("DELR_ROSTER")) return e;
    return join(asset_dir(), "brokers.json");
}

std::string rules_file() {
    if (const char* e = env_or_null("DELR_RULES")) return e;
    return join(asset_dir(), "pagerules.json");
}

// ── state ───────────────────────────────────────────────────────────────────

std::string state_dir() {
    if (const char* e = env_or_null("DELR_STATE")) return e;
    // XDG first and HOME second, which is the order the spec asks for and also
    // the order that lets a user relocate state without moving their home.
    if (const char* x = env_or_null("XDG_DATA_HOME"))
        return (fs::path(x) / "delr").string();
    if (const char* h = env_or_null("HOME"))
        return (fs::path(h) / ".local" / "share" / "delr").string();
    return {};  // see the header: there is no safe fourth guess
}

std::string cases_file() {
    if (const char* e = env_or_null("DELR_CASES")) return e;
    return join(state_dir(), "cases.json");
}

std::string egress_file() {
    if (const char* e = env_or_null("DELR_EGRESS")) return e;
    return join(state_dir(), "egress.json");
}

std::string profile_file() {
    if (const char* e = env_or_null("DELR_PROFILE")) return e;
    return join(state_dir(), "profile.json");
}

std::string journal_file() {
    if (const char* e = env_or_null("DELR_JOURNAL")) return e;
    // `.ndjson`, not `.json`, and the extension is load-bearing rather than
    // decorative: a tool that opens this as JSON gets a parse error on line
    // two instead of a plausible-looking first record.
    return join(state_dir(), "journal.ndjson");
}

bool ensure_state_dir(std::string* error) {
    const std::string dir = state_dir();
    if (dir.empty()) {
        if (error)
            *error = "No state directory: neither DELR_STATE, XDG_DATA_HOME "
                     "nor HOME is set. Set one -- delr will not write your "
                     "caseload beside the program.";
        return false;
    }

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !fs::is_directory(dir)) {
        if (error) *error = "Could not create " + dir + ": " + ec.message();
        return false;
    }

    // 0700, every run. The files inside are 0600; a parent anyone can list
    // gives away that this user runs a removal tool and how many cases they
    // have, which is metadata about exactly the thing being protected.
    fs::permissions(dir,
                    fs::perms::owner_read | fs::perms::owner_write |
                        fs::perms::owner_exec,
                    fs::perm_options::replace, ec);
    // Not fatal: some filesystems have no opinion about permissions, and a
    // caseload that saves on an exFAT stick beats one that refuses to.
    return true;
}

// ── migration ───────────────────────────────────────────────────────────────

Migration migrate_one(const std::string& from, const std::string& to,
                      std::string* error) {
    if (from.empty() || to.empty()) return Migration::NothingToDo;

    std::error_code ec;
    if (!fs::exists(from, ec) || ec) return Migration::NothingToDo;
    if (fs::exists(to, ec)) return Migration::KeptTarget;

    fs::create_directories(fs::path(to).parent_path(), ec);

    // Rename first: atomic, and it preserves the mode, which for egress.json
    // means 0600 survives the move rather than being re-applied afterwards
    // with a window in between.
    ec.clear();
    fs::rename(from, to, ec);
    if (!ec) return Migration::Moved;

    // Cross-device is the ordinary reason rename fails -- a home directory on
    // a different mount from the checkout. Copy, then remove, and treat a
    // failed removal as a failure rather than a success, because the whole
    // point of this move is that the old copy stops existing.
    ec.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "could not move " + from + ": " + ec.message();
        return Migration::Failed;
    }
    fs::permissions(to, fs::status(from).permissions(),
                    fs::perm_options::replace, ec);
    ec.clear();
    if (!fs::remove(from, ec) || ec) {
        if (error)
            *error = "copied to " + to + " but could not remove " + from +
                     " -- delete it by hand; it is the copy inside the source "
                     "tree.";
        return Migration::Failed;
    }
    return Migration::Moved;
}

std::vector<std::string> migrate_legacy_state() {
    std::vector<std::string> said;

    // The legacy layout, spelled out rather than derived: these are historical
    // constants, not a default that should follow `asset_dir` if that moves.
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (ec) return said;
    const fs::path legacy = cwd / "data";

    struct Item { const char* leaf; std::string dest; const char* what; };
    const Item items[] = {
        {"egress.json", egress_file(), "tunnel policy"},
        {"cases.json",  cases_file(),  "caseload"},
    };

    for (const auto& it : items) {
        const std::string from = (legacy / it.leaf).string();
        std::string err;
        switch (migrate_one(from, it.dest, &err)) {
            case Migration::NothingToDo:
                break;
            case Migration::Moved:
                said.push_back(std::string("moved your ") + it.what +
                               " out of the source tree into " + it.dest);
                break;
            case Migration::KeptTarget:
                said.push_back(std::string("kept the ") + it.what + " already at " +
                               it.dest + "; the older copy at " + from +
                               " was left alone -- delete it when you have "
                               "checked you do not want it");
                break;
            case Migration::Failed:
                said.push_back(std::string("could not move your ") + it.what +
                               ": " + err);
                break;
        }
    }
    return said;
}

}  // namespace delr::paths
