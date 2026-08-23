#pragma once
#include <string>
#include <vector>

// Where files live. One module, because until s12 there were four answers in
// three files and they had already started to drift.
//
// ── THE DISTINCTION THIS MODULE EXISTS TO DRAW ──────────────────────────────
//
// Two kinds of file, and conflating them is what put a user's address in a
// public repository's working tree:
//
//   ASSETS are shipped with the app, downloaded, and read-only at runtime. The
//   broker roster and the page rules. They are the same on every machine, they
//   belong in git, and they live beside the program.
//
//   STATE is written by the running app and is about ONE person. The caseload
//   (which broker pages expose this user, by URL) and the egress policy (which
//   holds `naked_exit`: this machine's address with the tunnel DOWN). It is
//   different on every machine, it must never be in a commit, and it lives
//   under XDG in the user's home.
//
// Before s12 all four defaulted to `<cwd>/data/`, which is the source tree.
// `.gitignore` now names the state files as well, but that is a second line of
// defence and this module is the first: a file that is not in the tree cannot
// be committed by an ignore rule that someone edits later.
//
// ── WHY THIS IS NOT IN `core` ───────────────────────────────────────────────
//
// Core modules never learn what a desktop is (RULES). `core::caseload_load`
// takes a path and has no opinion about where paths come from; that is what
// makes it testable and what lets encryption-at-rest land behind it without
// this file changing. Resolving a path against a home directory is desktop
// knowledge, so it lives out here with the other desktop knowledge.
//
// ── WHY NO GLIB ─────────────────────────────────────────────────────────────
//
// `g_get_user_data_dir()` would do this, and it CACHES on first call. A test
// that sets `XDG_DATA_HOME` and asks again gets the answer from before the
// test started, which is a test that passes while proving nothing. Plain
// `getenv` has no memory, so the checks below can actually exercise these
// functions. It also keeps `--netcheck` honest about needing no toolkit.
namespace delr::paths {

// ── assets ──────────────────────────────────────────────────────────────────

// `DELR_ASSETS`, else `<cwd>/data`. Read-only at runtime.
std::string asset_dir();

// `DELR_ROSTER` / `DELR_RULES` win outright, else the file inside `asset_dir`.
// The per-file overrides are kept from the call sites they replaced: the
// selftest and the sandbox both point at one file without moving the others.
std::string roster_file();
std::string rules_file();

// ── state ───────────────────────────────────────────────────────────────────

// `DELR_STATE`, else `$XDG_DATA_HOME/delr`, else `$HOME/.local/share/delr`.
//
// EMPTY when none of the three is available, and that is deliberate. There is
// no fourth guess that is not worse: falling back to the working directory is
// the bug this module was written to remove, and falling back to /tmp puts an
// address somewhere world-listable. An empty answer makes `ensure_state_dir`
// refuse with a sentence, which is the same shape as `ExitUnpinned` refusing
// when there is nothing to compare against -- a refusal the user can act on
// beats a guess they cannot see.
std::string state_dir();

// `DELR_CASES` / `DELR_EGRESS` win outright, else the file inside `state_dir`.
// Empty when `state_dir` is empty, so a caller that skipped `ensure_state_dir`
// fails at the open() rather than writing somewhere surprising.
std::string cases_file();
std::string egress_file();

// `DELR_PROFILE` wins outright, else the file inside `state_dir`.
//
// The most sensitive file this program writes, and the reason it is listed
// third rather than first is that the other two already hold most of it: a
// listing URL carries a name and a state in its path. All three are state, all
// three are 0600, and none of them is ever in the source tree.
std::string profile_file();

// `DELR_JOURNAL` wins outright, else the file inside `state_dir`.
//
// State, and the fourth of them. Less sensitive than the other three by
// construction -- `core/Journal` carries no url, no note and no free text at
// all, precisely because this is the file most likely to be sent to somebody
// -- but it is still a record of one person's activity and it still never goes
// near the source tree.
//
// It is also the only state file that GROWS. The other three are rewritten
// whole; this one is appended to and never rewritten, which is why it is the
// one whose loss would be unrecoverable.
std::string journal_file();

// Creates `state_dir` if absent and forces mode 0700 whether it created it or
// not. Enforced on every run rather than only at creation, because a directory
// that got made with the wrong mode once stays wrong forever otherwise, and
// the files inside it are 0600 for a reason that a 0755 parent undermines.
//
// False with `*error` set when there is no state directory to make.
bool ensure_state_dir(std::string* error = nullptr);

// ── migration ───────────────────────────────────────────────────────────────

enum class Migration {
    NothingToDo,  // no legacy file; the overwhelmingly common case
    Moved,        // legacy file is now at the new path and gone from the old
    KeptTarget,   // both existed; the new one wins and the old one is LEFT
    Failed,       // something is on disk that should not be -- see the error
};

// Moves one file, and refuses to lose data doing it.
//
// `KeptTarget` does not delete the legacy file. A stale copy in the source tree
// is untidy and `.gitignore` covers it; deleting a file the user might want is
// not recoverable, and this function does not get to make that call silently.
// It is reported so the user can.
Migration migrate_one(const std::string& from, const std::string& to,
                      std::string* error = nullptr);

// The one-time move of `<cwd>/data/{egress,cases}.json` into the state
// directory. Returns a line per file that moved or failed -- nothing for the
// ordinary case, so a machine that never had the old layout stays silent.
//
// Runs before anything reads a file, because the alternative is an app that
// starts with an empty policy, tells the user to re-record a baseline, and
// requires them to physically disconnect their VPN to recover a file that was
// sitting on disk the whole time.
std::vector<std::string> migrate_legacy_state();

}  // namespace delr::paths
