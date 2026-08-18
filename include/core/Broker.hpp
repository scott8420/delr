#pragma once
#include <cstddef>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// core/Broker -- the roster: who holds data, and how they let you out.
//
// This is table one, and it is the whole asset. The code around it is a
// weekend; this list is the thing that rots and the thing worth maintaining.
// It ships as versioned JSON so it can be fetched and updated independently of
// the binary.
//
// GTK-free and headless-testable. The CALLER resolves the on-disk path and
// hands it in -- the UI does the XDG/asset resolution and passes a plain
// string, so the core stays free of GTK (CANON: pumps at conceptual seams;
// data in the core, dir resolution and behaviour in the UI).
//
// Encode and decode live in ONE file so a write can't skew from its read.
// Round-trip fidelity is the bar: what you save is what you load.
//
// NOTE: no PII lives here. The roster is public knowledge about companies.
// The user's own identifiers are a separate concern with separate storage
// rules, and deliberately not modelled yet.
// ─────────────────────────────────────────────────────────────────────────────
namespace delr::core {

// How this broker accepts an opt-out. Drop == covered by California's DROP
// platform, i.e. a registered broker that must poll the state system every 45
// days -- for a CA resident that channel supersedes writing to them directly.
enum class Method { Web, Email, Postal, Phone, Drop, Unknown };

const char* method_name(Method m);
Method      method_from(const std::string& s);

struct Broker {
    std::string id;              // stable slug, e.g. "spokeo"
    std::string name;            // display name
    std::string site;            // homepage -- the one shown to a human

    // ── One registrant, many sites ───────────────────────────────────────────
    // `site` is the display homepage; `hosts` is every domain this ONE company
    // publishes listings under. They are not the same thing and conflating
    // them breaks the app on precisely the brokers it exists for.
    //
    // The CPPA registry proves it: BeenVerified registers twelve domains
    // (peoplelooker, ownerly, neighborwho, numberguru...) in a single row, and
    // Mississippi Tornado Alley registers ten (fastpeoplesearch, usphonebook,
    // searchpeoplefree...). Seven registrants between them carry ~37 listing
    // domains. Those seven are almost exactly the people-search brokers -- the
    // rows with pages a person can be FOUND on, which is to say the rows the
    // verification half of this program is for.
    //
    // The alternative -- one Broker per domain -- is wrong in the other
    // direction. Twelve BeenVerified entries means twelve opt-out emails to
    // one inbox for one legal entity that a single request already covers. The
    // registrant is the unit that receives a request; the host is the unit
    // that gets matched. So: one entry, many hosts.
    //
    // `site`'s host is matched too and does not need repeating here, though a
    // duplicate is harmless -- matching takes the best hit, not the first.
    std::vector<std::string> hosts;

    Method      method = Method::Unknown;
    std::string opt_out_url;     // for Method::Web
    std::string opt_out_email;   // for Method::Email
    bool        requires_id = false;   // demands a photo ID upload
    int         recheck_days = 45;     // how often to re-verify a removal
    bool        ca_registered = false; // on the CalPrivacy data broker registry

    // ── Two facts the registry states that change what the USER does ─────────
    // Registrants declare a dozen attributes and most are compliance trivia.
    // These two are not:
    //
    // `fcra_regulated` -- an FCRA-regulated broker may LAWFULLY refuse to
    // delete. A refusal from one of these is a correct outcome, not a failed
    // request, and a tool that reports it as failure teaches the user to
    // distrust the tool rather than to understand the law. 18 of 543 declare
    // it.
    //
    // `collects_geo` -- precise geolocation. This is the attribute that
    // decides whether a listing is an annoyance or a safety problem, and it is
    // the one a person fleeing someone needs surfaced first. 88 of 543 declare
    // it.
    //
    // Minors and reproductive-health collection are also declared and are
    // deliberately NOT carried: both are grounds for outrage and neither
    // changes what this app does about a listing. A field that cannot change
    // an action is a field that rots.
    bool        fcra_regulated = false;
    bool        collects_geo   = false;

    std::string notes;
};

using Roster = std::vector<Broker>;

// Lookup. Returns nullptr when absent.
const Broker* roster_find(const Roster& r, const std::string& id);

// Validation -- one list, N consumers, so the list itself is guarded. Returns
// a human-readable problem per issue; empty means clean. A roster that fails
// this is a bug in the DATA, which is exactly the failure this project must
// catch early and loudly (CANON: dead mapping entries lie about structure).
std::vector<std::string> roster_validate(const Roster& r);

// Persistence pump -- encode + decode adjacent so they can't drift.
// load is first-run tolerant: a missing file yields an empty roster, not an
// error. Malformed JSON yields empty and reports via `error` when supplied.
Roster roster_load(const std::string& file, std::string* error = nullptr);
bool   roster_save(const std::string& file, const Roster& r);

}  // namespace delr::core
