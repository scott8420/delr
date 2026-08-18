#pragma once
#include "core/Broker.hpp"

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// core/RosterImport -- the CPPA registry becomes the roster.
//
// Every handoff since s02 said the roster was gated on research hours nobody
// had. That stopped being true when California started compelling the list.
// Registrants must file annually, and the filing carries a name, a website, a
// contact email and a rights URL -- 543 rows in the 2025 file with all four
// present on every one of them. The roster is no longer a research project; it
// is a parsing problem.
//
// ── Why this is a module and not a script ────────────────────────────────────
// A Python script would have produced brokers.json faster. It would also have
// produced it ONCE, off to the side, unversioned and unchecked, and the file
// refreshes every year. Parsing here means the transformation is in the tree,
// has checks, and runs from the binary the user already has -- which matters
// for a local-first tool, because it means nobody has to trust a roster we
// published. They can hold the state's CSV and build it themselves.
//
// ── Pure ─────────────────────────────────────────────────────────────────────
// Text in, Roster out. No file handles, no clock, no network -- `--import-
// registry` reads the file and hands the bytes down. Same rule the rest of
// core/ lives under, same reason: the checks reach it.
//
// ── No regex ─────────────────────────────────────────────────────────────────
// PageRules refuses regex over network-sourced text because a backtracking
// bomb in a data file is a hang the user cannot diagnose. A CSV downloaded
// from a state website is the same class of input and gets the same answer: a
// hand-written state machine, linear, no backtracking, no library.
//
// ── NOTE: no PII ─────────────────────────────────────────────────────────────
// Everything here is public knowledge about companies, and the report is safe
// to print in full. That is not true of anything downstream of it.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::core {

// ── CSV ──────────────────────────────────────────────────────────────────────
// RFC 4180 as actually encountered: quoted fields, embedded commas, embedded
// newlines (the registry has them -- one narrative cell runs to five lines),
// doubled quotes, CRLF, and a UTF-8 BOM on the first cell. Ragged rows are
// returned ragged rather than padded; the column mapper handles short rows,
// because a row that is short is a fact about the data and padding it hides it.
using CsvRow   = std::vector<std::string>;
using CsvTable = std::vector<CsvRow>;

CsvTable csv_parse(const std::string& text);

// ── The report ───────────────────────────────────────────────────────────────
// An importer that says "543 brokers imported" and nothing else is an importer
// that silently dropped the seven rows that mattered. Every judgement it makes
// is counted, and everything it could not do lands in `notes` as a sentence.
struct ImportReport {
    int rows_read       = 0;   // registry rows with content
    int rows_skipped    = 0;   // ...that produced no broker, and why is in notes
    int hosts_total     = 0;   // listing domains across all entries
    int multi_host_rows = 0;   // registrants publishing under more than one
    int shared_host_rows = 0;  // ...filing a domain an earlier registrant claimed

    int merged_new      = 0;   // registrants the roster had never seen
    int merged_updated  = 0;   // matched an existing entry; its id survived
    int merged_kept     = 0;   // in the roster, absent from this registry -- KEPT
    int merged_renamed  = 0;   // matched, but the registrant filed a new name

    std::vector<std::string> notes;

    // Non-empty means nothing usable came out -- wrong file, no header row.
    // Distinct from an empty roster with notes, which means the file parsed
    // and said nothing.
    std::string failure;
};

// ── Import ───────────────────────────────────────────────────────────────────
// The registry as a roster, in registry terms only: no ids reconciled against
// anything, `method` set to Email throughout (see below), notes left empty.
// Feed the result to roster_merge() before saving it anywhere.
//
// METHOD IS EMAIL FOR EVERY ROW, and that is a deliberate flattening of a
// choice the enum cannot yet express. Each registrant files BOTH a contact
// email and a rights URL, so every one of them has two channels and `Method`
// holds one. Email wins because it is the channel this program will actually
// send on: it works from any state, it needs no form to be scraped, and it
// leaves a dated artefact -- which is the whole evidentiary point. The rights
// URL is kept in `opt_out_url` and loses nothing by riding along.
Roster registry_parse(const std::string& csv_text, ImportReport& rep);

// ── Merge ────────────────────────────────────────────────────────────────────
// The importer runs again every year, and the second run is the one that can
// do damage. Cases point at broker ids. Re-minting an id because a registrant
// added a comma to its legal name orphans every case filed against it, and the
// user's evidence that they filed becomes evidence about nobody.
//
// So the merge matches on HOSTS, not on names and not on ids. A company's
// domain is the most stable thing it owns and it is also what the matcher
// compares, so it is the right key twice over. A matched entry keeps its id
// whatever else changed.
//
// It NEVER deletes. An entry in the roster that this registry does not mention
// -- a deregistration, a hand-added broker, the DROP pseudo-entry -- survives
// untouched and is counted in `merged_kept`. Same reasoning as the s12
// migration leaving the older file behind: removing something the user may be
// mid-conversation with is not a call this function gets to make quietly.
//
// Registry facts (name, site, hosts, email, rights URL, the flags) are taken
// from `incoming`. Human facts (`notes`, `requires_id`, `recheck_days`) are
// left alone -- the registry does not know them and must not blank them.
// Hosts are UNIONED rather than replaced, because a host that disappears from
// a filing has not necessarily disappeared from the web, and a case may point
// at it.
Roster roster_merge(const Roster& existing, const Roster& incoming,
                    ImportReport& rep);

// ── Exposed for the checks ───────────────────────────────────────────────────

// "https://www.BeenVerified.com/x" -> "beenverified.com". Delegates to
// url_host() so the importer mints hosts in EXACTLY the form the matcher
// compares -- a roster normalised one way and searched another is a lookup
// table that quietly never hits.
std::string import_host_of(const std::string& url);

// One registry cell into the URLs inside it. Registrants list their whole
// estate in one field, separated by semicolons, newlines, or nothing but a
// space.
std::vector<std::string> split_urls(const std::string& cell);

// A slug from a host: "beenverified.com" -> "beenverified". The last label is
// dropped because a TLD identifies nobody. Collisions are real -- eight
// registrants share transunion.com -- and are resolved deterministically
// against `taken`, so the same CSV always mints the same ids.
std::string mint_id(const std::string& host, const std::string& name,
                    const std::vector<std::string>& taken);

}  // namespace delr::core
