#pragma once
#include "core/Broker.hpp"
#include "core/Case.hpp"

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// core/Intake -- turning a pasted URL into a case.
//
// Discovery is the user's job (see the README): they find the listing in their
// own browser and paste the URL here. Everything between that paste and a row
// in the caseload is decidable without a display, so it lives here rather than
// in the dialog: parse the URL, name the host, match a broker, mint an id,
// notice we already have this listing. The dialog's job shrinks to showing
// what this file concluded.
//
// GTK-free and headless-testable, like the rest of core/. The rule that put
// path resolution and the clock on the UI side puts them there again: `today`
// arrives as an argument.
//
// ── PII WARNING ──────────────────────────────────────────────────────────────
// Every function here handles a listing URL, and a listing URL is PII all by
// itself (`/John-Smith/Tennessee/...`). Nothing in this file logs, and the
// report struct below is for a window, never for a log line. When something
// upstream needs to say what happened, it says it with `log_ref()`.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::core {

// ── What's wrong with this URL ───────────────────────────────────────────────
// Named states rather than a bool, because "invalid URL" is unactionable and
// the four common pastes fail for four different reasons the user can fix.
// (CANON: name the states of anything a reader could flatten into a boolean.)
enum class UrlProblem {
    None,
    Empty,        // nothing typed yet -- not an error, just not ready
    Whitespace,   // a space survived the paste, or two URLs got pasted at once
    BadScheme,    // has a scheme and it isn't http/https (file:, data:, javascript:)
    NoHost,       // "https:///path", "https://"
    BadHost,      // no dot, or characters a hostname can't hold
    HasUserinfo   // "https://user@host/..." -- never a listing, often a trap
};

const char* url_problem_name(UrlProblem p);
// Human-readable, for the dialog's feedback line. Never contains the URL.
const char* url_problem_text(UrlProblem p);

// ── URL handling ─────────────────────────────────────────────────────────────
// Scheme-less input is accepted and read as https: people type `spokeo.com/...`
// far more often than they type the scheme, and refusing that paste would be
// pedantry with a privacy cost (a URL retyped by hand is a URL retyped wrong).
// Downgrading is never inferred -- an explicit http:// stays http://.
UrlProblem url_check(const std::string& raw);

// The URL as we would STORE it: trimmed, scheme supplied if missing, scheme and
// host lower-cased, fragment dropped, a bare trailing slash dropped. Path and
// query survive byte-for-byte -- brokers put case-sensitive slugs and required
// ids in both, and a normaliser that "tidies" them breaks the fetch. Returns
// empty when url_check() would refuse.
std::string url_normalize(const std::string& raw);

// The host, lower-cased, without userinfo, port, or a leading "www." -- the
// display form and the matching form. Empty when there isn't one.
//
// The Cases page shows this and never the path: the path is the part carrying
// the name, and a screenshot of that window is a thing that happens.
std::string url_host(const std::string& url);

// ── Matching a listing to the roster ─────────────────────────────────────────
// By host, against both the broker's `site` and its `opt_out_url`, and across
// subdomains ("www.example.com" and "search.example.com" are Example). Exact
// host wins over a subdomain match; the longest broker host wins after that, so
// a broker registered under a sub-brand isn't shadowed by its parent company.
//
// nullptr means the roster doesn't know this host. That is a NORMAL answer, not
// an error: the roster is a maintained asset that will always trail the web, so
// the UI must let the user name the broker itself. A tool that refuses the
// paste until the roster catches up is a tool that never gets used.
const Broker* broker_for_url(const Roster& r, const std::string& url);

// ── Ids ──────────────────────────────────────────────────────────────────────
// "<broker_id>-<yyyymmdd>", then "-2", "-3" on collision. Readable, sortable,
// stable once minted, and carrying nothing about the person -- an id built from
// the URL would put PII in every log line that was careful enough to use
// log_ref().
std::string next_case_id(const Caseload& c, const std::string& broker_id,
                         const std::string& today);

// ── Do we already have this listing? ─────────────────────────────────────────
// Compares normalised URLs. Returns the FIRST match in file order; when a
// listing has been through a relist cycle there will be several, and the caller
// wants the live one, so terminal cases (Relisted, Abandoned) are skipped.
const Case* caseload_find_by_url(const Caseload& c, const std::string& url);

// ── The one call the dialog makes ────────────────────────────────────────────
// Everything knowable about a paste, recomputed on every keystroke. Pointers
// are borrowed from the roster and caseload passed in and are only valid while
// those live -- this is a view, not a value to keep.
struct IntakeReport {
    UrlProblem    problem   = UrlProblem::Empty;
    std::string   normalized;         // what we would store; empty unless clean
    std::string   host;               // display form
    const Broker* broker    = nullptr;  // roster match, or nullptr
    const Case*   existing  = nullptr;  // a case already holding this URL

    // `existing` is Removed: the record came back. This is a RELIST, and a
    // relist is a new case that supersedes the old one, never an edit of it --
    // removal happened AND the record returned, and one mutable row can only
    // hold one of those facts.
    bool relist = false;

    // Enough to commit: a clean URL and a broker. Deliberately not "no
    // duplicate" -- the relist path is a legitimate commit.
    bool ready() const {
        return problem == UrlProblem::None && broker != nullptr &&
               (existing == nullptr || relist);
    }
    // A live case already holds this URL. Nothing to add; nothing to relist.
    bool duplicate() const { return existing != nullptr && !relist; }
};

IntakeReport intake_inspect(const Roster& r, const Caseload& c,
                            const std::string& raw_url);

// Build the case a ready report describes. Status Found, outcome Never, first
// seen today.
//
// Outcome stays Never even though the user just looked at the page with their
// own eyes: Outcome means what OUR FETCH saw, and we have not fetched. Folding
// a human sighting into the machine's evidence is the exact collapse this
// schema exists to prevent -- and a fresh case reading "unchecked" in the
// status line is true, and is the app's own to-do list.
//
// next_check is today: we have never verified this listing, so it is due now.
// The caseload is passed in so the id is minted against it here: a builder
// that left the id to the caller would be a builder whose output fails
// validation whenever someone forgets, and `caseload_commit` refusing it later
// is a worse place to find out.
Case intake_new_case(const IntakeReport& rep, const Caseload& c,
                     const std::string& today,
                     const std::vector<Field>& exposes, const std::string& note);

// The relist form: the successor to `rep.existing`, wired with `supersedes`.
// Sets outcome Listed and last_verified today, unlike the fresh case above --
// here the sighting confirms a return, and the returning record is the evidence
// that the removal we recorded did not hold.
Case intake_relist_case(const IntakeReport& rep, const Caseload& c,
                        const std::string& today);

// Commit a built case into the caseload. When it supersedes another, the
// predecessor is ended at Relisted HERE, in the same call -- a successor
// existing without its predecessor closed would be a caseload that counts one
// listing twice, and every report downstream reads that count.
//
// Returns false and changes nothing if the case is malformed (no id, no broker,
// no url, duplicate id) or names a predecessor that isn't here.
bool caseload_commit(Caseload& c, const Case& fresh);

}  // namespace delr::core
