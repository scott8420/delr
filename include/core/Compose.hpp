#pragma once
#include <string>
#include <vector>

#include "core/Broker.hpp"
#include "core/Case.hpp"
#include "core/Profile.hpp"
#include "core/Statute.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// core/Compose -- the request. Written by the app, sent by the person.
//
// ── COMPOSE, DO NOT SEND. This is a permanent property, not a phase. ─────────
//
// Nothing in this module opens a socket, and nothing above it may either. The
// request comes back as text and a channel; carrying it the last inch is the
// user's act, in the user's mail client, from the user's own account.
//
// This is not caution about SMTP being fiddly. Three reasons, and the third is
// the one that would still hold if the other two were solved:
//
//   1. Sending needs credentials. An app that sends mail holds the password to
//      the inbox that receives every reply about every listing the user is
//      trying to remove. That is a custody problem this program does not have
//      today and would be taking on for a convenience.
//
//   2. The moment of pressing send is the moment the user reads what is about
//      to leave. Filing an opt-out is an OUTBOUND DISCLOSURE to a data broker
//      -- see below -- and an app that sends on the user's behalf removes the
//      one checkpoint where a wrong address, a stale listing or an over-full
//      request gets caught. s16 made every observed sentence copyable for the
//      same reason: the human has to be able to see the thing.
//
//   3. Mail sent by a person, from their own account, with their own name on
//      it, IS the request. Mail sent by a robot on their behalf invites the
//      answer "we could not verify this came from you", and verification is a
//      right the recipient actually has. Compose-don't-send is not a weaker
//      version of sending; for this particular errand it is the stronger one.
//
// ── THE ONE PLACE THIS PROGRAM DISCLOSES ────────────────────────────────────
//
// Every other path in delr moves information TOWARD the user: a page is
// fetched, a verdict is recorded, a history is kept. This one moves
// information AWAY, to a company whose business is collecting it. That
// inversion is the entire design of this file.
//
// So: MINIMUM VIABLE DISCLOSURE. The default request carries the listing URL,
// the name printed on the listing, and an address to reply to. The URL and the
// name tell the broker nothing they did not already publish themselves -- it
// is their page. The reply address is the only genuinely new fact, and without
// it they cannot answer.
//
// Everything else on the profile -- other spellings of the name, phone
// numbers, places lived, other email addresses -- is NEW INFORMATION handed to
// a data broker in order to ask them to hold less of it. Each is off by
// default, each is a deliberate act by the user through `ComposeOptions`, and
// turning any of them on raises `Caution::DisclosesExtra` so the trade is
// stated rather than assumed. Sometimes it is the right trade: a common name
// on a big broker may not be findable without a city. It is still a trade.
//
// `Profile::note` and `Case::note` NEVER appear in a request. Both are marked
// "PII-adjacent by assumption" and free text is where a person writes the
// thing they would not have chosen to send. Same rule as `core/Journal`'s: the
// files most likely to leave the machine carry no prose.
//
// ── The composed text is never persisted ────────────────────────────────────
//
// It is the most PII-dense string this program produces -- name, jurisdiction,
// listing and reply address in one place -- and it exists to be handed
// straight to a mail client. It is not written to the caseload, not to the
// journal, and not to the log. `core/Journal` records that a request was FILED
// and through which channel; it does not record what it said. If the user
// wants a copy, their sent-mail folder is the copy, and it is a better one.
// Same shape as the fetched page dying in the worker's stack frame.
//
// ── NO ROW, NO CLAIM ────────────────────────────────────────────────────────
//
// When `core/Statute` has nothing for the user's jurisdiction, the request is
// composed as a COURTESY -- against the broker's own posted policy -- and says
// so. It does not cite a law, does not state a deadline, and does not imply
// one. Fail closed, applied to a right instead of to a fetch. A bluff is
// checkable in thirty seconds by the compliance desk that reads it.
//
// GTK-free, pure, and headless-testable: text in, text out, no clock (the
// caller supplies `today`), no filesystem, no network.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::core {

// ── What the user has to know before they press send ────────────────────────
// Not errors. A request with four cautions on it may still be exactly the
// right thing to send; what they have in common is that the user would be
// annoyed to learn any of them AFTERWARDS.
//
// They are values rather than sentences for the same reason `Reason` is: the
// UI decides the words, the tests assert on the value, and the set is
// enumerable so no case goes unhandled.
enum class Caution {
    // ── Blocking: there is nothing to send, or nobody to send it as ──────────
    NoName,               // the profile has no name; the request cannot identify anyone
    NoChannel,            // the roster holds no address of any kind for this broker
    ChannelNotComposable, // the declared method is a phone call

    // ── The request goes, but weaker than it could ───────────────────────────
    NoContactEmail,       // no reply address: they cannot answer you
    NoResidency,          // the profile does not say where you live
    NoStatute,            // no law for that place: this is a courtesy request
    NoCitation,           // a law, but nobody has verified a section number

    // ── About the broker, and each changes what to expect back ───────────────
    WebFormOnly,          // paste it into their form; there may be no free-text box
    RequiresId,           // they demand a photo ID upload before acting
    FcraRegulated,        // they may LAWFULLY refuse. A refusal is a correct outcome.
    DropCovered,          // a state platform covers this broker and covers you
    DropNotYours,         // it covers this broker and does NOT cover you

    // ── About what this particular request costs you ─────────────────────────
    DisclosesExtra,       // you chose to include more than the listing already shows
    UnverifiedListing     // we have never fetched this page cleanly. See below.
};

const char* caution_name(Caution c);

// The sentence the window shows. Lives here rather than in the UI so the words
// are checkable and so two surfaces cannot describe the same caution
// differently -- the same reason `verdict_text` and `page_verdict_text` are in
// the core.
const char* caution_text(Caution c);

// True for the three that mean nothing can go out. A window greys the button
// on these and only these; the rest are things to read, not to fix.
bool caution_blocking(Caution c);

// ── What to put in beyond the minimum ────────────────────────────────────────
// All false, permanently. A default that discloses is a default nobody chose.
struct ComposeOptions {
    bool include_aliases = false;  // other spellings the record may be under
    bool include_places  = false;  // cities lived in -- disambiguates a common name
    bool include_phones  = false;  // printed forms, as the listing would show them
    bool include_emails  = false;  // the other addresses, beyond the reply one
};

// True when any of them is set, i.e. when the request says more than the
// broker's own page already does.
bool options_disclose_extra(const ComposeOptions& o);

// ── One request ─────────────────────────────────────────────────────────────
struct Request {
    // The channel this would actually go out on -- derived from what the
    // roster HAS, not only from what it declares. See `compose_channel`.
    Method channel = Method::Unknown;

    // An email address for `Email`, a form URL for `Web`, empty otherwise.
    std::string to;

    std::string subject;
    std::string body;

    // Sorted, unique, blocking ones first. The order is part of the contract:
    // a window that prints them in composition order would bury "there is
    // nowhere to send this" under "they might ask for ID".
    std::vector<Caution> cautions;

    // The row this request stands on, or empty when it stands on none. Kept as
    // an id rather than a copy so a later amendment to the statute table can
    // be seen for what it is instead of silently restating what was claimed.
    std::string statute_id;

    // From the row, and zero when there is none. NEVER rendered as "due
    // immediately": a zero here means no deadline was claimed.
    int respond_days = 0;

    // Something to say, somewhere to send it, and somebody to send it as.
    // Equivalent to "no blocking caution", and carried as a field so the UI
    // does not re-derive the rule.
    bool sendable = false;
};

// The channel a request to this broker would really use. The declared
// `Method` first, then what the roster actually holds -- a broker declaring
// `email` with no `opt_out_email` has no email channel, whatever the column
// says, and a request composed to nowhere is worse than one that admits it.
Method compose_channel(const Broker& b);

// The whole job. `law` may be null and null is the common case; see the
// header. `today` is the caller's clock, as everywhere in the core.
Request compose_request(const Profile& p, const Broker& b, const Case& k,
                        const Statute* law, const std::string& today,
                        const ComposeOptions& opt = {});

bool request_has(const Request& r, Caution c);

// Safe log identifier -- "request:<channel>/<n> cautions". No body, no
// address, no url, no name. Anything that logs a request logs THIS.
std::string compose_log_ref(const Request& r);

}  // namespace delr::core
