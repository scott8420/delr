// The core's selftest -- exercises the GTK-free core with no display.
// CANON: an un-run reference is worse than nothing; it looks authoritative and
// lies. Everything in core/ is exercised here or it doesn't ship.
//
// NOT a separate binary. It compiles into delr and runs as `delr --selftest`,
// and build.sh invokes it after every build, so a broken core fails the build
// rather than waiting for someone to remember a second executable exists.
#include "selftest.hpp"
#include "core/Broker.hpp"
#include "core/Case.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace delr::core;

namespace {
int g_pass = 0, g_fail = 0;

void check(bool cond, const std::string& what) {
    if (cond) { ++g_pass; std::printf("  pass  %s\n", what.c_str()); }
    else      { ++g_fail; std::printf("  FAIL  %s\n", what.c_str()); }
}

std::string tmp_path(const char* leaf) {
    const char* dir = std::getenv("TMPDIR");
    return std::string(dir ? dir : "/tmp") + "/" + leaf;
}

Case mkcase(const std::string& id, const std::string& broker) {
    Case k;
    k.id = id;
    k.broker_id = broker;
    k.url = "https://" + broker + ".example/listing/" + id;
    k.first_seen = "2026-01-15";
    k.next_check = "2026-03-01";
    return k;
}

Broker mk(const std::string& id, Method m) {
    Broker b;
    b.id = id;
    b.name = id + " Inc";
    b.site = "https://" + id + ".example";
    b.method = m;
    if (m == Method::Web)   b.opt_out_url = "https://" + id + ".example/optout";
    if (m == Method::Email) b.opt_out_email = "privacy@" + id + ".example";
    return b;
}
}  // namespace

namespace delr::selftest {

int run() {
    std::printf("delr selftest\n\n");

    std::printf("method round-trip\n");
    for (auto m : {Method::Web, Method::Email, Method::Postal,
                   Method::Phone, Method::Drop, Method::Unknown}) {
        check(method_from(method_name(m)) == m,
              std::string("method '") + method_name(m) + "' survives name->from");
    }

    std::printf("\nroster lookup\n");
    Roster r{mk("alpha", Method::Web), mk("beta", Method::Email)};
    check(roster_find(r, "alpha") != nullptr, "find hits a present id");
    check(roster_find(r, "nope")  == nullptr, "find misses an absent id");
    check(roster_find(r, "beta")->method == Method::Email, "find returns the right entry");

    std::printf("\nroster validation\n");
    check(roster_validate(r).empty(), "a clean roster reports no problems");

    Roster dup{mk("alpha", Method::Web), mk("alpha", Method::Web)};
    check(roster_validate(dup).size() == 1, "duplicate id is caught");

    Roster bad_web{mk("gamma", Method::Web)};
    bad_web[0].opt_out_url.clear();
    check(roster_validate(bad_web).size() == 1, "web method with no url is caught");

    Roster bad_mail{mk("delta", Method::Email)};
    bad_mail[0].opt_out_email.clear();
    check(roster_validate(bad_mail).size() == 1, "email method with no address is caught");

    Roster unknown{mk("eps", Method::Unknown)};
    check(!roster_validate(unknown).empty(), "unknown method is caught");

    Roster noid{mk("zeta", Method::Web)};
    noid[0].id.clear();
    check(!roster_validate(noid).empty(), "empty id is caught");

    Roster bad_days{mk("eta", Method::Web)};
    bad_days[0].recheck_days = 0;
    check(!roster_validate(bad_days).empty(), "non-positive recheck_days is caught");

    std::printf("\npersistence pump (round-trip fidelity)\n");
    const std::string f = tmp_path("delr_selftest_roster.json");
    Roster src{mk("spokeo", Method::Web), mk("acxiom", Method::Email),
               mk("statewide", Method::Drop)};
    src[0].requires_id = true;
    src[0].recheck_days = 30;
    src[2].ca_registered = true;
    src[1].notes = "responds slowly";

    check(roster_save(f, src), "save writes the file");

    std::string err;
    Roster back = roster_load(f, &err);
    check(err.empty(), "load reports no error");
    check(back.size() == src.size(), "load returns the same count");

    bool identical = back.size() == src.size();
    for (std::size_t i = 0; identical && i < src.size(); ++i) {
        const auto& a = src[i];
        const auto& b = back[i];
        identical = a.id == b.id && a.name == b.name && a.site == b.site &&
                    a.method == b.method && a.opt_out_url == b.opt_out_url &&
                    a.opt_out_email == b.opt_out_email &&
                    a.requires_id == b.requires_id &&
                    a.recheck_days == b.recheck_days &&
                    a.ca_registered == b.ca_registered && a.notes == b.notes;
    }
    check(identical, "every field survives the round trip");
    check(roster_validate(back).empty(), "the reloaded roster validates");

    std::printf("\nfirst-run tolerance\n");
    std::string err2;
    Roster missing = roster_load(tmp_path("delr_no_such_file.json"), &err2);
    check(missing.empty(), "an absent file yields an empty roster");
    check(err2.empty(), "an absent file is not an error");

    std::remove(f.c_str());

    // ── the caseload ─────────────────────────────────────────────────────────

    std::printf("\ncase enum round-trips\n");
    for (auto v : {Status::Found, Status::Requested, Status::Removed,
                   Status::Relisted, Status::Abandoned, Status::Unknown})
        check(status_from(status_name(v)) == v,
              std::string("status '") + status_name(v) + "' survives name->from");
    for (auto v : {Provenance::None, Provenance::BrokerClaim,
                   Provenance::PlatformClaim, Provenance::SelfVerified})
        check(provenance_from(provenance_name(v)) == v,
              std::string("provenance '") + provenance_name(v) + "' survives name->from");
    for (auto v : {Outcome::Listed, Outcome::NotFound,
                   Outcome::Indeterminate, Outcome::Never})
        check(outcome_from(outcome_name(v)) == v,
              std::string("outcome '") + outcome_name(v) + "' survives name->from");
    for (auto v : {Reason::None, Reason::NoTunnel, Reason::Blocked, Reason::Captcha,
                   Reason::RateLimited, Reason::Timeout, Reason::BadResponse,
                   Reason::UrlDead})
        check(reason_from(reason_name(v)) == v,
              std::string("reason '") + reason_name(v) + "' survives name->from");
    for (auto v : {Field::Name, Field::Aliases, Field::Age, Field::Dob,
                   Field::Address, Field::AddressHistory, Field::Phone,
                   Field::Email, Field::Relatives, Field::Employer, Field::Other})
        check(field_from(field_name(v)) == v,
              std::string("field '") + field_name(v) + "' survives name->from");

    std::printf("\ndate arithmetic\n");
    check(date_valid("2026-02-28"), "a real date validates");
    check(!date_valid("2026-02-30"), "february 30th is rejected");
    check(!date_valid("2025-02-29"), "a non-leap february 29th is rejected");
    check(date_valid("2024-02-29"),  "a leap february 29th is accepted");
    check(!date_valid("2026-13-01"), "month 13 is rejected");
    check(!date_valid("26-01-01"),   "a short form is rejected");
    check(!date_valid(""),           "an empty string is not a date");
    check(date_add_days("2024-02-28", 1) == "2024-02-29", "leap day is counted");
    check(date_add_days("1900-02-28", 1) == "1900-03-01", "1900 is not a leap year");
    check(date_add_days("2000-02-28", 1) == "2000-02-29", "2000 is a leap year");
    check(date_add_days("2026-12-31", 1) == "2027-01-01", "the year rolls over");
    check(date_add_days("2026-01-15", 45) == "2026-03-01", "45 days lands correctly");
    check(date_add_days("2026-03-01", -45) == "2026-01-15", "and reverses exactly");
    check(date_add_days("nonsense", 45).empty(), "a bad date yields no date");
    check(date_compare("2026-01-01", "2026-01-02") < 0, "compare orders dates");

    std::printf("\ncase lookup and due list\n");
    Caseload cl{mkcase("c1", "spokeo"), mkcase("c2", "acxiom"), mkcase("c3", "radaris")};
    cl[1].next_check = "2026-02-01";      // due earlier
    cl[2].next_check = "2026-09-01";      // not yet due
    check(caseload_find(cl, "c2") != nullptr, "find hits a present id");
    check(caseload_find(cl, "nope") == nullptr, "find misses an absent id");

    auto due = caseload_due(cl, "2026-03-01");
    check(due.size() == 2, "only cases at or past their date are due");
    check(due[0]->id == "c2", "the longest overdue sorts first");

    Caseload term{mkcase("t1", "spokeo"), mkcase("t2", "spokeo")};
    term[0].status = Status::Abandoned;
    term[1].status = Status::Relisted;
    check(caseload_due(term, "2026-09-01").empty(),
          "abandoned and relisted cases are never due");

    std::printf("\napply_check\n");
    Case base = mkcase("c9", "spokeo");
    Case blocked = apply_check(base, Outcome::Indeterminate, Reason::Blocked,
                               "2026-03-01", 45);
    check(blocked.outcome == Outcome::Indeterminate, "the outcome is recorded");
    check(blocked.reason == Reason::Blocked, "the reason rides with it");
    check(blocked.consecutive_failures == 1, "a failed check counts");
    check(blocked.last_attempt == "2026-03-01", "the attempt is dated");
    check(blocked.last_verified.empty(), "a blocked check verifies nothing");
    check(blocked.next_check == "2026-04-15", "a blocked case is still scheduled");
    check(blocked.status == base.status, "a check never moves the status");

    Case again = apply_check(blocked, Outcome::Indeterminate, Reason::NoTunnel,
                             "2026-04-15", 45);
    check(again.consecutive_failures == 2, "failures accumulate");
    check(again.reason == Reason::NoTunnel, "the newest reason replaces the old");

    Case clean = apply_check(again, Outcome::NotFound, Reason::None,
                             "2026-05-30", 45);
    check(clean.consecutive_failures == 0, "a clean fetch clears the streak");
    check(clean.reason == Reason::None, "a clean fetch carries no reason");
    check(clean.last_verified == "2026-05-30", "a clean fetch dates the verification");
    check(clean.status != Status::Removed, "not-found does not itself declare removal");

    Case still = apply_check(again, Outcome::Listed, Reason::None, "2026-05-30", 45);
    check(still.consecutive_failures == 0, "listed is also a clean fetch");
    check(still.last_verified == "2026-05-30", "listed verifies too");

    std::printf("\nrelist succession\n");
    Case old = mkcase("c1", "spokeo");
    old.status = Status::Removed;
    old.provenance = Provenance::SelfVerified;
    old.exposes = {Field::Name, Field::Address};
    Case next = relist_successor(old, "c1b", "2026-06-01");
    check(next.supersedes == "c1", "the successor points back");
    check(next.broker_id == old.broker_id && next.url == old.url,
          "broker and url carry over");
    check(next.status == Status::Found, "the successor starts fresh");
    check(next.provenance == Provenance::None, "the old proof does not transfer");
    check(next.exposes.size() == 2, "the observed fields carry over as a start");

    std::printf("\nlog safety\n");
    Case pii = mkcase("c7", "spokeo");
    pii.note = "SECRET";
    const std::string ref = log_ref(pii);
    check(ref.find(pii.url) == std::string::npos, "log_ref never carries the url");
    check(ref.find("SECRET") == std::string::npos, "log_ref never carries the note");
    check(ref.find("c7") != std::string::npos, "log_ref does carry the case id");

    std::printf("\ncase validation\n");
    check(caseload_validate(cl).empty(), "a clean caseload reports no problems");

    Caseload dupc{mkcase("d", "spokeo"), mkcase("d", "acxiom")};
    check(!caseload_validate(dupc).empty(), "duplicate case id is caught");

    Caseload noreason{mkcase("e", "spokeo")};
    noreason[0].outcome = Outcome::Indeterminate;
    check(!caseload_validate(noreason).empty(),
          "indeterminate without a reason is caught");

    Caseload stale{mkcase("f", "spokeo")};
    stale[0].outcome = Outcome::NotFound;
    stale[0].reason  = Reason::Blocked;
    check(!caseload_validate(stale).empty(),
          "a reason on a clean outcome is caught");

    Caseload unproven{mkcase("g", "spokeo")};
    unproven[0].status = Status::Removed;
    check(!caseload_validate(unproven).empty(),
          "removed without a provenance is caught");

    Caseload unverified{mkcase("h", "spokeo")};
    unverified[0].status = Status::Removed;
    unverified[0].provenance = Provenance::SelfVerified;
    check(!caseload_validate(unverified).empty(),
          "self-verified with no verification date is caught");

    Caseload backwards{mkcase("i", "spokeo")};
    backwards[0].first_seen   = "2026-05-01";
    backwards[0].last_attempt = "2026-01-01";
    check(!caseload_validate(backwards).empty(),
          "an attempt before the listing was found is caught");

    Caseload orphan{mkcase("j", "spokeo")};
    orphan[0].supersedes = "ghost";
    check(!caseload_validate(orphan).empty(),
          "superseding an unknown case is caught");

    Caseload badurl{mkcase("k", "spokeo")};
    badurl[0].url.clear();
    check(!caseload_validate(badurl).empty(), "an empty url is caught");

    for (const auto& p : caseload_validate(badurl))
        check(p.find("spokeo.example") == std::string::npos,
              "a validation problem never quotes the url");

    std::printf("\nexposure aggregation\n");
    Caseload exp{mkcase("x1", "spokeo"), mkcase("x2", "acxiom"),
                 mkcase("x3", "radaris")};
    exp[0].exposes = {Field::Name, Field::Phone, Field::Address, Field::Phone};
    exp[1].exposes = {Field::Name, Field::Phone};
    exp[2].exposes = {Field::Name};
    exp[2].status = Status::Removed;
    exp[2].provenance = Provenance::BrokerClaim;

    auto by = exposure_by_field(exp, false);
    check(!by.empty(), "aggregation returns something");
    check(by[0].field == Field::Name && by[0].count == 2,
          "a removed case is excluded from live exposure");
    check(by[1].field == Field::Phone && by[1].count == 2,
          "a field repeated within one case counts once");

    auto by_all = exposure_by_field(exp, true);
    check(by_all[0].count == 3, "including removed raises the count");

    std::printf("\ncaseload pump (round-trip fidelity)\n");
    const std::string cf = tmp_path("delr_selftest_cases.json");
    Caseload csrc{mkcase("p1", "spokeo"), mkcase("p2", "acxiom")};
    csrc[0].status = Status::Removed;
    csrc[0].provenance = Provenance::SelfVerified;
    csrc[0].outcome = Outcome::NotFound;
    csrc[0].last_verified = "2026-02-20";
    csrc[0].last_attempt = "2026-02-20";
    csrc[0].requested = "2026-01-20";
    csrc[0].exposes = {Field::Name, Field::Relatives, Field::Dob};
    csrc[0].note = "form wanted a photo id; used the email channel instead";
    csrc[1].outcome = Outcome::Indeterminate;
    csrc[1].reason = Reason::Captcha;
    csrc[1].consecutive_failures = 3;
    csrc[1].supersedes = "p1";

    check(caseload_save(cf, csrc), "save writes the caseload");
    std::string cerr;
    Caseload cback = caseload_load(cf, &cerr);
    check(cerr.empty(), "load reports no error");
    check(cback.size() == csrc.size(), "load returns the same count");

    bool same = cback.size() == csrc.size();
    for (std::size_t i = 0; same && i < csrc.size(); ++i) {
        const auto& a = csrc[i];
        const auto& b = cback[i];
        same = a.id == b.id && a.broker_id == b.broker_id && a.url == b.url &&
               a.status == b.status && a.provenance == b.provenance &&
               a.outcome == b.outcome && a.reason == b.reason &&
               a.first_seen == b.first_seen && a.requested == b.requested &&
               a.last_attempt == b.last_attempt &&
               a.last_verified == b.last_verified &&
               a.next_check == b.next_check &&
               a.consecutive_failures == b.consecutive_failures &&
               a.exposes == b.exposes && a.supersedes == b.supersedes &&
               a.note == b.note;
    }
    check(same, "every case field survives the round trip");
    check(caseload_validate(cback).empty(), "the reloaded caseload validates");

    std::string cerr2;
    Caseload cmissing = caseload_load(tmp_path("delr_no_cases.json"), &cerr2);
    check(cmissing.empty(), "an absent caseload file yields an empty caseload");
    check(cerr2.empty(), "an absent caseload file is not an error");

    std::remove(cf.c_str());

    std::printf("\n%d pass / %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

}  // namespace delr::selftest
