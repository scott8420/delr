#pragma once
#include "core/Broker.hpp"
#include "core/Case.hpp"

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// core/PageRules -- what makes a fetched page a NotFound.
//
// Table three, and the one that ROTS. The roster is facts about companies and
// changes when a company does; these are facts about markup and change when a
// designer does. Same repo, separate file, separate version: bundling them
// would mean every A/B test on a broker's footer bumps the roster's version,
// and every new broker invalidates a rules cache that was fine.
//
// A broker with no rule is a NORMAL state, not an error -- the same shape as
// `broker_for_url` returning nullptr. It means this case can be fetched but
// not READ, which is a maintenance gap the report has to show rather than a
// failure to paper over.
//
// GTK-free, headless, and pure: bytes and a rule in, one named verdict out. No
// clock (`today` arrives as an argument), no filesystem except the pump.
//
// ── Why not regex ────────────────────────────────────────────────────────────
// These rules arrive over the network from a hosted roster and get run against
// a hostile page. A regex in that position is an execution surface -- a
// backtracking bomb in a data file, triggered by a page we don't control. Plain
// substring matching over normalised text is weaker and that is the point: the
// worst a bad marker can do is match or not match.
//
// ── PII WARNING ──────────────────────────────────────────────────────────────
// `PageNeedles` is the user's own identifiers. It is the most sensitive struct
// in the codebase: it exists to be compared against a broker's page, and it
// must never be written into a rule, a log line, a verdict string, or the
// caseload. Nothing in this file logs. `verdict_text()` is safe to show and
// safe to log; a needle is neither.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::core {

// ── Reading a page the way a person does ─────────────────────────────────────
// `no&nbsp;results` and `no <b>results</b>` are one sentence to a reader and
// three byte sequences to a matcher, so every comparison in this file happens
// against this form and never against the raw body. Script and style bodies are
// dropped whole: a marker that only appears inside a JS blob is a marker
// matching the site's code rather than its page.
//
// Lower-cased, tags and comments removed (each becoming a space, so `a<br>b`
// does not become `ab`), the common entities decoded, whitespace collapsed,
// trimmed. Markers and needles are normalised through this same function, so a
// rule author writes "No results found." and it matches.
std::string page_text(const std::string& body);

// True when `needle`, normalised, appears in `text`. Empty needle is false --
// an empty marker matching everything is how a rules file silently starts
// reporting every page as a hit.
bool page_contains(const std::string& text, const std::string& needle);

// ── A rule ───────────────────────────────────────────────────────────────────
// Three lists, each answering a different question, because one list can only
// answer "did this string appear" and that is not enough to know whether a
// removal happened.
struct PageRule {
    std::string broker_id;   // -> Broker::id

    // Is this the broker's page at all? ALL of these must appear. Chrome that
    // survives a redesign: a brand name in the nav, a legal string in the
    // footer. Keep it to one or two -- ALL-semantics means every extra entry is
    // another thing that can rot, and a rotted fingerprint refuses everything.
    //
    // This is the anti-rot mechanism and the reason the module can be trusted.
    // Without it a login wall, a parked domain, a CDN error page and a
    // captcha interstitial all read as "the absence marker wasn't there", i.e.
    // as a live listing -- or worse, an empty page reads as a removal.
    std::vector<std::string> fingerprint;

    // The record is there. ANY of these is enough (sites A/B test their
    // templates, and a rule that only knows one variant fails half the time).
    std::vector<std::string> present;

    // The page says nothing is there -- the broker's own "no results" copy.
    // ANY of these. Include the soft-404 wording here too if the broker serves
    // one, because a soft 404 is the broker telling us the record is gone in
    // the only voice it has.
    std::vector<std::string> absent;

    // Presence must ALSO be confirmed by the listing's own identifiers being on
    // the page. Set this on brokers whose listing pages are per-person: it
    // turns "this looks like a profile page" into "this is YOUR profile page",
    // and it is what makes `NeedleAbsent` available on that broker.
    bool needs_needle = false;

    // ── This broker has no page to check (s15) ───────────────────────────────
    // Some brokers publish no stable per-person listing page at all -- only a
    // lead-capture funnel whose URL carries a session id, where "your results"
    // are assembled behind a form and never live at an address a second visit
    // can reach. Instant Checkmate is the one that taught us this.
    //
    // That is not a rule that needs writing. It is a broker that cannot be
    // verified by fetching, ever, and the honest thing is to say so once
    // rather than leave a case in the maintenance queue implying a maintainer
    // could fix it. A rule with this set needs no fingerprint and no markers;
    // it exists to carry this one fact and the `notes` explaining it.
    //
    // It is also a REFUSAL TO FETCH, which is the part that matters for the
    // user: checking a funnel hands the broker a fresh visit and returns
    // nothing. See `rule_verifiable()` -- the caller is expected to consult it
    // BEFORE the fetch, not after.
    bool funnel_only = false;

    // ISO date a human last opened this broker's page and confirmed the rule
    // still describes it. Not enforced -- see `rule_stale()`. Provenance for
    // the rule itself, in a project whose whole argument is that provenance is
    // a field and not a footnote.
    std::string reviewed;

    std::string notes;
};

using PageRules = std::vector<PageRule>;

const PageRule* rules_find(const PageRules& r, const std::string& broker_id);

// May a fetch of this broker's listing page tell us anything? False only for
// `funnel_only`. A null rule is `true` -- no rule means unreadable, not
// unfetchable, and those are different refusals with different owners.
//
// The caller checks this BEFORE fetching. A funnel fetch costs the user a
// recorded visit from their exit address and returns no information, which is
// the worst trade in the program.
bool rule_verifiable(const PageRule* r);

// Days since `reviewed`, or -1 when unreviewed or either date is unusable.
int  rule_age_days(const PageRule& r, const std::string& today);
// Unreviewed counts as stale. A rule nobody has looked at is not disqualified
// from producing a verdict -- refusing on age would break every check the day a
// maintainer went on holiday -- but a NotFound produced by a two-year-old rule
// is weaker evidence than a fresh one, and the report is entitled to say so.
bool rule_stale(const PageRule& r, const std::string& today, int max_age_days = 365);

// Validation. A rules file that fails this is a bug in the DATA, which is the
// failure this project must catch early and loudly.
//   - broker_id present and unique; present in `roster` when one is supplied
//   - fingerprint / present / absent all non-empty
//   - no marker blank or shorter than kMinMarker once normalised ("no" matches
//     every page ever written)
//   - no string in both `present` and `absent` -- that guarantees Ambiguous
//   - `reviewed`, when set, a valid date
std::vector<std::string> rules_validate(const PageRules& r, const Roster* roster = nullptr);

// Shortest a marker may be once normalised. Four is arbitrary and low; its job
// is to catch "no" and "0", not to police wording.
constexpr std::size_t kMinMarker = 4;

// Persistence pump -- encode and decode adjacent so a write can't skew from its
// read. First-run tolerant: a missing file is an empty rule set, not an error.
PageRules rules_load(const std::string& file, std::string* error = nullptr);
bool      rules_save(const std::string& file, const PageRules& r);

// ── The listing's own identifiers ────────────────────────────────────────────
// PII, and the reason it lives here as a named struct rather than a bare vector
// of strings: it needs somewhere to hang this warning. Filled at check time
// from the profile (not built yet) and thrown away with the fetch. It is never
// stored in a rule, never persisted with a case, and never logged.
//
// Terms should be things a listing would print: a name, a city, a phone. Not
// a middle initial -- `page_contains` is a substring match and a one-character
// needle matches the whole internet.
struct PageNeedles {
    std::vector<std::string> terms;
    // ANY term by default. A broker whose pages are noisy enough that one name
    // hit is meaningless can be checked with ALL, at the cost of refusing every
    // listing that happens to omit one field.
    bool require_all = false;
};

// ── The verdict ──────────────────────────────────────────────────────────────
// One named state per check, like `Egress::Verdict`. Two of these mean the
// record is gone and everything else is either a sighting or a refusal, and
// which refusal it is decides whose bug it is.
enum class PageVerdict {
    // Clean fetches that produced an answer about the listing.
    Present,        // presence confirmed; the record is there
    Absent,         // the broker's page says nothing is there
    NeedleAbsent,   // the broker's page, correctly fingerprinted, with the
                    // listing's identifiers not on it -- see the note below

    // Refusals that are OUR problem. A maintainer fixes these; retrying does
    // nothing, and none of them are evidence about the listing.
    NoRule,         // this broker has no rule. Fetchable, unreadable.
    NoNeedles,      // the rule wants identity confirmation and got none
    Unfingerprinted,// not the page this rule was written for
    Ambiguous,      // present AND absent both matched. The rule is broken, or
                    // the page now says both things. Never a precedence call:
                    // picking a winner here is picking which lie to tell.
    Silent,         // fingerprint matched, neither marker did. The rule has
                    // rotted, or this is a page shape it has never seen.
    Empty,          // a 200 with no readable text. Not a page.

    // Refusals that are THEIR problem, or the network's. These bump the failure
    // streak; the ones above do not.
    NoResponse,     // the fetch produced nothing at all
    HttpDead,       // 404 / 410. Gone, or the slug was retired. Ambiguous BY
                    // NATURE and never a removal -- see Case::clean_absences.
    HttpBlocked,    // 401 / 403, and the wall did not say whose fault it is
    HttpThrottled,  // 429
    HttpServerError,// 5xx
    HttpUnexpected, // anything else, including an unfollowed redirect

    // ── The attributed walls (s15) ───────────────────────────────────────────
    // A 401/403 whose own body names what it refused. These are OURS, not
    // theirs, and so they do NOT bump the failure streak -- a broker that walls
    // our exit forever would otherwise accumulate a streak indistinguishable
    // from a listing quietly dying, and argue a live case toward Abandoned on
    // the strength of a configuration choice the user made in ten seconds.
    //
    // The bar for producing either of these is the wall's own words. See
    // `refusal_attribute()`.
    HttpBlockedEgress,  // refused the address: VPN / datacenter / geo
    HttpBlockedClient,  // refused the client shape: JS required, bot check

    // Not a failure at all. The broker has no page to check, declared by a
    // human in the rule -- see `PageRule::funnel_only`. Produced without a
    // fetch, and the only verdict here that is nobody's bug.
    FunnelOnly
};

const char* page_verdict_name(PageVerdict v);
// One actionable sentence. Contains no URL, no needle, no page content --
// safe to display and safe to log.
const char* page_verdict_text(PageVerdict v);
// True for the three verdicts that answer the question the fetch was asked.
bool page_verdict_is_clean(PageVerdict v);

// ── The check ────────────────────────────────────────────────────────────────
// Everything the fetch layer produces, judged in one pure call. `rule` may be
// null (that is `NoRule`). `status` <= 0 means the transport never got a
// response.
//
// Order of judgment, and why:
//   1. the status, because a 403's body is the bot wall's body and running
//      markers over it is reading someone else's page
//   2. the rule's existence
//   3. readable text at all
//   4. the fingerprint -- is this even the right page
//   5. needles, when the rule demands them and has none
//   6. both markers -> Ambiguous, before either one alone
//   7. absent -> Absent
//   8. present (+ needle, when required) -> Present
//   9. needle-absence, when the rule allows it
//  10. Silent
PageVerdict page_check(const PageRule* rule, int status, const std::string& body,
                       const PageNeedles& needles = {});

// ── Reading the wall's own words ─────────────────────────────────────────────
// Given the body of a 401/403, decide whether the wall said WHAT it refused.
// Returns `HttpBlockedEgress`, `HttpBlockedClient`, or plain `HttpBlocked` when
// it said nothing usable -- which is the common case and the correct default.
//
// This is the one place the app reads a page it was not looking for. `page_check`
// refuses to run listing markers over a 403 body, and is right to: that is
// someone else's page and a "no results" string on a bot wall would be read as
// a removal. But the wall is a perfectly good witness ABOUT ITSELF, and
// throwing its explanation away is what made three different problems look like
// one for a whole session.
//
// Substring matching over normalised text, same as everything else in this
// file, and for the same reason: this input arrives from the network, and a
// backtracking regex over hostile bytes is an attack surface, not a feature.
//
// The phrase sets are deliberately small and generic. They are the wall's
// vocabulary, not any one vendor's -- a list of product names would rot on
// every rebrand, and a wall that has to explain itself to a human tends to use
// the same dozen words whoever built it. When in doubt the set stays short:
// a missed attribution costs one `Blocked`, and a wrong one sends the user to
// change exits over a header problem.
PageVerdict refusal_attribute(const std::string& body);

// ── The seam to the caseload ─────────────────────────────────────────────────
// What this verdict means in the vocabulary a case stores. `Absent` and
// `NeedleAbsent` are the only two that produce NotFound; every refusal produces
// Indeterminate, and Indeterminate never rounds.
struct PageOutcome { Outcome outcome; Reason reason; };
PageOutcome page_outcome(PageVerdict v);

// Apply a page verdict to a case: records what was seen, moves the dates,
// schedules the next check. Does NOT touch status -- promotion is still a
// separate judgment made with the streak in view (`promotion_for`).
//
// The failure streak moves only for the THEIR-problem verdicts. A rotted rule
// or a missing one is a claim about us; left to accumulate it would read,
// months later, as a broker that keeps refusing us, and could argue a case
// toward Abandoned on the strength of a redesign nobody re-read.
//
// Nor does a maintenance refusal retry tomorrow the way an egress refusal does.
// A tunnel may be up in the morning; a rule maintainer will not be. Rescheduled
// at the normal cadence, and surfaced immediately instead --
// `caseload_unverifiable()` is the report's handle on it.
Case apply_page_verdict(const Case& c, PageVerdict v, const std::string& today,
                        int recheck_days);

// Cases whose last check failed because WE could not read the page. The
// maintenance queue, and the honest denominator for any "N listings verified"
// claim the report makes.
std::vector<const Case*> caseload_unverifiable(const Caseload& c);

// ── Cases behind a wall, and cases with no wall to get behind ────────────────
// Split out of the maintenance queue on purpose. `caseload_unverifiable()` is
// the list a MAINTAINER works: a rule is missing or has rotted, and writing one
// fixes it. Neither of these is that list.
//
// Walled: the last check was refused by the far end. Nothing here is evidence
// about the listing, and the report must not count these as "checked". Sorted
// into who can act by reading `Case::reason`: `EgressBlocked` is the user's to
// fix, `ClientBlocked` is ours, `Blocked` is nobody's until we learn more.
std::vector<const Case*> caseload_walled(const Caseload& c);

// Unverifiable BY CONSTRUCTION: the broker publishes no page a fetch can read.
// These never leave this list by being retried, and the report owes the user a
// different sentence for them -- not "we could not check yet" but "this one
// cannot be checked, and filing is the only lever you have here". The honest
// numerator problem in reverse: every "N of M verified" claim has to exclude
// these from M or it is quietly blaming itself for a broker's design.
std::vector<const Case*> caseload_unverifiable_by_design(const Caseload& c);

}  // namespace delr::core
