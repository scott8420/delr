#include "core/PageRules.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace delr::core {
namespace {

// The entities that actually show up between a marker and its match. Not a
// full table: an entity we don't decode leaves a `&nbsp;`-shaped smudge in the
// text, and the only cost of a smudge is a marker that has to be chosen a word
// earlier. A wrong decode would be worse.
struct Entity { const char* name; char sub; };
const Entity kEntities[] = {
    {"amp",  '&'},  {"lt",    '<'}, {"gt",   '>'}, {"quot", '"'},
    {"apos", '\''}, {"nbsp",  ' '}, {"#39",  '\''}, {"#34",  '"'},
    {"#38",  '&'},  {"#160",  ' '}, {"#8217", '\''}, {"#8220", '"'},
    {"#8221", '"'}, {"#8211", '-'}, {"#8212", '-'},
};

bool ci_starts(const std::string& s, std::size_t at, const char* lit) {
    std::size_t i = 0;
    for (; lit[i]; ++i) {
        if (at + i >= s.size()) return false;
        if (std::tolower(static_cast<unsigned char>(s[at + i])) !=
            std::tolower(static_cast<unsigned char>(lit[i])))
            return false;
    }
    return true;
}

// Skip a <script>/<style> element whole, contents included. `at` points at the
// '<'. Returns the index just past the closing tag, or npos when unterminated
// (an unterminated script is the rest of the file, and none of it is page).
std::size_t skip_element(const std::string& s, std::size_t at, const char* tag) {
    const std::string close = std::string("</") + tag;
    for (std::size_t i = at + 1; i + close.size() <= s.size(); ++i) {
        if (s[i] == '<' && ci_starts(s, i, close.c_str())) {
            std::size_t gt = s.find('>', i);
            return gt == std::string::npos ? std::string::npos : gt + 1;
        }
    }
    return std::string::npos;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Reading a page the way a person does
// ─────────────────────────────────────────────────────────────────────────────
std::string page_text(const std::string& body) {
    std::string out;
    out.reserve(body.size() / 2);

    bool pending_space = false;  // collapse runs; never emit a leading space
    auto push = [&](char c) {
        if (pending_space && !out.empty()) out.push_back(' ');
        pending_space = false;
        out.push_back(c);
    };

    for (std::size_t i = 0; i < body.size();) {
        const char c = body[i];

        if (c == '<') {
            // Comments and CDATA-ish things: skip to the terminator, not to the
            // first '>' -- a comment routinely contains markup.
            if (ci_starts(body, i, "<!--")) {
                std::size_t end = body.find("-->", i + 4);
                i = (end == std::string::npos) ? body.size() : end + 3;
                pending_space = true;
                continue;
            }
            if (ci_starts(body, i, "<script")) {
                std::size_t end = skip_element(body, i, "script");
                i = (end == std::string::npos) ? body.size() : end;
                pending_space = true;
                continue;
            }
            if (ci_starts(body, i, "<style")) {
                std::size_t end = skip_element(body, i, "style");
                i = (end == std::string::npos) ? body.size() : end;
                pending_space = true;
                continue;
            }
            std::size_t gt = body.find('>', i);
            i = (gt == std::string::npos) ? body.size() : gt + 1;
            // A tag is a word boundary: `a<br>b` is two words, not "ab".
            pending_space = true;
            continue;
        }

        if (c == '&') {
            std::size_t semi = body.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 8) {
                const std::string name = body.substr(i + 1, semi - i - 1);
                bool done = false;
                for (const Entity& e : kEntities) {
                    if (name == e.name) {
                        if (e.sub == ' ') pending_space = true;
                        else              push(static_cast<char>(std::tolower(
                                              static_cast<unsigned char>(e.sub))));
                        i = semi + 1;
                        done = true;
                        break;
                    }
                }
                if (done) continue;
            }
            // Not an entity we know: it is a literal ampersand on the page.
            push('&');
            ++i;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c))) {
            pending_space = true;
            ++i;
            continue;
        }

        push(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        ++i;
    }
    return out;
}

bool page_contains(const std::string& text, const std::string& needle) {
    const std::string n = page_text(needle);
    if (n.empty()) return false;  // an empty marker matches everything: refuse
    return text.find(n) != std::string::npos;
}

namespace {
bool any_match(const std::string& text, const std::vector<std::string>& list) {
    for (const std::string& m : list)
        if (page_contains(text, m)) return true;
    return false;
}
bool all_match(const std::string& text, const std::vector<std::string>& list) {
    if (list.empty()) return false;
    for (const std::string& m : list)
        if (!page_contains(text, m)) return false;
    return true;
}
bool needles_match(const std::string& text, const PageNeedles& n) {
    if (n.terms.empty()) return false;
    return n.require_all ? all_match(text, n.terms) : any_match(text, n.terms);
}
}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The rule set
// ─────────────────────────────────────────────────────────────────────────────
const PageRule* rules_find(const PageRules& r, const std::string& broker_id) {
    if (broker_id.empty()) return nullptr;
    for (const PageRule& p : r)
        if (p.broker_id == broker_id) return &p;
    return nullptr;
}

bool rule_verifiable(const PageRule* r) {
    // A null rule is verifiable. No rule means we cannot READ the page, which
    // is a maintainer's problem; funnel_only means there is no page to read,
    // which is nobody's. Collapsing them would put a broker that can never be
    // checked into the queue of brokers waiting for someone to write a rule.
    return !r || !r->funnel_only;
}

int rule_age_days(const PageRule& r, const std::string& today) {
    if (!date_valid(r.reviewed) || !date_valid(today)) return -1;
    // No day-count primitive in Case; walking is fine at this scale and reuses
    // the calendar-correct arithmetic rather than opening a second one.
    if (date_compare(r.reviewed, today) > 0) return -1;  // reviewed in the future
    std::string cursor = r.reviewed;
    for (int d = 0; d <= 4000; ++d) {
        if (date_compare(cursor, today) == 0) return d;
        cursor = date_add_days(cursor, 1);
        if (cursor.empty()) return -1;
    }
    return 4000;
}

bool rule_stale(const PageRule& r, const std::string& today, int max_age_days) {
    const int age = rule_age_days(r, today);
    if (age < 0) return true;  // never reviewed, or an unusable date: stale
    return age > max_age_days;
}

std::vector<std::string> rules_validate(const PageRules& r, const Roster* roster) {
    std::vector<std::string> problems;
    std::set<std::string> seen;

    auto check_list = [&](const std::string& who, const char* which,
                          const std::vector<std::string>& list) {
        if (list.empty()) {
            problems.push_back(who + ": " + which + " is empty");
            return;
        }
        for (const std::string& m : list) {
            const std::string n = page_text(m);
            if (n.empty()) {
                problems.push_back(who + ": blank " + which + " marker");
            } else if (n.size() < kMinMarker) {
                problems.push_back(who + ": " + which + " marker '" + n +
                                   "' is too short to mean anything");
            }
        }
    };

    for (const PageRule& p : r) {
        const std::string who = p.broker_id.empty() ? "<no broker_id>" : p.broker_id;
        if (p.broker_id.empty())
            problems.push_back("rule with no broker_id");
        else if (!seen.insert(p.broker_id).second)
            problems.push_back(who + ": duplicate rule");

        if (roster && !p.broker_id.empty() && !roster_find(*roster, p.broker_id))
            problems.push_back(who + ": no such broker in the roster");

        // A funnel-only rule carries one fact and no markers. Demanding a
        // fingerprint for a page that does not exist would force a maintainer
        // to invent one, and an invented marker is worse than an absent one --
        // it will match something, eventually, and produce a verdict about a
        // listing nobody ever published. Markers ARE still checked when
        // present, so a rule that was funnel-only and stops being one does not
        // smuggle junk through on the way back.
        if (!p.funnel_only) {
            check_list(who, "fingerprint", p.fingerprint);
            check_list(who, "present", p.present);
            check_list(who, "absent", p.absent);
        } else {
            if (p.needs_needle)
                problems.push_back(who + ": funnel_only and needs_needle "
                                         "together -- there is no page to find "
                                         "needles on");
            if (!p.fingerprint.empty() || !p.present.empty() || !p.absent.empty())
                problems.push_back(who + ": funnel_only rule carries markers "
                                         "that will never run");
        }

        // A string in both lists guarantees Ambiguous on every fetch, which is
        // a rule that refuses forever while looking configured.
        for (const std::string& a : p.present)
            for (const std::string& b : p.absent)
                if (!a.empty() && page_text(a) == page_text(b))
                    problems.push_back(who + ": '" + page_text(a) +
                                       "' is both a present and an absent marker");

        if (!p.reviewed.empty() && !date_valid(p.reviewed))
            problems.push_back(who + ": reviewed date is malformed");
    }
    return problems;
}

// ─────────────────────────────────────────────────────────────────────────────
// The verdict
// ─────────────────────────────────────────────────────────────────────────────
const char* page_verdict_name(PageVerdict v) {
    switch (v) {
        case PageVerdict::Present:         return "present";
        case PageVerdict::Absent:          return "absent";
        case PageVerdict::NeedleAbsent:    return "needle-absent";
        case PageVerdict::NoRule:          return "no-rule";
        case PageVerdict::NoNeedles:       return "no-needles";
        case PageVerdict::Unfingerprinted: return "unfingerprinted";
        case PageVerdict::Ambiguous:       return "ambiguous";
        case PageVerdict::Silent:          return "silent";
        case PageVerdict::Empty:           return "empty";
        case PageVerdict::NoResponse:      return "no-response";
        case PageVerdict::HttpDead:        return "http-dead";
        case PageVerdict::HttpBlocked:     return "http-blocked";
        case PageVerdict::HttpThrottled:   return "http-throttled";
        case PageVerdict::HttpServerError: return "http-server-error";
        case PageVerdict::HttpUnexpected:  return "http-unexpected";
        case PageVerdict::HttpBlockedEgress: return "http-blocked-egress";
        case PageVerdict::HttpBlockedClient: return "http-blocked-client";
        case PageVerdict::FunnelOnly:        return "funnel-only";
    }
    return "silent";
}

const char* page_verdict_text(PageVerdict v) {
    switch (v) {
        case PageVerdict::Present:
            return "The listing is still there.";
        case PageVerdict::Absent:
            return "The page says the record is not there.";
        case PageVerdict::NeedleAbsent:
            return "The page loaded and your details are not on it.";
        case PageVerdict::NoRule:
            return "No page rule for this broker, so this page cannot be read. "
                   "The listing was reached, not checked.";
        case PageVerdict::NoNeedles:
            return "This broker's pages are checked against your own details, "
                   "and no details were available to check.";
        case PageVerdict::Unfingerprinted:
            return "This is not the page the rule describes -- a login wall, an "
                   "error page, or a redesign. Nothing was concluded.";
        case PageVerdict::Ambiguous:
            return "The page reads as both listed and not listed. The rule is "
                   "wrong and needs a maintainer.";
        case PageVerdict::Silent:
            return "The page is the broker's, but says neither that the record "
                   "is there nor that it is gone. The rule has gone out of date.";
        case PageVerdict::Empty:
            return "The response had no readable text.";
        case PageVerdict::NoResponse:
            return "Nothing came back. The site may be down or unreachable.";
        case PageVerdict::HttpDead:
            return "The page is gone. That may mean removal, or a retired "
                   "address for the same record -- it is not proof either way.";
        case PageVerdict::HttpBlocked:
            return "The site refused the request. It is blocking automated "
                   "checks, not answering about the listing.";
        case PageVerdict::HttpThrottled:
            return "The site asked us to slow down. Checked too often, or "
                   "sharing an exit with other traffic.";
        case PageVerdict::HttpServerError:
            return "The site failed on its own end. Worth retrying later.";
        case PageVerdict::HttpUnexpected:
            return "The site answered in a way the check does not understand.";

        // The three sentences this whole stone exists to produce. Each one
        // names who can act, because that is the only thing a person reading a
        // refusal actually needs to know.
        case PageVerdict::HttpBlockedEgress:
            return "The site refused the address the check came from. It blocks "
                   "VPN and datacenter traffic. A different exit, or a different "
                   "city on the same one, may get through.";
        case PageVerdict::HttpBlockedClient:
            return "The site refused the check itself, not the address -- it "
                   "wants a real browser. The same page opens normally through "
                   "the same tunnel. This one is ours to fix, not yours.";
        case PageVerdict::FunnelOnly:
            return "This broker publishes no page to check. Results are "
                   "assembled behind a form and never live at a fixed address, "
                   "so no fetch can confirm a removal here. Filing is the only "
                   "lever on this one.";
    }
    return "The page could not be read.";
}

bool page_verdict_is_clean(PageVerdict v) {
    return v == PageVerdict::Present || v == PageVerdict::Absent ||
           v == PageVerdict::NeedleAbsent;
}

PageVerdict page_check(const PageRule* rule, int status, const std::string& body,
                       const PageNeedles& needles) {
    // 0. Is there anything here to check. Declared by a human, not observed --
    //    a funnel-only broker answers 200 with a plausible page and still has
    //    no listing at this address. It is checked FIRST, before the status,
    //    because whatever came back is irrelevant: the caller should not have
    //    fetched at all (`rule_verifiable`), and if it did, the response is not
    //    evidence about a listing that was never published.
    if (rule && rule->funnel_only) return PageVerdict::FunnelOnly;

    // 1. The status. A 403's body belongs to the bot wall, and running a
    //    broker's markers over someone else's page is how a wall becomes a
    //    removal. It is still read -- by `refusal_attribute`, and only about
    //    ITSELF. The wall is a bad witness about the listing and a good one
    //    about what it just refused.
    if (status <= 0)                    return PageVerdict::NoResponse;
    if (status == 404 || status == 410) return PageVerdict::HttpDead;
    if (status == 401 || status == 403) return refusal_attribute(body);
    if (status == 429)                  return PageVerdict::HttpThrottled;
    if (status >= 500)                  return PageVerdict::HttpServerError;
    if (status != 200)                  return PageVerdict::HttpUnexpected;

    // 2. Is there a rule at all.
    if (!rule) return PageVerdict::NoRule;

    // 3. Text at all.
    const std::string text = page_text(body);
    if (text.empty()) return PageVerdict::Empty;

    // 4. Is this the page the rule was written for. Everything after this line
    //    assumes the answer is yes, and the assumption is the reason the rest
    //    of the file can be trusted.
    if (!all_match(text, rule->fingerprint)) return PageVerdict::Unfingerprinted;

    const bool has_needles = !needles.terms.empty();
    if (rule->needs_needle && !has_needles) return PageVerdict::NoNeedles;

    const bool present = any_match(text, rule->present);
    const bool absent  = any_match(text, rule->absent);

    // 5. Both. Not a precedence call: choosing a winner here is choosing which
    //    lie to tell, and either lie is worse than saying the rule is broken.
    if (present && absent) return PageVerdict::Ambiguous;

    // 6. The broker's own words about absence outrank our inference from them.
    if (absent) return PageVerdict::Absent;

    const bool needle_hit = has_needles && needles_match(text, needles);

    if (present) {
        // On a per-person page, a presence marker without the person is the
        // template, not the listing. Fall through rather than reporting a
        // sighting we did not have.
        if (!rule->needs_needle || needle_hit) return PageVerdict::Present;
        return PageVerdict::NeedleAbsent;
    }

    // 7. No marker fired. On a rule that opted into identity matching, a
    //    correctly fingerprinted page without the listing's details on it is
    //    the observation this app exists to make -- weaker than the broker
    //    saying so, and named separately so the difference survives into the
    //    report.
    if (rule->needs_needle && has_needles && !needle_hit)
        return PageVerdict::NeedleAbsent;

    return PageVerdict::Silent;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reading the wall's own words
//
// Two closed phrase sets, normalised through `page_text` like every other
// marker in this file. Substring matching only: this body arrives from a
// hostile network and a regex over it is an attack surface.
//
// Choosing what goes in these lists is the whole design, and the rule is: a
// phrase earns its place only if a wall would print it ABOUT THE THING IT
// REFUSED. Generic anger ("access denied", "forbidden", "request blocked")
// appears on both kinds of wall and is therefore worthless here -- it is
// exactly what plain `HttpBlocked` already means. What distinguishes them is
// the noun: an address wall talks about addresses and networks, a client wall
// talks about browsers and scripts.
//
// Both lists are short on purpose and should stay short. A miss costs one
// unattributed `Blocked`, which is the honest default anyway. A false hit
// sends the user to change VPN exits over a User-Agent header, or tells them
// to sit tight waiting for a fix while a wall they could have walked around
// keeps refusing them. The asymmetry says: when unsure, leave it out.
//
// Deliberately NOT a list of vendor names. Naming the three big bot-defence
// products would rot on the next rebrand and would teach the app to recognise
// companies rather than sentences. Walls that must explain themselves to a
// human converge on the same dozen words whoever built them.
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// The wall refused WHERE the request came from. The user can act on this today.
const char* const kEgressPhrases[] = {
    "vpn",
    "proxy",
    "datacenter",
    "data center",
    "hosting provider",
    "anonymiser",
    "anonymizer",
    "your ip address",
    "this ip address",
    "ip address has been blocked",
    "not available in your country",
    "not available in your region",
    "unavailable in your location",
};

// The wall refused WHAT made the request. Ours to fix; the user cannot.
const char* const kClientPhrases[] = {
    "enable javascript",
    "javascript is required",
    "javascript to continue",
    "requires javascript",
    "supports javascript",
    "browser check",
    "checking your browser",
    "verify you are human",
    "are you a robot",
    "confirm you are not a robot",
    "human verification",
    "automated traffic",
    "automated requests",
    "unusual traffic",
    "bot detected",
    "enable cookies",
    "cookies are required",
};

bool any_phrase(const std::string& text, const char* const* set, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (page_contains(text, set[i])) return true;
    return false;
}

}  // namespace

PageVerdict refusal_attribute(const std::string& body) {
    const std::string text = page_text(body);
    if (text.empty()) return PageVerdict::HttpBlocked;

    const bool egress = any_phrase(text, kEgressPhrases,
                                   sizeof(kEgressPhrases) / sizeof(*kEgressPhrases));
    const bool client = any_phrase(text, kClientPhrases,
                                   sizeof(kClientPhrases) / sizeof(*kClientPhrases));

    // Both fired, or neither. Not a precedence call -- the same refusal as
    // `Ambiguous`, for the same reason: a wall that talks about VPNs AND about
    // browsers has told us it is a wall and nothing more, and picking the
    // louder list would be inventing a diagnosis out of word counts.
    // Unattributed is the truth.
    if (egress == client) return PageVerdict::HttpBlocked;

    return egress ? PageVerdict::HttpBlockedEgress : PageVerdict::HttpBlockedClient;
}

// ─────────────────────────────────────────────────────────────────────────────
// The seam to the caseload
// ─────────────────────────────────────────────────────────────────────────────
PageOutcome page_outcome(PageVerdict v) {
    switch (v) {
        case PageVerdict::Present:
            return {Outcome::Listed, Reason::None};
        case PageVerdict::Absent:
        case PageVerdict::NeedleAbsent:
            return {Outcome::NotFound, Reason::None};

        case PageVerdict::NoRule:
            return {Outcome::Indeterminate, Reason::NoRule};
        case PageVerdict::NoNeedles:
        case PageVerdict::Unfingerprinted:
        case PageVerdict::Ambiguous:
        case PageVerdict::Silent:
        case PageVerdict::Empty:
            return {Outcome::Indeterminate, Reason::PageUnreadable};

        case PageVerdict::NoResponse:
            return {Outcome::Indeterminate, Reason::Timeout};
        case PageVerdict::HttpDead:
            return {Outcome::Indeterminate, Reason::UrlDead};
        case PageVerdict::HttpBlocked:
            return {Outcome::Indeterminate, Reason::Blocked};
        case PageVerdict::HttpBlockedEgress:
            return {Outcome::Indeterminate, Reason::EgressBlocked};
        case PageVerdict::HttpBlockedClient:
            return {Outcome::Indeterminate, Reason::ClientBlocked};
        case PageVerdict::FunnelOnly:
            return {Outcome::Indeterminate, Reason::NoListingPage};
        case PageVerdict::HttpThrottled:
            return {Outcome::Indeterminate, Reason::RateLimited};
        case PageVerdict::HttpServerError:
        case PageVerdict::HttpUnexpected:
            return {Outcome::Indeterminate, Reason::BadResponse};
    }
    return {Outcome::Indeterminate, Reason::PageUnreadable};
}

namespace {
// Whose bug is it. The streak is a claim about the listing, so only the
// broker's and the network's failures may move it.
bool ours(PageVerdict v) {
    switch (v) {
        case PageVerdict::NoRule:
        case PageVerdict::NoNeedles:
        case PageVerdict::Unfingerprinted:
        case PageVerdict::Ambiguous:
        case PageVerdict::Silent:
        case PageVerdict::Empty:
        // The attributed walls join the list (s15). Both are configuration --
        // our exit, our client shape -- and neither is the far end saying
        // anything about the listing. Left in the streak they would read,
        // months later, as a broker that keeps refusing us, and could argue a
        // live case toward Abandoned on the strength of a settings choice.
        // Plain `HttpBlocked` deliberately stays out: an unattributed wall
        // might really be the broker, and we do not get to assume otherwise.
        case PageVerdict::HttpBlockedEgress:
        case PageVerdict::HttpBlockedClient:
        // Nobody's failure at all, so nothing to count.
        case PageVerdict::FunnelOnly:
            return true;
        default:
            return false;
    }
}
}  // namespace

Case apply_page_verdict(const Case& c, PageVerdict v, const std::string& today,
                        int recheck_days) {
    const PageOutcome o = page_outcome(v);
    Case n = apply_check(c, o.outcome, o.reason, today, recheck_days);
    // apply_check bumped the streak, and was right to for every failure that
    // belongs to the far end. Not for ours.
    if (ours(v)) n.consecutive_failures = c.consecutive_failures;
    return n;
}

std::vector<const Case*> caseload_walled(const Caseload& c) {
    std::vector<const Case*> out;
    for (const Case& k : c) {
        if (k.outcome != Outcome::Indeterminate) continue;
        if (k.reason == Reason::Blocked || k.reason == Reason::EgressBlocked ||
            k.reason == Reason::ClientBlocked)
            out.push_back(&k);
    }
    return out;
}

std::vector<const Case*> caseload_unverifiable_by_design(const Caseload& c) {
    std::vector<const Case*> out;
    for (const Case& k : c) {
        if (k.outcome != Outcome::Indeterminate) continue;
        if (k.reason == Reason::NoListingPage) out.push_back(&k);
    }
    return out;
}

std::vector<const Case*> caseload_unverifiable(const Caseload& c) {
    std::vector<const Case*> out;
    for (const Case& k : c) {
        if (k.outcome != Outcome::Indeterminate) continue;
        if (k.reason == Reason::NoRule || k.reason == Reason::PageUnreadable)
            out.push_back(&k);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Persistence pump -- encode and decode adjacent so they cannot drift
// ─────────────────────────────────────────────────────────────────────────────
namespace {

std::vector<std::string> str_list(const json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.contains(key) || !j.at(key).is_array()) return out;
    for (const json& e : j.at(key))
        if (e.is_string()) out.push_back(e.get<std::string>());
    return out;
}

std::string str_or(const json& j, const char* key, const char* fallback = "") {
    if (j.contains(key) && j.at(key).is_string()) return j.at(key).get<std::string>();
    return fallback;
}

}  // namespace

PageRules rules_load(const std::string& file, std::string* error) {
    if (error) error->clear();
    std::ifstream in(file);
    if (!in) return {};  // first run: no rules is not an error

    PageRules out;
    try {
        json root;
        in >> root;
        if (!root.contains("rules") || !root.at("rules").is_array()) {
            if (error) *error = "no 'rules' array";
            return {};
        }
        for (const json& j : root.at("rules")) {
            if (!j.is_object()) continue;
            PageRule p;
            p.broker_id   = str_or(j, "broker_id");
            p.fingerprint = str_list(j, "fingerprint");
            p.present     = str_list(j, "present");
            p.absent      = str_list(j, "absent");
            p.needs_needle = j.contains("needs_needle") && j.at("needs_needle").is_boolean()
                                 ? j.at("needs_needle").get<bool>() : false;
            p.funnel_only  = j.contains("funnel_only") && j.at("funnel_only").is_boolean()
                                 ? j.at("funnel_only").get<bool>() : false;
            p.reviewed    = str_or(j, "reviewed");
            p.notes       = str_or(j, "notes");
            out.push_back(p);
        }
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return {};
    }
    return out;
}

bool rules_save(const std::string& file, const PageRules& r) {
    json root;
    root["version"] = 1;
    root["_comment"] =
        "Page rules -- what a fetched page has to say for a listing to count as "
        "gone. Separate from the roster on purpose: this file rots on a "
        "designer's schedule, not a company's. Every rule needs a fingerprint "
        "(is this the broker's page at all), a present list and an absent list; "
        "re-read them against the live page and bump 'reviewed'.";
    root["rules"] = json::array();
    for (const PageRule& p : r) {
        json j;
        j["broker_id"]    = p.broker_id;
        j["fingerprint"]  = p.fingerprint;
        j["present"]      = p.present;
        j["absent"]       = p.absent;
        j["needs_needle"] = p.needs_needle;
        j["funnel_only"]  = p.funnel_only;
        j["reviewed"]     = p.reviewed;
        j["notes"]        = p.notes;
        root["rules"].push_back(j);
    }
    std::ofstream out(file);
    if (!out) return false;
    out << root.dump(2) << "\n";
    return out.good();
}

}  // namespace delr::core
