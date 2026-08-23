#pragma once
#include "core/PageRules.hpp"

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// core/Profile -- table four: who you are, in the words a broker would print.
//
// The first of Scott's six lines, and the one that never existed. Everything
// built before this session assumed it: `PageNeedles` says "filled at check
// time from the profile (not built yet)", and `Shell_work` has passed an EMPTY
// one since s8 -- which means every rule with `needs_needle` has returned
// `NoNeedles` for six sessions. This file is what that hole was shaped like.
//
// ── WHAT A PROFILE IS, AND THE THING THAT DECIDES ITS SHAPE ──────────────────
//
// It is NOT an identity document. It is a SEARCH KEY: the set of strings a
// listing about you would literally have on it. That single choice settles most
// of what follows --
//
//   * A search key has no tense. A maiden name and a 2009 address are exactly
//     as good at finding you as your current ones -- better, often, because a
//     broker's record is a decade of accretion and the old rows are the ones
//     that make you findable. So there is no current/former flag on a term.
//     When FILING needs "your address, today" (s15) that is a different field
//     with a different job, and it can be added then rather than guessed at now.
//
//   * Every field is plural. You are in there under Robert and Bob, under two
//     spellings and three cities, and a model with one `name` string is a model
//     that finds a third of you. The only singular fields below are the ones
//     that are singular in fact.
//
//   * Not everything true about you belongs here. The test for a field is
//     whether a broker's page would PRINT it. That is why there is no SSN, no
//     date of birth to the day, no employer -- see `birth_year` below.
//
// ── THE ASYMMETRY, SAID OUT LOUD BECAUSE NOBODY ELSE SAYS IT ─────────────────
//
// Every term added makes you easier to find AND easier to identify. That is one
// trade, not two, and a removal service that asks for your full history without
// mentioning it is being paid not to. delr's version of the deal:
//
//   * A term is a SEARCH KEY, never a submission. Nothing in this struct is
//     sent to anybody by the act of typing it. Filing is a separate, explicit
//     act against a named broker, and when it lands it will say what it is
//     about to disclose before it discloses it.
//   * The file is the single most sensitive thing this program writes. 0600,
//     under XDG state, never in the source tree, never in a log. See the pump.
//   * Least that does the job: `birth_year`, not a date of birth. A year
//     disambiguates John Smith from the other John Smith -- which is the whole
//     job -- and a day and month buy nothing this app can use while being the
//     two fields every identity thief actually wants.
//
// ── ONE PROFILE, ONE PERSON ──────────────────────────────────────────────────
//
// Deliberately. Clearing a spouse's or a parent's listings is a real want and a
// different program: it needs consent, a separate caseload, and an answer to
// "who authorised this", and bolting a second name onto this struct would fake
// all three.
//
// GTK-free and headless-testable. The caller resolves the path (the seam), and
// encode/decode live in one file so a write cannot skew from its read.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::core {

struct Profile {
    // The name you would expect to see printed. Also the first search term.
    std::string full_name;

    // Maiden names, nicknames, spellings, the version with the middle initial.
    // A people-search record is filed under all of them.
    std::vector<std::string> also_known_as;

    // Every address you have used, not just the one you read mail at. Brokers
    // key on the old ones and so does finding yourself.
    std::vector<std::string> emails;

    // Stored however typed; the needle derivation below emits the printed
    // FORMS, because a page saying "(615) 555-0100" does not contain the
    // string "6155550100" and a substring matcher does not know that.
    std::vector<std::string> phones;

    // Handles. Good needles when they are distinctive, refused when they are
    // not -- see `needle_usable`.
    std::vector<std::string> usernames;

    // Where you have lived, in the form a listing prints: "Nashville, TN", a
    // zip, a street line. Cities and states are what people-search pages
    // actually show, and they are what disambiguates a common name.
    std::vector<std::string> places;

    // 0 means not given. Year only, on purpose -- see the header.
    int birth_year = 0;

    // The inbox an opt-out is filed from and replied to. Singular because it
    // is singular in fact: a request needs one address that a human reads.
    // Must be one of `emails` -- validated, so the app cannot file from an
    // address the user never told it about.
    std::string contact_email;

    // Free text for the user. PII by assumption, like `Case::note`, and never
    // logged for the same reason.
    std::string note;
};

// True when nothing has been entered. NOT an error: a first run has no profile
// and the window's job is to say so, not to refuse.
bool profile_is_empty(const Profile& p);

// ── Terms ────────────────────────────────────────────────────────────────────

// Trim, collapse inner whitespace, drop empties, drop case-insensitive
// duplicates, preserve order. The parse behind every multi-line box in the UI:
// one term per line, and the surface decides nothing.
std::vector<std::string> terms_parse(const std::string& text);

// The inverse, for painting a box from a loaded profile. Adjacent to the parse
// so a read cannot skew from its write.
std::string terms_join(const std::vector<std::string>& terms);

// Digits only. Empty when there are not enough of them to be a phone number.
std::string phone_digits(const std::string& raw);

// The printed forms of a 10-digit US number: bare, dotted, dashed, spaced,
// parenthesised, and +1-prefixed. A closed set, which is why this is worth
// doing here rather than teaching the matcher about phone numbers: the formats
// brokers print are few and known, and enumerating them costs six strings
// while a digit-normalising matcher would be a second normalisation rule
// living in the file least equipped to own one.
//
// Anything that is not ten digits comes back as itself, once: an international
// number is not something this function should be guessing about.
std::vector<std::string> phone_variants(const std::string& raw);

// ── Needles ──────────────────────────────────────────────────────────────────

// Whether a term may be used to confirm a listing is yours.
//
// The guard is not about length for its own sake. `PageNeedles` warns that a
// one-character needle matches the whole internet, and the failure it protects
// against is a term that hits every page rather than a term that is short: a
// surname can be two letters and is a perfectly good needle.
//
//   * at least two characters once normalised
//   * a purely numeric term needs five digits -- a birth year is four and would
//     match a copyright notice, a house number is three and matches a price
bool needle_usable(const std::string& term);

// The terms this profile can confirm a listing with. Deliberately a SUBSET,
// and the exclusions are the content of this function:
//
//   INCLUDED  names, also-known-as, usernames, places, phones (as variants)
//
//   EXCLUDED  emails. Brokers mask them -- "j****@gmail.com" -- so the string
//             is not on the page even when the address is, and a needle that
//             cannot match makes a listing look like it is not yours. Wrong in
//             the dangerous direction: `NeedleAbsent` produces `NotFound`.
//
//   EXCLUDED  birth_year. Four digits; matches a copyright line on every page
//             on the web. It is here to disambiguate for a HUMAN reading the
//             report, not for the matcher.
//
// Deduplicated, order preserved, unusable terms dropped. ANY-match, because
// a listing that prints your name and not your city is still your listing.
PageNeedles needles_for(const Profile& p);

// How many usable needles the profile yields. The number the window shows,
// because "you have entered 9 things and 6 of them can confirm a page" is the
// only honest way to say what a profile is worth.
std::size_t needle_count(const Profile& p);

// ── Validation ───────────────────────────────────────────────────────────────
// Sentences, not codes -- the window prints these verbatim.
//   - an email has an @ with something either side and a dot after it
//   - `contact_email`, when set, is one of `emails`
//   - `birth_year` is 0 or plausible (1900..2100)
//   - a phone has enough digits to be one
//   - no duplicate terms within a field
std::vector<std::string> profile_validate(const Profile& p);

// One line for the window: what is in here and what it is worth.
// "3 names, 2 emails, 1 phone, 2 places -- 7 terms can confirm a page."
std::string profile_summary(const Profile& p);

// ── Logging ──────────────────────────────────────────────────────────────────
// COUNTS, never values. The same job `Case::log_ref` does, and the reason it
// exists is the same: the safe thing has to be the easy thing, because the
// alternative is a maintainer printing a struct while chasing a bug and
// putting a user's address in a file that gets attached to an issue.
//
// "profile: 1 name, 2 aka, 1 email, 1 phone, 0 user, 2 place, 5 needles"
std::string profile_log_ref(const Profile& p);

// ── Persistence ──────────────────────────────────────────────────────────────
// First-run tolerant: a missing file is an empty profile, not an error.
// Malformed JSON yields empty and reports via `error`.
//
// The save writes 0600 BEFORE the first byte of content, exactly as the egress
// policy does -- creating a world-readable file and narrowing it afterwards
// leaves a window in which a user's whole identity is on disk and readable.
//
// ── ON ENCRYPTION AT REST, WHICH THIS DOES NOT DO ────────────────────────────
// The open question at s13 close was "keyring or unencrypted", framed as a
// question about THIS file. It is not one, and the reason is next door:
// `cases.json` holds URLs like `spokeo.com/John-Smith/Tennessee/...`, which is
// a name and a state in a path. THE CASELOAD ALREADY CONTAINS THE PROFILE.
// Encrypting this file while that one sits beside it in the clear protects
// nothing and looks like it protects something, which is worse than doing
// neither.
//
// So the fork is not about the profile. It is "is the STATE DIRECTORY encrypted
// at rest", it covers three files, and it is a stone of its own. This pump
// takes a plain path so that stone can land underneath it without this header
// changing -- the same promise `core/Case`'s pump has carried since s02.
Profile profile_load(const std::string& file, std::string* error = nullptr);
bool    profile_save(const std::string& file, const Profile& p);

}  // namespace delr::core
