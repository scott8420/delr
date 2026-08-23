#include "core/Case.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>

using json = nlohmann::json;

namespace delr::core {

// ── Enum tables ──────────────────────────────────────────────────────────────
// Named, not bare integers, and round-tripped in the selftest. The status of a
// case is the thing a report renders; a silent rename here would rewrite
// history on the next save.

const char* status_name(Status s) {
    switch (s) {
        case Status::Found:     return "found";
        case Status::Requested: return "requested";
        case Status::Removed:   return "removed";
        case Status::Relisted:  return "relisted";
        case Status::Abandoned: return "abandoned";
        case Status::Unknown:   return "unknown";
    }
    return "unknown";
}

Status status_from(const std::string& s) {
    if (s == "found")     return Status::Found;
    if (s == "requested") return Status::Requested;
    if (s == "removed")   return Status::Removed;
    if (s == "relisted")  return Status::Relisted;
    if (s == "abandoned") return Status::Abandoned;
    return Status::Unknown;
}

const char* provenance_name(Provenance p) {
    switch (p) {
        case Provenance::None:          return "none";
        case Provenance::BrokerClaim:   return "broker-claim";
        case Provenance::PlatformClaim: return "platform-claim";
        case Provenance::SelfVerified:  return "self-verified";
    }
    return "none";
}

Provenance provenance_from(const std::string& s) {
    if (s == "broker-claim")   return Provenance::BrokerClaim;
    if (s == "platform-claim") return Provenance::PlatformClaim;
    if (s == "self-verified")  return Provenance::SelfVerified;
    return Provenance::None;
}

const char* outcome_name(Outcome o) {
    switch (o) {
        case Outcome::Listed:        return "listed";
        case Outcome::NotFound:      return "not-found";
        case Outcome::Indeterminate: return "indeterminate";
        case Outcome::Never:         return "never";
    }
    return "never";
}

Outcome outcome_from(const std::string& s) {
    if (s == "listed")        return Outcome::Listed;
    if (s == "not-found")     return Outcome::NotFound;
    if (s == "indeterminate") return Outcome::Indeterminate;
    return Outcome::Never;
}

const char* reason_name(Reason r) {
    switch (r) {
        case Reason::None:        return "none";
        case Reason::NoTunnel:    return "no-tunnel";
        case Reason::Blocked:     return "blocked";
        case Reason::Captcha:     return "captcha";
        case Reason::RateLimited: return "rate-limited";
        case Reason::Timeout:     return "timeout";
        case Reason::BadResponse: return "bad-response";
        case Reason::UrlDead:     return "url-dead";
        case Reason::NoRule:         return "no-rule";
        case Reason::PageUnreadable: return "page-unreadable";
        case Reason::EgressBlocked:  return "egress-blocked";
        case Reason::ClientBlocked:  return "client-blocked";
        case Reason::NoListingPage:  return "no-listing-page";
    }
    return "none";
}

Reason reason_from(const std::string& s) {
    if (s == "no-tunnel")    return Reason::NoTunnel;
    if (s == "blocked")      return Reason::Blocked;
    if (s == "captcha")      return Reason::Captcha;
    if (s == "rate-limited") return Reason::RateLimited;
    if (s == "timeout")      return Reason::Timeout;
    if (s == "bad-response") return Reason::BadResponse;
    if (s == "url-dead")     return Reason::UrlDead;
    if (s == "no-rule")         return Reason::NoRule;
    if (s == "page-unreadable") return Reason::PageUnreadable;
    if (s == "egress-blocked")  return Reason::EgressBlocked;
    if (s == "client-blocked")  return Reason::ClientBlocked;
    if (s == "no-listing-page") return Reason::NoListingPage;
    return Reason::None;
}

const char* field_name(Field f) {
    switch (f) {
        case Field::Name:           return "name";
        case Field::Aliases:        return "aliases";
        case Field::Age:            return "age";
        case Field::Dob:            return "dob";
        case Field::Address:        return "address";
        case Field::AddressHistory: return "address-history";
        case Field::Phone:          return "phone";
        case Field::Email:          return "email";
        case Field::Relatives:      return "relatives";
        case Field::Employer:       return "employer";
        case Field::Other:          return "other";
    }
    return "other";
}

Field field_from(const std::string& s) {
    if (s == "name")            return Field::Name;
    if (s == "aliases")         return Field::Aliases;
    if (s == "age")             return Field::Age;
    if (s == "dob")             return Field::Dob;
    if (s == "address")         return Field::Address;
    if (s == "address-history") return Field::AddressHistory;
    if (s == "phone")           return Field::Phone;
    if (s == "email")           return Field::Email;
    if (s == "relatives")       return Field::Relatives;
    if (s == "employer")        return Field::Employer;
    return Field::Other;
}

// ── Dates ────────────────────────────────────────────────────────────────────
// Days-from-civil / civil-from-days (the standard proleptic Gregorian
// algorithm). Written out rather than pulled from <chrono>'s calendar support,
// which is C++20 and this project is C++17. It is ~20 lines, it is exact, and
// the selftest walks it across a leap day and a century boundary -- the two
// places every hand-rolled date routine gets it wrong.

namespace {

bool parse_iso(const std::string& iso, int& y, unsigned& m, unsigned& d) {
    if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-') return false;
    for (std::size_t i = 0; i < iso.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (iso[i] < '0' || iso[i] > '9') return false;
    }
    y = std::atoi(iso.substr(0, 4).c_str());
    m = static_cast<unsigned>(std::atoi(iso.substr(5, 2).c_str()));
    d = static_cast<unsigned>(std::atoi(iso.substr(8, 2).c_str()));
    if (m < 1 || m > 12 || d < 1) return false;

    static const unsigned len[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned dim = len[m - 1];
    if (m == 2) {
        const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        if (leap) dim = 29;
    }
    return d <= dim;
}

long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long>(era) * 146097 + static_cast<long>(doe) - 719468;
}

void civil_from_days(long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long yy = static_cast<long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = static_cast<int>(yy + (m <= 2));
}

std::string fmt_iso(int y, unsigned m, unsigned d) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", y, m, d);
    return buf;
}

}  // namespace

bool date_valid(const std::string& iso) {
    int y; unsigned m, d;
    return parse_iso(iso, y, m, d);
}

std::string date_add_days(const std::string& iso, int days) {
    int y; unsigned m, d;
    if (!parse_iso(iso, y, m, d)) return {};
    long z = days_from_civil(y, m, d) + days;
    civil_from_days(z, y, m, d);
    return fmt_iso(y, m, d);
}

int date_days_between(const std::string& a, const std::string& b) {
    int ya, yb; unsigned ma, da, mb, db;
    if (!parse_iso(a, ya, ma, da) || !parse_iso(b, yb, mb, db)) return 0;
    return static_cast<int>(days_from_civil(yb, mb, db) - days_from_civil(ya, ma, da));
}

int date_compare(const std::string& a, const std::string& b) {
    const bool va = date_valid(a), vb = date_valid(b);
    if (!va && !vb) return 0;
    if (!va) return -1;
    if (!vb) return 1;
    return a.compare(b);   // ISO dates are lexicographically ordered by design
}

// ── Queries ──────────────────────────────────────────────────────────────────

const Case* caseload_find(const Caseload& c, const std::string& id) {
    for (const auto& k : c) if (k.id == id) return &k;
    return nullptr;
}

std::vector<const Case*> caseload_due(const Caseload& c, const std::string& today) {
    std::vector<const Case*> due;
    if (!date_valid(today)) return due;
    for (const auto& k : c) {
        if (k.status == Status::Abandoned || k.status == Status::Relisted) continue;
        if (!date_valid(k.next_check)) continue;
        if (date_compare(k.next_check, today) <= 0) due.push_back(&k);
    }
    std::sort(due.begin(), due.end(), [](const Case* a, const Case* b) {
        const int c2 = date_compare(a->next_check, b->next_check);
        return c2 != 0 ? c2 < 0 : a->id < b->id;   // id breaks ties: stable output
    });
    return due;
}

std::vector<FieldCount> exposure_by_field(const Caseload& c, bool include_removed) {
    std::map<int, int> tally;
    for (const auto& k : c) {
        if (k.status == Status::Abandoned) continue;
        if (!include_removed && k.status == Status::Removed) continue;
        // A field counts ONCE per case however many times it's listed -- the
        // report's claim is "on N sites", not "N mentions".
        std::set<int> once;
        for (auto f : k.exposes) once.insert(static_cast<int>(f));
        for (int f : once) ++tally[f];
    }
    std::vector<FieldCount> out;
    out.reserve(tally.size());
    for (const auto& kv : tally)
        out.push_back(FieldCount{static_cast<Field>(kv.first), kv.second});
    std::sort(out.begin(), out.end(), [](const FieldCount& a, const FieldCount& b) {
        if (a.count != b.count) return a.count > b.count;
        return static_cast<int>(a.field) < static_cast<int>(b.field);
    });
    return out;
}

// ── Transitions ──────────────────────────────────────────────────────────────

Case apply_check(const Case& c, Outcome o, Reason r,
                 const std::string& today, int recheck_days) {
    Case n = c;
    n.outcome = o;
    // A reason belongs to Indeterminate and nothing else. Carrying a stale one
    // forward would let a report explain a clean fetch with last week's block.
    n.reason = (o == Outcome::Indeterminate) ? r : Reason::None;

    if (date_valid(today)) n.last_attempt = today;

    if (o == Outcome::Indeterminate) {
        ++n.consecutive_failures;
        // The absence streak is NOT broken. A check we could not run is not a
        // sighting; absence of evidence is not evidence of presence, and
        // resetting here would mean one blocked fetch erases a clean absence we
        // did observe. It does not advance either -- only a clean look counts.
        // Still scheduled. A check we could not run is not a case we stop
        // watching -- it is the case we most need to come back to.
        if (date_valid(today)) n.next_check = date_add_days(today, recheck_days);
        return n;
    }

    // Listed and NotFound are both CLEAN FETCHES. Either one proves the tunnel,
    // the URL and the parser all worked, so the failure streak resets on both.
    n.consecutive_failures = 0;
    // The other streak moves the other way: a clean absence is evidence toward
    // "gone", and one sighting wipes it out entirely.
    if (o == Outcome::NotFound) ++n.clean_absences;
    else                        n.clean_absences = 0;
    if (date_valid(today)) {
        n.last_verified = today;
        n.next_check    = date_add_days(today, recheck_days);
    }

    // Deliberately NOT touched: status and provenance. One clean NotFound is
    // evidence toward "removed", not the decision itself -- brokers 404 a page
    // and re-serve it under a new slug, and a single fetch cannot tell those
    // apart. Promotion is a caller's judgment, made with the streak in view.
    return n;
}

const char* promotion_name(Promotion p) {
    switch (p) {
        case Promotion::None:     return "none";
        case Promotion::Removed:  return "removed";
        case Promotion::Returned: return "returned";
    }
    return "none";
}

Case apply_filed(const Case& c, const std::string& today) {
    if (c.status == Status::Removed || c.status == Status::Relisted ||
        c.status == Status::Abandoned)
        return c;

    Case out = c;
    out.status = Status::Requested;
    if (out.requested.empty() && date_valid(today)) out.requested = today;
    return out;
}

Promotion promotion_for(const Case& c, const PromotionRule& r) {
    // Terminal. A relisted case has a successor carrying the story now, and an
    // abandoned one is a decision we already made.
    if (c.status == Status::Relisted || c.status == Status::Abandoned)
        return Promotion::None;

    // The record is back. This outranks everything below it: a case we believed
    // gone that fetches Listed is the single event this app was built to catch,
    // and it is true regardless of how many clean absences preceded it.
    if (c.status == Status::Removed && c.outcome == Outcome::Listed)
        return Promotion::Returned;
    if (c.status == Status::Removed) return Promotion::None;

    // The LAST look has to be the clean one. A streak of absences followed by a
    // sighting is a case that came back before we ever called it gone.
    if (c.outcome != Outcome::NotFound) return Promotion::None;

    // SelfVerified without a date fails validation, and a promotion that writes
    // an invalid case is worse than one that waits for the next check.
    if (c.last_verified.empty()) return Promotion::None;

    int needed = r.clean_absences_required;
    if (r.claim_counts_as_one &&
        (c.provenance == Provenance::BrokerClaim || c.provenance == Provenance::PlatformClaim))
        --needed;
    if (needed < 1) needed = 1;   // never zero: something has to have been seen

    return c.clean_absences >= needed ? Promotion::Removed : Promotion::None;
}

Case apply_promotion(const Case& c, const PromotionRule& r) {
    Case n = c;
    if (promotion_for(c, r) != Promotion::Removed) return n;
    n.status = Status::Removed;
    // Not BrokerClaim, whatever was there before: we fetched the live page and
    // it was gone. That is the strongest evidence in the schema and it is ours,
    // and overwriting a claim with it is the entire point of the app.
    n.provenance = Provenance::SelfVerified;
    return n;
}

Case relist_successor(const Case& old, const std::string& new_id,
                      const std::string& today) {
    Case n;
    n.id         = new_id;
    n.broker_id  = old.broker_id;
    n.url        = old.url;
    n.status     = Status::Found;
    n.provenance = Provenance::None;   // the old proof does not transfer
    n.outcome    = Outcome::Listed;    // we are here because we saw it
    n.reason     = Reason::None;
    n.first_seen = today;
    n.last_attempt = today;
    n.last_verified = today;
    n.next_check = old.next_check;
    n.exposes    = old.exposes;        // a starting assumption, re-observable
    n.supersedes = old.id;
    return n;
}

std::string log_ref(const Case& c) {
    return "case:" + c.id + "@" + c.broker_id;
}

// ── Validation ───────────────────────────────────────────────────────────────

std::vector<std::string> caseload_validate(const Caseload& c) {
    std::vector<std::string> problems;
    std::set<std::string> seen;

    for (std::size_t i = 0; i < c.size(); ++i) {
        const auto& k = c[i];
        const std::string at = "case[" + std::to_string(i) + "]";
        // Problems name the INDEX and the id, never the url -- a validation
        // report is a log by another name.
        const std::string ref = at + (k.id.empty() ? "" : " '" + k.id + "'");

        if (k.id.empty())        problems.push_back(at + ": empty id");
        else if (!seen.insert(k.id).second)
            problems.push_back(ref + ": duplicate id");
        if (k.broker_id.empty()) problems.push_back(ref + ": empty broker_id");
        if (k.url.empty())       problems.push_back(ref + ": empty url");

        const struct { const char* n; const std::string& v; bool required; } dates[] = {
            {"first_seen",    k.first_seen,    true},
            {"requested",     k.requested,     false},
            {"last_attempt",  k.last_attempt,  false},
            {"last_verified", k.last_verified, false},
            {"next_check",    k.next_check,    false},
        };
        for (const auto& d : dates) {
            if (d.v.empty()) {
                if (d.required) problems.push_back(ref + ": missing " + d.n);
            } else if (!date_valid(d.v)) {
                problems.push_back(ref + ": malformed " + d.n);
            }
        }

        // Ordering, checked only between pairs that are both present and valid.
        const auto ordered = [&](const char* an, const std::string& a,
                                 const char* bn, const std::string& b) {
            if (a.empty() || b.empty() || !date_valid(a) || !date_valid(b)) return;
            if (date_compare(a, b) > 0)
                problems.push_back(ref + ": " + an + " is after " + bn);
        };
        ordered("first_seen", k.first_seen, "requested",    k.requested);
        ordered("first_seen", k.first_seen, "last_attempt", k.last_attempt);
        ordered("last_verified", k.last_verified, "last_attempt", k.last_attempt);

        // The invariant the whole report rests on.
        if (k.outcome == Outcome::Indeterminate && k.reason == Reason::None)
            problems.push_back(ref + ": indeterminate without a reason");
        if (k.outcome != Outcome::Indeterminate && k.reason != Reason::None)
            problems.push_back(ref + ": reason on a non-indeterminate outcome");

        if (k.status == Status::Removed && k.provenance == Provenance::None)
            problems.push_back(ref + ": removed without a provenance");
        if (k.provenance == Provenance::SelfVerified && k.last_verified.empty())
            problems.push_back(ref + ": self-verified without a last_verified date");
        if (k.status == Status::Requested && k.requested.empty())
            problems.push_back(ref + ": requested without a requested date");

        if (k.consecutive_failures < 0)
            problems.push_back(ref + ": negative consecutive_failures");
        if (k.clean_absences < 0)
            problems.push_back(ref + ": negative clean_absences");
        if (!k.supersedes.empty() && k.supersedes == k.id)
            problems.push_back(ref + ": supersedes itself");
    }

    // A successor must point at a case that exists, or the history is broken.
    for (const auto& k : c) {
        if (k.supersedes.empty()) continue;
        if (!caseload_find(c, k.supersedes))
            problems.push_back("case '" + k.id + "': supersedes an unknown case");
    }
    return problems;
}

// ── Pump ─────────────────────────────────────────────────────────────────────

Caseload caseload_load(const std::string& file, std::string* error) {
    if (error) error->clear();
    std::ifstream in(file);
    if (!in) return {};   // first-run tolerance: absent is empty, not an error

    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        if (error) *error = std::string("parse: ") + e.what();
        return {};
    }
    if (!j.is_object() || !j.contains("cases") || !j["cases"].is_array()) {
        if (error) *error = "no 'cases' array";
        return {};
    }

    Caseload out;
    for (const auto& e : j["cases"]) {
        if (!e.is_object()) continue;
        Case k;
        k.id         = e.value("id", "");
        k.broker_id  = e.value("broker_id", "");
        k.url        = e.value("url", "");
        k.status     = status_from(e.value("status", "found"));
        k.provenance = provenance_from(e.value("provenance", "none"));
        k.outcome    = outcome_from(e.value("outcome", "never"));
        k.reason     = reason_from(e.value("reason", "none"));
        k.first_seen    = e.value("first_seen", "");
        k.requested     = e.value("requested", "");
        k.last_attempt  = e.value("last_attempt", "");
        k.last_verified = e.value("last_verified", "");
        k.next_check    = e.value("next_check", "");
        k.consecutive_failures = e.value("consecutive_failures", 0);
        k.clean_absences       = e.value("clean_absences", 0);
        k.supersedes = e.value("supersedes", "");
        k.note       = e.value("note", "");
        if (e.contains("exposes") && e["exposes"].is_array())
            for (const auto& f : e["exposes"])
                if (f.is_string()) k.exposes.push_back(field_from(f.get<std::string>()));
        out.push_back(std::move(k));
    }
    return out;
}

bool caseload_save(const std::string& file, const Caseload& c) {
    json arr = json::array();
    for (const auto& k : c) {
        json fields = json::array();
        for (auto f : k.exposes) fields.push_back(field_name(f));
        arr.push_back(json{
            {"id",         k.id},
            {"broker_id",  k.broker_id},
            {"url",        k.url},
            {"status",     status_name(k.status)},
            {"provenance", provenance_name(k.provenance)},
            {"outcome",    outcome_name(k.outcome)},
            {"reason",     reason_name(k.reason)},
            {"first_seen",    k.first_seen},
            {"requested",     k.requested},
            {"last_attempt",  k.last_attempt},
            {"last_verified", k.last_verified},
            {"next_check",    k.next_check},
            {"consecutive_failures", k.consecutive_failures},
            {"clean_absences",       k.clean_absences},
            {"exposes",    fields},
            {"supersedes", k.supersedes},
            {"note",       k.note},
        });
    }
    json j;
    j["version"] = 1;
    j["cases"]   = std::move(arr);

    std::ofstream out(file);
    if (!out) return false;
    out << j.dump(2) << "\n";
    return out.good();
}

}  // namespace delr::core
