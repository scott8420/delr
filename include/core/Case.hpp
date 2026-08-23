#pragma once
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// core/Case -- table two: what you've asked, when, and where it stands.
//
// A case is ONE LISTING, not one broker. Spokeo can hold two records for you
// under two addresses; those are two cases, checked independently, removed
// independently. The broker id rides along as a field.
//
// A relist opens a NEW case carrying `supersedes` = the old case's id, and the
// old one ends at Relisted. It is not the old case reopened. Removal happened;
// the record came back; both facts are true and a single mutable row can only
// hold one of them. The history is the evidence, and this app exists because
// nobody else keeps it.
//
// GTK-free and headless-testable. The caller resolves the path (the seam).
// Encode and decode live in one file so a write can't skew from its read.
//
// ── PII WARNING, load-bearing ────────────────────────────────────────────────
// Unlike the roster, THIS TABLE IS PII. `url` in particular is PII all by
// itself -- `spokeo.com/John-Smith/Tennessee/...` carries a name and a state in
// the path -- so the no-PII-in-logs rule extends to URLs. Log `id`, log the
// outcome, never log `url`, `note`, or the exposure list. `log_ref()` exists to
// make the safe thing the easy thing.
//
// This file is also the thing encryption-at-rest has to protect. The pump takes
// a plain path today; when the crypto stone lands it swaps underneath, which is
// why nothing above core:: is allowed to know the on-disk shape.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::core {

// ── What we believe about this listing ───────────────────────────────────────
// The lifecycle. Distinct from Outcome below: Status is the case's standing,
// Outcome is what the last fetch literally saw. They are different axes and
// collapsing them is how "we couldn't look" becomes "you're clean".
enum class Status {
    Found,      // you found it; nothing asked yet
    Requested,  // opt-out sent, awaiting effect
    Removed,    // believed gone -- see Provenance for who says so
    Relisted,   // was removed, came back; terminal (a new case supersedes)
    Abandoned,  // deliberately stopped chasing
    Unknown
};

// ── Who says it's gone ───────────────────────────────────────────────────────
// The whole point of the app. A broker's dashboard tick and our own fetch of
// the live page are not the same claim, and a report that renders them
// identically is lying by omission.
enum class Provenance {
    None,          // nobody has claimed anything
    BrokerClaim,   // the broker says so
    PlatformClaim, // a state platform (DROP) relays the broker's declaration
    SelfVerified   // WE fetched the page and it was gone. The only evidence.
};

// ── What the last check actually saw ─────────────────────────────────────────
enum class Outcome {
    Listed,        // the record is there
    NotFound,      // fetched cleanly, record absent
    Indeterminate, // we could not look. NEVER rounds to NotFound.
    Never          // no check has run yet
};

// Why a check was indeterminate. A bare "failed" is unactionable; these
// distinguish "the site blocked us" from "our own tunnel was down", which are
// different bugs with different fixes.
enum class Reason {
    None,
    NoTunnel,      // egress preflight refused -- we declined to expose ourselves
    Blocked,       // a wall, and it did not say whose. See the note below.
    Captcha,
    RateLimited,
    Timeout,
    BadResponse,   // 5xx, truncated, unparseable
    UrlDead,       // 404 on the URL itself: gone, or moved. Ambiguous by nature.

    // The two below are OUR bugs, not theirs, and that is the whole reason
    // they are separate values: a maintainer fixes these, a retry does not.
    // See core/PageRules -- neither one bumps the failure streak, because a
    // rule we cannot read with is not evidence about the listing.
    NoRule,        // no page rule for this broker: fetchable, unreadable
    PageUnreadable,// a rule exists and did not fit the page it was given

    // ── The three walls, named apart (s15) ───────────────────────────────────
    // The first live checks against real brokers hit three refusals that a
    // single `Blocked` rendered identically, and they are not the same event.
    // They differ in WHO CAN FIX THEM, which is the only axis a refusal value
    // is for -- the same reason `NoRule` and `PageUnreadable` were split.
    //
    //   EgressBlocked -- the wall refused the ADDRESS. The user fixes it: a
    //                    different exit, a different city. Actionable, today.
    //   ClientBlocked -- the wall refused the CLIENT SHAPE. A browser through
    //                    the same tunnel gets through. THE APP fixes it; the
    //                    user cannot, and telling them to change exits wastes
    //                    their afternoon.
    //   NoListingPage -- the broker publishes no stable per-person page at all,
    //                    only a lead-capture funnel. NOBODY fixes it. There is
    //                    no rule that could work and no exit that would help,
    //                    and a case sitting in the maintenance queue waiting
    //                    for one is a promise the app cannot keep.
    //
    // `Blocked` SURVIVES and is the default, deliberately. A bare 403 that does
    // not say why is exactly that: a wall, unattributed. Attribution requires
    // evidence -- the wall's own words -- and guessing between these three from
    // a status code would be the app inventing a diagnosis. Fail closed here
    // too: an unattributed wall is honest, a wrong attribution sends the user
    // to fix something that was never broken.
    EgressBlocked,
    ClientBlocked,
    NoListingPage
};

// ── What this listing exposes ────────────────────────────────────────────────
// Observed on YOUR listing, not inherited from the roster's description of what
// a broker typically holds. The report aggregates on this axis, because "your
// phone is on nine sites" is the sentence that tells you where the exposure is.
enum class Field {
    Name, Aliases, Age, Dob, Address, AddressHistory,
    Phone, Email, Relatives, Employer, Other
};

const char* status_name(Status s);      Status     status_from(const std::string& s);
const char* provenance_name(Provenance p); Provenance provenance_from(const std::string& s);
const char* outcome_name(Outcome o);    Outcome    outcome_from(const std::string& s);
const char* reason_name(Reason r);      Reason     reason_from(const std::string& s);
const char* field_name(Field f);        Field      field_from(const std::string& s);

// ── Dates ────────────────────────────────────────────────────────────────────
// ISO "YYYY-MM-DD", stored as text so the file stays readable and diffable, and
// so no time library leaks into the core. Days-precision on purpose: this app
// schedules on a 45-day rhythm, and a timestamp would imply a precision the
// evidence does not have.
bool date_valid(const std::string& iso);
// Calendar-correct day arithmetic (leap years included). Empty/invalid in,
// empty out -- never a silently wrong date.
std::string date_add_days(const std::string& iso, int days);
// <0, 0, >0. Invalid dates sort before valid ones rather than throwing.
int date_compare(const std::string& a, const std::string& b);

// Days from `a` to `b`, calendar-correct and signed. Either invalid yields 0,
// which is the same never-a-silently-wrong-answer contract `date_add_days`
// keeps -- a caller that wants to tell "same day" from "could not tell" checks
// `date_valid` first, and every caller that does not is asking a question a
// zero answers safely.
//
// Exists because the run history asks a question the schedule never did: the
// caseload only ever needed "is this due yet", which `date_compare` answers,
// while "how long has this broker been refusing you" needs a duration.
int date_days_between(const std::string& a, const std::string& b);

struct Case {
    std::string id;          // stable slug, unique within the caseload
    std::string broker_id;   // -> Broker::id in the roster
    std::string url;         // PII. The listing itself. Never logged.

    Status     status     = Status::Found;
    Provenance provenance = Provenance::None;

    // Last check, and only the last -- the run history is a separate concern
    // and deliberately not modelled yet.
    Outcome outcome = Outcome::Never;
    Reason  reason  = Reason::None;

    // first_seen   -- when YOU found the listing
    // requested    -- when the opt-out went out
    // last_attempt -- when we last TRIED, however it went
    // last_verified-- when we last fetched cleanly. The one that means anything.
    // next_check   -- when it comes due
    std::string first_seen, requested, last_attempt, last_verified, next_check;

    int consecutive_failures = 0;  // resets on any clean fetch, either way

    // Consecutive CLEAN fetches that found nothing. Separate from the failure
    // count above and pointed the other way: this is the evidence that a record
    // is gone, and "believed gone" is a claim that needs more than one look.
    //
    // A 404 does not land here. A dead URL is Indeterminate/UrlDead, never
    // NotFound, because brokers retire a slug and re-serve the same record
    // under a new one -- and the difference between "the page says you are not
    // here" and "the page is missing" is the difference between removal and a
    // rename.
    int clean_absences = 0;

    std::vector<Field> exposes;    // observed on this listing
    std::string supersedes;        // the case this one replaces, after a relist
    std::string note;              // PII-adjacent by assumption. Never logged.
};

using Caseload = std::vector<Case>;

const Case* caseload_find(const Caseload& c, const std::string& id);

// Cases due on or before `today`, oldest-due first. Abandoned and Relisted are
// skipped: both are terminal, and re-checking them forever would be the app
// generating traffic about records it has stopped tracking.
std::vector<const Case*> caseload_due(const Caseload& c, const std::string& today);

// Exposure by field across live cases -- the report's spine. Counts each field
// once per case; Removed cases are excluded (that exposure is closed) unless
// `include_removed`. Sorted by count, descending, then by field for stability.
struct FieldCount { Field field; int count; };
std::vector<FieldCount> exposure_by_field(const Caseload& c, bool include_removed = false);

// Apply a check result: sets outcome/reason, moves the dates, bumps or clears
// the failure count, and schedules the next check `recheck_days` out. Does NOT
// touch `status` -- believing a record is gone is a judgment, and one clean
// NotFound is evidence toward it rather than the thing itself. Returns the
// updated case; pure, so the selftest can assert on it without a filesystem.
Case apply_check(const Case& c, Outcome o, Reason r,
                 const std::string& today, int recheck_days);

// ── Filing, which is neither a check nor a belief ────────────────────────────
// Records that a request went out: status moves to Requested and `requested`
// gets a date. Outcome and reason are left ALONE -- a filing is not a look at
// the page, and borrowing the check fields to describe one would make the
// caseload claim we had seen something we had not. Same axis discipline as
// `apply_check` refusing to touch `status`.
//
// `requested` is set only when it is EMPTY. A second request about the same
// listing is a follow-up, and a statutory clock runs from the first one; an
// app that moved the date every time the user re-sent would hand a broker a
// fresh deadline for ignoring the last one. (The authoritative anchor is
// `journal_filed_on` -- this field is the snapshot's copy of it.)
//
// Terminal cases come back untouched. Filing against a case already closed as
// Removed, Relisted or Abandoned is not a transition this can express, and
// silently reopening one would lose the reason it closed.
Case apply_filed(const Case& c, const std::string& today);

// ── Believing a record is gone ───────────────────────────────────────────────
// `apply_check` records what a fetch SAW and deliberately stops there. This is
// the separate act of deciding what to BELIEVE, kept apart because recording
// and concluding are different jobs and only one of them is reversible by
// looking again.
struct PromotionRule {
    // Clean absences before we will call a listing removed. Two, because one is
    // an event and two is a pattern: brokers serve empty pages under load, and
    // a single clean miss followed by 45 days of silence is exactly the story a
    // removal service would sell you.
    int  clean_absences_required = 2;

    // A broker's or a state platform's claim lowers the bar by one. It is not
    // evidence on its own -- the whole app exists because it is not -- but our
    // own clean fetch AGREEING with a claim is two independent sources, and
    // that is a different thing from one source twice.
    bool claim_counts_as_one = true;
};

// What this case's own record now says should happen to it.
enum class Promotion {
    None,      // nothing has changed our mind
    Removed,   // enough clean absences: believe it gone
    Returned   // a Removed case is Listed again. The event this app exists for.
};

const char* promotion_name(Promotion p);

// Pure, and reads only what is already on the case. Terminal cases yield None.
Promotion promotion_for(const Case& c, const PromotionRule& r = {});

// Apply a Promotion::Removed: status Removed, provenance SelfVerified. Anything
// else comes back untouched -- Returned deliberately included, because a return
// is two rows (the old case ends, a successor opens) and no single-case edit
// can express it honestly. `caseload_record_return()` in core/Intake does that
// one, where the caseload is in scope.
Case apply_promotion(const Case& c, const PromotionRule& r = {});

// Open the successor to a relisted case: same broker and URL, new id, status
// Found, provenance cleared, `supersedes` wired. The caller ends the old one.
Case relist_successor(const Case& old, const std::string& new_id,
                      const std::string& today);

// Safe log identifier -- "case:<id>@<broker_id>". Carries no URL, no note, no
// field list. Anything that logs a case logs THIS.
std::string log_ref(const Case& c);

// Validation. Guards the invariants the report depends on, loudly:
//   - ids present and unique; broker_id and url present
//   - every date well-formed, and ordered (first_seen <= requested <= ...)
//   - Indeterminate carries a Reason; every other outcome carries none
//   - Removed carries a Provenance, and SelfVerified requires a last_verified
//   - Relisted carries a successor's supersedes elsewhere -- not checkable here
//   - consecutive_failures and clean_absences >= 0
std::vector<std::string> caseload_validate(const Caseload& c);

// Persistence pump. First-run tolerant: a missing file is an empty caseload,
// not an error. Malformed JSON yields empty and reports via `error`.
Caseload caseload_load(const std::string& file, std::string* error = nullptr);
bool     caseload_save(const std::string& file, const Caseload& c);

}  // namespace delr::core
