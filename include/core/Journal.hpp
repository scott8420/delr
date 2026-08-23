#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/Case.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// core/Journal -- table five: what this program DID, and when.
//
// ── Why this exists ──────────────────────────────────────────────────────────
//
// `core/Case` has carried this line since s8:
//
//     // Last check, and only the last -- the run history is a separate
//     // concern and deliberately not modelled yet.
//
// And its file header carries this one:
//
//     // The history is the evidence, and this app exists because nobody
//     // else keeps it.
//
// Both are true, and together they are an accusation. delr's whole pitch is
// that a removal service reports its own success and never goes back to look;
// that the gap between "the broker says gone" and "we fetched the page and it
// was gone" is the product. But a case holds ONE outcome and ONE reason, and
// every check overwrites the last. Ask delr today how many times it has looked
// at a listing, or how long a broker has been refusing this machine, and it
// cannot answer. It kept a snapshot and called it a record.
//
// This file is the record. Append-only, one line per act, and nothing in it is
// ever rewritten.
//
// ── What the case still owns, and why the split is not redundancy ────────────
//
// The caseload is CURRENT STATE: mutable rows, a snapshot of what is believed
// right now, rewritten whole on every save. The journal is HISTORY: immutable
// rows, monotone growth, appended one at a time. They answer different
// questions and the shapes follow from that -- `Case::outcome` answers "where
// does this stand", the journal answers "how did it get here".
//
// The journal is deliberately NOT the source of truth for the case. Rebuilding
// a caseload by replaying entries would make every read depend on every write
// that ever happened, and a single lost line would silently change what the
// app believes. The case stays authoritative about the present; the journal is
// authoritative about the past. When they disagree, that disagreement IS the
// signal -- same rule as traces and eyes.
//
// ── Why NDJSON and not a JSON array ──────────────────────────────────────────
//
// One JSON object per line, opened in append mode. Not the whole-file
// `dump(2)` pump that `caseload_save` and `egress_save` use, and the
// difference is not stylistic:
//
//   - Rewriting an array to add one row puts the ENTIRE history at risk on
//     every write. A truncate that fails halfway costs everything. For the one
//     file in this program that is evidence, losing the file to record an
//     event is the worst available failure mode. An append risks only the line
//     being written.
//   - A corrupt line can be skipped. A corrupt array cannot -- one bad byte
//     and the parser hands back nothing, and `caseload_load`'s honest
//     first-run tolerance ("absent is empty") becomes a silent erasure.
//   - It is greppable and diffable by a human who does not have the app, which
//     matters for a file whose eventual job is to be shown to somebody else.
//
// ── No free text. Deliberately. ──────────────────────────────────────────────
//
// Every field below is an enum, an id, a date or a sequence number. There is
// no note, no message, no detail string, and there is not going to be one.
//
// This is the file most likely to leave the machine -- pasted into a complaint
// to an attorney general, attached to a statutory follow-up, handed to
// somebody to prove a broker sat on a request past its deadline. A free-text
// column is where a human types an address, and `Case::note` is already marked
// "PII-adjacent by assumption. Never logged." Giving the journal one would put
// the same hazard in the file least likely to stay home.
//
// No `url` either, for the reason `Case`'s header gives: a listing URL carries
// a name and a state in its path. The journal carries `case_id` and
// `broker_id`, which are exactly what `log_ref()` already decided was safe to
// write down.
//
// ── Forward tolerance, and why `kind_raw` is here ────────────────────────────
//
// An append-only evidence file outlives the binary that wrote it. A newer delr
// will write kinds this one has never heard of -- `filed` is the obvious next
// one -- and an older binary reading that file MUST NOT quietly drop the row.
// Unknown kinds decode to `Kind::Other` with the label preserved in
// `kind_raw`, so a round trip through a binary that does not understand an
// entry gives the entry back unchanged instead of deleting somebody's proof
// that they filed.
//
// GTK-free and headless-testable. The caller resolves the path (the seam).
// Encode and decode live in one file so a write cannot skew from its read.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::core {

// ── What kind of act ─────────────────────────────────────────────────────────
// Small and closed, and every value has a producer in the app TODAY. A kind
// with no writer is a promise the file cannot keep, and `Other` already covers
// the forward case without anyone having to guess at it in advance.
enum class Kind {
    Opened,   // a case entered the caseload
    Checked,  // a fetch ran and was judged -- carries outcome and reason
    Declined, // NO fetch was attempted, and why -- carries reason
    Changed,  // the case's standing moved -- carries from, to, provenance
    Other     // written by a newer delr; label preserved in `kind_raw`
};

const char* kind_name(Kind k);
Kind        kind_from(const std::string& s);

// ── One act ──────────────────────────────────────────────────────────────────
// Flat rather than a variant, matching `Case`: the payload fields that do not
// apply to a kind sit at their defaults and the pump stays trivial. The cost
// is a few unused members per row; the benefit is that decode never has to
// branch on kind to know what shape it is reading, which is exactly the skew
// that "encode and decode adjacent" exists to prevent.
struct Entry {
    // Monotone within one journal, assigned by `journal_append`. Order as a
    // RECORDED FACT rather than a property of the file layout: line order
    // already carries it, but a file that gets sorted, merged or partially
    // recovered loses that and keeps this. It is also the only stable handle
    // an entry has -- nothing else about a row is unique.
    std::int64_t seq = 0;

    // ISO "YYYY-MM-DD". Days precision, matching `Case` and for its reason:
    // this app schedules on a 45-day rhythm and a timestamp would imply a
    // precision the evidence does not have. Two acts on one day are told apart
    // by `seq`, which is what `seq` is for.
    std::string date;

    std::string case_id;    // safe ref. Never a url.
    std::string broker_id;  // -> Broker::id. Denormalised on purpose: an entry
                            // has to stay readable after its case is gone.

    Kind        kind = Kind::Other;
    std::string kind_raw;   // set only when `kind == Other`. See the header.

    // Checked: what the fetch saw.
    Outcome outcome = Outcome::Never;

    // Checked (when Indeterminate) and Declined: why. Same invariant as
    // `Case` -- an indeterminate without a reason is unactionable.
    Reason reason = Reason::None;

    // Changed: the transition, and who says so.
    Status     from       = Status::Unknown;
    Status     to         = Status::Unknown;
    Provenance provenance = Provenance::None;

    // The successor's id on a relist. The one place an entry points at another
    // case, and it is here rather than in a second entry because a relist is
    // ONE event about two rows -- the same reason `caseload_record_return`
    // does both halves in one call.
    std::string other_id;
};

using Journal = std::vector<Entry>;

// ── Making entries ───────────────────────────────────────────────────────────
// Pure builders, so the selftest can assert on what an act records without a
// filesystem, and so the caller in Shell_work does not hand-populate a struct
// where a wrong `outcome` would become permanent.
//
// They take the case AFTER the apply_* call, because the case is where the
// outcome landed and re-deriving it here would be a second opinion that can
// disagree with the first.

Entry entry_opened(const Case& k, const std::string& today);

// The check that ran. `outcome`/`reason` are read off the case.
Entry entry_checked(const Case& k, const std::string& today);

// The check that did NOT run: an egress refusal, a funnel-only rule, anything
// where nothing left this machine. A separate kind rather than a Checked with
// a sad reason, because the difference is whether the broker was contacted at
// all -- which is the question an honest "N checks against M brokers" has to
// answer, and the one a user asking "did this thing phone them" is asking.
Entry entry_declined(const Case& k, Reason r, const std::string& today);

// A standing moved. `other_id` is the successor on a relist and empty
// otherwise.
Entry entry_changed(const Case& k, Status from, Status to,
                    const std::string& today,
                    const std::string& other_id = "");

// Safe log identifier -- "entry:<seq>/<kind>@<case_id>". No dates, no
// transition, no broker. Anything that logs an entry logs THIS.
std::string log_ref(const Entry& e);

// ── Reading the history ──────────────────────────────────────────────────────
// The queries no field on `Case` can answer, which is the entire justification
// for the table.

// This case's entries, in recorded order (ascending `seq`).
std::vector<const Entry*> journal_for_case(const Journal& j, const std::string& case_id);

// The most recent entry of a kind for a case, or null. `Kind::Other` matches
// nothing here on purpose: "the last entry of a kind I do not understand" is
// not a question with an answer.
const Entry* journal_last(const Journal& j, const std::string& case_id, Kind k);

// The FIRST entry of a kind for a case, or null. The filing date lives behind
// this once filing exists -- a statutory deadline runs from when the request
// went out, not from the last time anyone touched the case.
const Entry* journal_first(const Journal& j, const std::string& case_id, Kind k);

// Everything on or after `iso`, in order. Empty or invalid date yields empty
// rather than everything: a malformed filter that silently means "no filter"
// is how a report ends up claiming a decade of activity happened last week.
std::vector<const Entry*> journal_since(const Journal& j, const std::string& iso);

// ── The payoff query ─────────────────────────────────────────────────────────
// The date the CURRENT unbroken run of walls began, or empty if the last thing
// that happened to this case was not a wall.
//
// This is the sentence no snapshot can produce. `Case` knows it is blocked and
// knows how many failures have stacked up, but "this broker has been refusing
// your exit since March" needs a date that only a history has -- and that
// sentence is the difference between a user shrugging at a red row and a user
// changing exits, or knowing the wall is `ClientBlocked` and therefore ours to
// fix and not worth them touching at all.
//
// A run is broken by any Checked that was not a wall. `Declined` does NOT
// break it and does not extend it either: our own tunnel being down is not
// evidence about the broker, the same reason `apply_egress_refusal` leaves the
// failure streak alone.
std::string journal_walled_since(const Journal& j, const std::string& case_id);

// True when the reason is one of the four walls -- Blocked and the three
// attributed ones. Shared by the query above and the checks, so "what counts
// as a wall" has one definition and not two that drift.
bool reason_is_wall(Reason r);

// ── The honest denominator ───────────────────────────────────────────────────
// Every "N of M verified" claim this app makes has to be able to show its
// working, and the working is here rather than in a case field because a case
// that has been checked forty times and read cleanly twice looks, in the
// snapshot, exactly like one checked twice.
struct Tally {
    int checked   = 0;  // fetches that ran and were judged
    int declined  = 0;  // acts where nothing left this machine
    int clean     = 0;  // Listed or NotFound -- we could actually see
    int walled    = 0;  // refused by the far end
    int ours      = 0;  // NoRule, PageUnreadable, NoTunnel -- our bugs
};

// Over one case when `case_id` is given, over the whole journal when empty.
Tally journal_tally(const Journal& j, const std::string& case_id = "");

// ── Validation ───────────────────────────────────────────────────────────────
// Read-time only. `journal_append` NEVER refuses an entry on its content --
// see the pump below -- so this is where a malformed history gets noticed,
// after it is safely on disk rather than instead of being on disk.
//
// Guards:
//   - seq present, positive, and strictly increasing
//   - dates well-formed, and non-decreasing down the file
//   - case_id present on every kind (all four are about a case today)
//   - Checked carries an outcome other than Never
//   - Indeterminate carries a reason; every other outcome carries none
//   - Declined carries a reason
//   - Changed carries a real transition (from != to)
//   - Other carries its raw label, or the entry is unrecoverable
std::vector<std::string> journal_validate(const Journal& j);

// ── Pump ─────────────────────────────────────────────────────────────────────

// First-run tolerant: a missing file is an empty journal, not an error.
//
// LINE-TOLERANT, which is the property the format was chosen for. A line that
// will not parse is skipped and counted into `*skipped`; every line that WILL
// parse is returned. A process killed mid-append leaves a partial last line,
// and losing that one entry is correct -- losing the other nine hundred to it
// would not be. `*error` is set only when the file exists and cannot be READ,
// which is a different problem with a different fix.
Journal journal_load(const std::string& file, std::string* error = nullptr,
                     int* skipped = nullptr);

// One line, appended. Assigns `e.seq` from `next_seq` and returns it, so the
// caller writes the entry it thinks it wrote.
//
// It does NOT validate. A journal that drops the entries it disapproves of is
// not a journal, and the moment an event happens is the worst possible moment
// to decide it was not worth recording. The only refusal here is I/O: the file
// would not open, or the write did not land.
//
// Heals a torn tail: if the file does not end in a newline, a newline is
// written first, so a half-written line stays one skippable bad line instead
// of swallowing the entry that follows it.
//
// Mode 0600 before the first byte, same as the egress policy and the profile.
// Re-applied on every append rather than only at creation, for the reason
// `ensure_state_dir` re-applies 0700: a file that got made with the wrong mode
// once stays wrong forever otherwise.
bool journal_append(const std::string& file, Entry& e, std::int64_t next_seq);

// The next sequence number for a journal: one past the highest seen, or 1 for
// an empty one. Reads the loaded journal rather than the file, so a caller
// that already has it does not pay for a second read.
std::int64_t journal_next_seq(const Journal& j);

// Convenience for the common shape: load, append one, done. Returns false on
// any I/O failure, with the same never-refuses-on-content rule.
bool journal_record(const std::string& file, Entry& e);

}  // namespace delr::core
