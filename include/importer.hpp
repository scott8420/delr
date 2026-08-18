#pragma once
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// `delr --import-registry <csv> [<out.json>]` -- the third non-GUI mode.
//
// In the idiom `--selftest` and `--netcheck` established, and for the same
// three reasons: it needs no display, it can be run by hand and watched, and
// what it did lands on a terminal where a person can read it rather than in a
// window that has to be built first.
//
// This file is the I/O half and nothing else -- read a file, print a report,
// write a file. Every judgement belongs to core/RosterImport, which is pure
// and checked. The split is the same one the whole program is built on.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::importer {

// Returns a process exit code. Non-zero means nothing was written.
int run(const std::string& csv_path, const std::string& out_path);

}  // namespace delr::importer
