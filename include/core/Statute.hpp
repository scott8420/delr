#pragma once
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// core/Statute -- the law you are standing on when you ask.
//
// ── Why this is a table and not a constant ───────────────────────────────────
//
// Up to here every judgment this program makes is about a MACHINE: is the
// tunnel up, did the page load, does the text still say your name. Filing is
// the first act that is about a RIGHT, and a right is a fact about where the
// user lives -- not about the broker, not about the listing, and not about
// anything the app can observe by fetching something.
//
// It is the third asset class, alongside the roster and the page rules: the
// same on every machine, in git, read-only at runtime, and updated on a
// cadence no rebuild should be required for. Legislatures move faster than
// this program ships.
//
// ── FAIL CLOSED APPLIES TO LAW TOO ───────────────────────────────────────────
//
// `statute_for()` returning null is a CORRECT and common answer. Most US
// states have no consumer deletion right at all, and a request that cites a
// statute the user cannot invoke is worse than one that cites nothing: it is
// a bluff, it is checkable in thirty seconds by the person on the other end,
// and it teaches a broker that this sender does not know what they are owed.
//
// So the composer's rule, stated here because this is the file that tempts
// otherwise: NO ROW, NO CLAIM. A request from an unrepresented jurisdiction
// is a courtesy request against the broker's own posted policy, and it says
// so plainly rather than dressing up as a demand.
//
// The same rule runs one level deeper, at the field. `citation` may be empty
// on a row that is otherwise real, and an empty citation means the letter
// names the act and stops -- it does not invent a section number. Which is
// the honest state of the seed file today: see `data/statutes.json`.
//
// ── This file is CONTENT, and content is not a session ───────────────────────
//
// RULES: "distinguish code from content ... projecting those as sessions is a
// lie." The pump below is code and it is done when it compiles. The rows are
// research -- somebody reading a statute, or a lawyer -- and no number of
// sessions produces them. The seed ships with the deadlines, which are the
// part this app acts on, and with the citations mostly blank, which is the
// part a stranger acts on.
//
// GTK-free and headless-testable. The caller resolves the path (the seam).
// Encode and decode live in one file so a write cannot skew from its read.
//
// NOTE: no PII lives here. This is public law about jurisdictions, exactly
// like the roster is public knowledge about companies. The USER's jurisdiction
// -- which row applies to them -- is on the profile, which is 0600 state.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::core {

// ── Jurisdiction keys ────────────────────────────────────────────────────────
// "US-CA", "US-TN". ISO 3166-2 shaped, uppercase, and the SAME string on the
// statute row and on `Profile::residency` so the join is an equality test and
// not a matching problem. A dropdown in the UI produces these; the user never
// types one.
//
// Not a bare "CA": California and Canada are both CA in the abbreviations
// people actually use, this app will not stay inside one country forever, and
// a key that is ambiguous on the day it is introduced never gets less so.
bool jurisdiction_valid(const std::string& j);

// Uppercase, trim, and turn a bare two-letter US state into "US-XX". Empty in,
// empty out; anything it cannot make sense of comes back empty rather than
// guessed at -- an unrecognised residency must land on "no statute", which is
// a safe answer, and never on the wrong one.
std::string jurisdiction_normalize(const std::string& raw);

// The state part, for printing: "US-TN" -> "TN". Empty when it is not a key.
std::string jurisdiction_region(const std::string& j);

struct Statute {
    std::string id;            // stable slug, e.g. "us-ca-ccpa"
    std::string jurisdiction;  // "US-CA" -- joins to Profile::residency
    std::string name;          // "California Consumer Privacy Act"
    std::string short_name;    // "CCPA" -- what a letter says on second mention

    // The section that grants the deletion right. MAY BE EMPTY, and empty is
    // not a bug: see the header. A composer prints this only when it is here.
    std::string citation;

    // Where the text lives, for the user who wants to read it and for whoever
    // has to check that the row above is right. Empty until researched.
    std::string url;

    // ── The two numbers the app actually schedules on ───────────────────────
    // How long the business has to answer, and how much more it may take if it
    // tells you it is taking it. Both are days from the date the request went
    // out -- which is why the journal's `Filed` entry, and not the case's
    // mutable `requested` field, is the anchor a follow-up is measured from.
    //
    // Zero means the row does not state one. A deadline of zero must never be
    // rendered as "due immediately"; callers check for it.
    int respond_days   = 0;
    int extension_days = 0;

    // The business may require you to prove you are who you say you are before
    // acting. True nearly everywhere, and worth carrying because it is the
    // field that predicts the reply the user is about to get and does not
    // understand -- "we need to verify your identity" reads as a brush-off and
    // usually is not one.
    bool verification_allowed = true;

    // Whether the request has to ASSERT residency to trigger the duty. The
    // right belongs to residents of the jurisdiction, so a letter that never
    // says which one the sender lives in is a letter the recipient can answer
    // with a shrug. Where true, the composer states it.
    bool requires_residency_statement = true;

    std::string notes;
};

using Statutes = std::vector<Statute>;

// Lookup by jurisdiction key. NULL IS A LEGITIMATE ANSWER -- see the header.
// Takes the first row for a jurisdiction; `statutes_validate` refuses two.
const Statute* statute_for(const Statutes& s, const std::string& jurisdiction);

// Lookup by id, for a case that recorded which law it was filed under. A
// filing is anchored to the row that was live when it went out, and a later
// amendment to the table must not silently restate what the user claimed.
const Statute* statute_find(const Statutes& s, const std::string& id);

// The deadline a request filed on `filed_iso` runs to, or empty when the row
// states no `respond_days` or the date is unusable. Empty is the same
// never-a-silently-wrong-answer contract `date_add_days` keeps: a caller with
// no deadline says nothing rather than printing one it made up.
std::string statute_due(const Statute& s, const std::string& filed_iso);

// The deadline including an extension the business claimed. Same rules.
std::string statute_due_extended(const Statute& s, const std::string& filed_iso);

// Validation -- one list, N consumers, so the list itself is guarded:
//   - id present and unique
//   - jurisdiction present and well-formed, and NOT duplicated: two rows for
//     one place makes `statute_for` a coin flip, and a coin flip about which
//     law a person is invoking is worse than having no row at all
//   - name present -- a row that cannot be named cannot be cited
//   - respond_days and extension_days >= 0
std::vector<std::string> statutes_validate(const Statutes& s);

// Persistence pump -- encode + decode adjacent so they cannot drift.
// First-run tolerant: a missing file yields an empty table, not an error, and
// an empty table means every request composes as a courtesy. That degradation
// is deliberate. The app still works with no law at all; it just asks nicely.
Statutes statutes_load(const std::string& file, std::string* error = nullptr);
bool     statutes_save(const std::string& file, const Statutes& s);

}  // namespace delr::core
