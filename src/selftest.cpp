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
#include "core/Egress.hpp"
#include "core/Intake.hpp"
#include "core/Journal.hpp"
#include "core/PageRules.hpp"
#include "core/Probe.hpp"
#include "core/Profile.hpp"
#include "core/RosterImport.hpp"
#include "net/Fetch.hpp"
#include "net/Observer.hpp"
#include "Paths.hpp"

#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <fstream>
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
// A policy and an observation that agree: the tunnel is up, bound, at a known
// exit, with lookups proxied. Every egress test below is this pair with exactly
// one thing broken.
EgressPolicy mkpolicy() {
    EgressPolicy p;
    p.interface_name = "wg0";
    p.accepted_exits = {"203.0.113.9"};
    p.naked_exit     = "198.51.100.7";
    p.dns            = DnsMode::Proxied;
    p.proxy          = "socks5h://127.0.0.1:1080";
    p.preflight_ttl_s = 300;
    return p;
}

EgressObservation mkobs() {
    EgressObservation o;
    o.interface_present = true;
    o.interface_up      = true;
    o.interface_address = "10.7.0.2";
    o.bound             = true;
    o.bound_address     = "10.7.0.2";
    o.observed_exit     = "203.0.113.9";
    o.canary            = Canary::Clean;
    o.observed_at_s     = 1000;
    return o;
}
const std::int64_t kNow = 1100;   // 100s after the observation; ttl is 300
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
    // The host list and the two registry flags ride the same pump and must not
    // be able to skew from it -- a field added to the struct and forgotten in
    // encode is a field that silently empties on every save.
    src[0].hosts = {"spokeo.example", "thatsthem.example"};
    src[1].fcra_regulated = true;
    src[2].collects_geo = true;

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
                    a.ca_registered == b.ca_registered && a.notes == b.notes &&
                    a.hosts == b.hosts && a.fcra_regulated == b.fcra_regulated &&
                    a.collects_geo == b.collects_geo;
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
                   Reason::UrlDead, Reason::NoRule, Reason::PageUnreadable})
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

    // ── Intake: the paste-a-URL path ─────────────────────────────────────────
    // Everything between a pasted URL and a row in the caseload is decided
    // here, headless. The dialog only shows what these functions concluded, so
    // this is where the feature is actually tested.
    std::printf("\nurl checking\n");
    check(url_check("") == UrlProblem::Empty, "an empty paste is Empty, not an error");
    check(url_check("   \n") == UrlProblem::Empty, "whitespace only is Empty");
    check(url_check("https://spokeo.example/John-Smith") == UrlProblem::None,
          "a full https URL passes");
    check(url_check("spokeo.example/John-Smith") == UrlProblem::None,
          "a scheme-less paste passes -- people type the host, not the scheme");
    check(url_check("http://spokeo.example/x") == UrlProblem::None, "http passes");
    check(url_check("  https://spokeo.example/x  ") == UrlProblem::None,
          "surrounding whitespace is trimmed, not refused");
    check(url_check("https://spokeo.example/a b") == UrlProblem::Whitespace,
          "an embedded space is Whitespace -- two URLs pasted at once");
    check(url_check("file:///etc/passwd") == UrlProblem::BadScheme, "file: is refused");
    check(url_check("javascript:alert(1)") == UrlProblem::BadScheme,
          "javascript: is refused");
    check(url_check("mailto:privacy@spokeo.example") == UrlProblem::BadScheme,
          "mailto: is refused");
    check(url_check("https://") == UrlProblem::NoHost, "a scheme with no host is NoHost");
    check(url_check("https:spokeo.example/x") == UrlProblem::NoHost,
          "a scheme without // is a typo, not a guess");
    check(url_check("https://localhost/x") == UrlProblem::BadHost,
          "a dotless host is refused -- a listing lives on the public web");
    check(url_check("https://spokeo..example/x") == UrlProblem::BadHost,
          "a doubled dot is refused");
    check(url_check("https://spokeo.example:80x/x") == UrlProblem::BadHost,
          "a non-numeric port is refused");
    check(url_check("https://user@spokeo.example/x") == UrlProblem::HasUserinfo,
          "userinfo is refused rather than silently stripped");
    for (auto p : {UrlProblem::None, UrlProblem::Empty, UrlProblem::Whitespace,
                   UrlProblem::BadScheme, UrlProblem::NoHost, UrlProblem::BadHost,
                   UrlProblem::HasUserinfo})
        check(std::string(url_problem_name(p)) != "", "every url problem has a name");
    check(std::string(url_problem_text(UrlProblem::None)).empty(),
          "a clean URL has no complaint to show");
    check(std::string(url_problem_text(UrlProblem::BadScheme)).find("http") !=
          std::string::npos, "the bad-scheme message says what IS accepted");

    std::printf("\nurl normalising\n");
    check(url_normalize("spokeo.example/John-Smith") == "https://spokeo.example/John-Smith",
          "a scheme-less paste normalises to https");
    check(url_normalize("http://spokeo.example/x") == "http://spokeo.example/x",
          "an explicit http is never upgraded away -- we do not guess at where a request goes");
    check(url_normalize("HTTPS://Spokeo.Example/John-Smith") ==
          "https://spokeo.example/John-Smith",
          "scheme and host lower-case, path does not");
    check(url_normalize("https://spokeo.example/John-Smith#relatives") ==
          "https://spokeo.example/John-Smith",
          "the fragment is dropped -- it never reaches the server");
    check(url_normalize("https://spokeo.example/") == "https://spokeo.example",
          "a bare trailing slash is dropped");
    check(url_normalize("https://spokeo.example/p/?id=7&x=Y") ==
          "https://spokeo.example/p/?id=7&x=Y",
          "the query survives byte-for-byte -- broker ids live in it");
    check(url_normalize("https://spokeo.example:8443/x") == "https://spokeo.example:8443/x",
          "a non-default port is part of where the request goes");
    check(url_normalize("https://www.spokeo.example/x") == "https://www.spokeo.example/x",
          "www. is kept in the stored URL -- the stored URL has to fetch");
    check(url_normalize("file:///etc/passwd").empty(),
          "a URL that fails checking normalises to empty, never to a guess");

    std::printf("\nurl host\n");
    check(url_host("https://www.Spokeo.example/John-Smith") == "spokeo.example",
          "the display host drops www. and lower-cases");
    check(url_host("https://spokeo.example:8443/x") == "spokeo.example",
          "the display host drops the port");
    check(url_host("spokeo.example/x") == "spokeo.example",
          "the display host works on a scheme-less paste");
    check(url_host("").empty(), "no host in an empty string");
    check(url_host("https://spokeo.example/John-Smith/Tennessee").find('/') ==
          std::string::npos,
          "the host NEVER carries the path -- the path is the part with the name in it");

    std::printf("\nbroker matching\n");
    {
        Roster r;
        r.push_back(mk("spokeo", Method::Web));
        r.push_back(mk("radaris", Method::Email));
        Broker sub;                       // a sub-brand under a longer host
        sub.id = "spokeo-uk"; sub.name = "Spokeo UK";
        sub.site = "https://uk.spokeo.example"; sub.method = Method::Web;
        r.push_back(sub);
        Broker owner;                     // matched only by its opt-out URL
        owner.id = "peoplefind"; owner.name = "PeopleFind";
        owner.site = "https://corp.example"; owner.method = Method::Web;
        owner.opt_out_url = "https://peoplefind.example/optout";
        r.push_back(owner);

        const Broker* b = broker_for_url(r, "https://spokeo.example/John-Smith");
        check(b && b->id == "spokeo", "an exact host match finds the broker");
        b = broker_for_url(r, "https://www.spokeo.example/John-Smith");
        check(b && b->id == "spokeo", "www. matches the bare host");
        b = broker_for_url(r, "https://search.spokeo.example/x");
        check(b && b->id == "spokeo", "an unknown subdomain matches the parent");
        b = broker_for_url(r, "https://uk.spokeo.example/x");
        check(b && b->id == "spokeo-uk",
              "an exact match on a sub-brand beats a subdomain match on its parent");
        b = broker_for_url(r, "https://peoplefind.example/listing/7");
        check(b && b->id == "peoplefind", "the opt-out URL's host matches too");
        check(broker_for_url(r, "https://notspokeo.example/x") == nullptr,
              "a host that merely ENDS with a broker host does not match");
        check(broker_for_url(r, "https://unknown.example/x") == nullptr,
              "an unknown host is nullptr -- a normal answer, not an error");
        check(broker_for_url(r, "not a url") == nullptr, "a bad URL matches nothing");
    }

    std::printf("\ncase ids\n");
    {
        Caseload c;
        check(next_case_id(c, "spokeo", "2026-08-17") == "spokeo-20260817",
              "the first id of the day is broker + date");
        Case a = mkcase("spokeo-20260817", "spokeo");
        c.push_back(a);
        check(next_case_id(c, "spokeo", "2026-08-17") == "spokeo-20260817-2",
              "a taken id bumps a counter");
        Case b2 = mkcase("spokeo-20260817-2", "spokeo");
        c.push_back(b2);
        check(next_case_id(c, "spokeo", "2026-08-17") == "spokeo-20260817-3",
              "and keeps bumping");
        check(next_case_id(c, "Weird Broker!", "2026-08-17") == "weird-broker-20260817",
              "a broker id with junk in it is slugged, never passed through");
        check(next_case_id(c, "spokeo", "2026-08-17").find("John") == std::string::npos,
              "an id is minted from the broker and the date -- never from the URL");
    }

    std::printf("\nintake\n");
    {
        Roster r;
        r.push_back(mk("spokeo", Method::Web));
        Caseload c;

        IntakeReport rep = intake_inspect(r, c, "spokeo.example/John-Smith");
        check(rep.problem == UrlProblem::None, "a good paste has no problem");
        check(rep.ready(), "a good paste on a known host is ready to commit");
        check(rep.broker && rep.broker->id == "spokeo", "the report names the broker");
        check(rep.host == "spokeo.example", "the report carries the display host");
        check(rep.normalized == "https://spokeo.example/John-Smith",
              "the report carries the URL as it would be stored");
        check(!rep.duplicate() && !rep.relist, "nothing to collide with yet");

        IntakeReport unknown = intake_inspect(r, c, "https://elsewhere.example/x");
        check(!unknown.ready(), "an unmatched host is not ready -- the user must name the broker");
        check(unknown.problem == UrlProblem::None,
              "an unmatched host is not a URL problem; the URL is fine");

        Case k = intake_new_case(rep, c, "2026-08-17", {Field::Name, Field::Phone}, "found via search");
        check(k.id == "spokeo-20260817", "the new case carries a minted id");
        check(k.broker_id == "spokeo", "the new case is filed under the matched broker");
        check(k.url == rep.normalized, "the new case stores the normalised URL");
        check(k.status == Status::Found, "a pasted listing starts at Found");
        check(k.outcome == Outcome::Never,
              "outcome stays Never -- the user looked, WE have not fetched");
        check(k.provenance == Provenance::None, "nobody has claimed anything yet");
        check(k.first_seen == "2026-08-17", "first_seen is the day the user found it");
        check(k.next_check == "2026-08-17", "never verified, so it is due now");
        check(k.exposes.size() == 2, "the observed fields ride along");
        check(k.consecutive_failures == 0, "a fresh case has no failures");

        check(caseload_commit(c, k), "committing a good case succeeds");
        check(c.size() == 1, "the caseload grew by one");
        check(caseload_validate(c).empty(), "a case built by intake validates");
        check(log_ref(k).find("John") == std::string::npos,
              "the new case's log_ref carries no URL");

        check(!caseload_commit(c, k), "committing the same id twice is refused");
        check(c.size() == 1, "a refused commit changes nothing");

        // The duplicate paths.
        IntakeReport dup = intake_inspect(r, c, "https://spokeo.example/John-Smith");
        check(dup.duplicate(), "pasting a tracked listing again is a duplicate");
        check(!dup.ready(), "a duplicate is not ready to commit");
        check(dup.existing && dup.existing->id == "spokeo-20260817",
              "the report names the case already holding it");
        IntakeReport dup2 = intake_inspect(r, c, "http://WWW.Spokeo.example/John-Smith/");
        check(dup2.duplicate(),
              "the same listing pasted with www, http and a trailing slash is still a duplicate");
        IntakeReport other = intake_inspect(r, c, "https://spokeo.example/John-Smith-2");
        check(!other.duplicate(),
              "a different listing on the same broker is a different case -- a case is one LISTING");
        check(other.ready(), "and it is ready to commit");

        // The relist path: the record we believed was gone is back.
        c[0].status        = Status::Removed;
        c[0].provenance    = Provenance::SelfVerified;
        c[0].last_verified = "2026-08-17";
        c[0].last_attempt  = "2026-08-17";
        c[0].outcome       = Outcome::NotFound;
        check(caseload_validate(c).empty(), "the removed case is well-formed to start from");

        IntakeReport re = intake_inspect(r, c, "https://spokeo.example/John-Smith");
        check(re.relist, "pasting a REMOVED listing again is a relist, not a duplicate");
        check(!re.duplicate(), "and a relist is not blocked as a duplicate");
        check(re.ready(), "a relist is ready to commit");

        Case succ = intake_relist_case(re, c, "2026-08-17");
        check(succ.supersedes == "spokeo-20260817", "the successor names its predecessor");
        check(succ.id != succ.supersedes, "the successor is a NEW case, not an edit");
        check(succ.status == Status::Found, "the successor starts at Found");
        check(succ.provenance == Provenance::None,
              "the old proof does not transfer -- it was disproved by the relist");
        check(succ.outcome == Outcome::Listed, "we are here because the record was seen again");
        check(succ.next_check == "2026-08-17", "the successor comes due now, not on the old schedule");
        check(succ.exposes == c[0].exposes, "the old exposure list is a starting assumption");

        check(caseload_commit(c, succ), "committing the successor succeeds");
        check(c.size() == 2, "the caseload grew by one");
        check(c[0].status == Status::Relisted,
              "the predecessor is ended at Relisted in the SAME call -- both facts, both kept");
        check(caseload_validate(c).empty(), "the caseload validates after a relist");

        IntakeReport after = intake_inspect(r, c, "https://spokeo.example/John-Smith");
        check(after.existing && after.existing->id == succ.id,
              "the URL now resolves to the successor, not the ended case");

        // A commit that names a predecessor we don't have must change nothing.
        Case orphan = mkcase("orphan-1", "spokeo");
        orphan.supersedes = "no-such-case";
        const std::size_t before = c.size();
        check(!caseload_commit(c, orphan), "a successor to an unknown case is refused");
        check(c.size() == before, "and the caseload is untouched");

        Case nameless = mkcase("", "spokeo");
        check(!caseload_commit(c, nameless), "a case with no id is refused");
    }

    std::printf("\nintake survives the pump\n");
    {
        Roster r;
        r.push_back(mk("spokeo", Method::Web));
        Caseload c;
        IntakeReport rep = intake_inspect(r, c, "spokeo.example/p/?id=7&x=Y#frag");
        Case k = intake_new_case(rep, c, "2026-08-17", {Field::Phone}, "a note");
        check(caseload_commit(c, k), "committed");

        const std::string f = tmp_path("delr_intake_cases.json");
        check(caseload_save(f, c), "an intake-built caseload saves");
        std::string err;
        Caseload back = caseload_load(f, &err);
        check(err.empty() && back.size() == 1, "and loads back");
        check(back[0].url == k.url, "the URL survives the round trip, query and all");
        check(back[0].next_check == k.next_check, "the schedule survives");
        check(back[0].exposes == k.exposes, "the exposure list survives");
        check(caseload_validate(back).empty(), "and the reloaded caseload validates");
        std::remove(f.c_str());
    }



    std::printf("\nclean absences accumulate\n");
    {
        Case k = mkcase("spokeo-20260115", "spokeo");
        k = apply_check(k, Outcome::NotFound, Reason::None, "2026-03-01", 45);
        check(k.clean_absences == 1, "a clean absence counts");
        k = apply_check(k, Outcome::Indeterminate, Reason::Blocked, "2026-04-15", 3);
        check(k.clean_absences == 1, "a check we could not run neither advances nor breaks the streak");
        check(k.consecutive_failures == 1, "but it is a failure");
        k = apply_check(k, Outcome::NotFound, Reason::None, "2026-04-18", 45);
        check(k.clean_absences == 2, "the next clean absence continues the streak");
        check(k.consecutive_failures == 0, "and clears the failures");
        k = apply_check(k, Outcome::Listed, Reason::None, "2026-06-02", 45);
        check(k.clean_absences == 0, "one sighting wipes the absence streak out");
    }

    std::printf("\nwhen a record is believed gone\n");
    {
        Case k = mkcase("spokeo-20260115", "spokeo");
        check(promotion_for(k) == Promotion::None, "a fresh case is not going anywhere");

        k = apply_check(k, Outcome::NotFound, Reason::None, "2026-03-01", 45);
        check(promotion_for(k) == Promotion::None, "one clean absence is an event, not a pattern");
        k = apply_check(k, Outcome::NotFound, Reason::None, "2026-04-15", 45);
        check(promotion_for(k) == Promotion::Removed, "two is a pattern");

        const Case done = apply_promotion(k);
        check(done.status == Status::Removed, "the promotion moves the status");
        check(done.provenance == Provenance::SelfVerified, "and records that WE are the ones who looked");
        check(done.outcome == Outcome::NotFound, "what we saw is untouched by what we concluded");
        Caseload one{done};
        check(caseload_validate(one).empty(), "and a promoted case validates");

        // A claim lowers the bar by one, and is overwritten when we agree.
        Case claimed = mkcase("acme-20260115", "acme");
        claimed.provenance = Provenance::BrokerClaim;
        claimed = apply_check(claimed, Outcome::NotFound, Reason::None, "2026-03-01", 45);
        check(promotion_for(claimed) == Promotion::Removed,
              "a broker's claim plus our own clean fetch is two sources");
        check(apply_promotion(claimed).provenance == Provenance::SelfVerified,
              "and our fetch outranks the claim it agrees with");

        PromotionRule strict; strict.claim_counts_as_one = false;
        check(promotion_for(claimed, strict) == Promotion::None, "unless the rule refuses to count claims");
        PromotionRule three; three.clean_absences_required = 3;
        check(promotion_for(k, three) == Promotion::None, "a stricter rule needs a longer streak");
        PromotionRule zero; zero.clean_absences_required = 0;
        check(promotion_for(mkcase("x", "y"), zero) == Promotion::None,
              "and no rule promotes a case nothing was ever seen on");

        // The last look has to be the clean one.
        Case back = apply_check(k, Outcome::Listed, Reason::None, "2026-06-02", 45);
        check(promotion_for(back) == Promotion::None, "a sighting after the streak stops the promotion");

        // A blocked fetch is not an absence.
        Case blocked = mkcase("acme-20260201", "acme");
        blocked = apply_check(blocked, Outcome::Indeterminate, Reason::UrlDead, "2026-03-01", 3);
        blocked = apply_check(blocked, Outcome::Indeterminate, Reason::UrlDead, "2026-03-04", 3);
        check(promotion_for(blocked) == Promotion::None,
              "two dead URLs are not two clean absences -- a retired slug is not a removal");
    }

    std::printf("\nwhen the record comes back\n");
    {
        Case k = mkcase("spokeo-20260115", "spokeo");
        k = apply_check(k, Outcome::NotFound, Reason::None, "2026-03-01", 45);
        k = apply_check(k, Outcome::NotFound, Reason::None, "2026-04-15", 45);
        k = apply_promotion(k);
        check(promotion_for(k) == Promotion::None, "a removed case that stays gone stays put");

        Case seen = apply_check(k, Outcome::Listed, Reason::None, "2026-06-01", 45);
        check(promotion_for(seen) == Promotion::Returned, "a removed case fetched Listed has returned");

        Case terminal = seen; terminal.status = Status::Abandoned;
        check(promotion_for(terminal) == Promotion::None, "an abandoned case is a decision already made");
        terminal.status = Status::Relisted;
        check(promotion_for(terminal) == Promotion::None, "and a relisted one has a successor telling the story");
    }

    std::printf("\nthe return, recorded\n");
    {
        Case k = mkcase("spokeo-20260115", "spokeo");
        k = apply_check(k, Outcome::NotFound, Reason::None, "2026-03-01", 45);
        k = apply_check(k, Outcome::NotFound, Reason::None, "2026-04-15", 45);
        k = apply_promotion(k);
        k = apply_check(k, Outcome::Listed, Reason::None, "2026-06-01", 45);

        Caseload c{k};
        std::string fresh_id;
        check(!caseload_record_return(c, "not-a-case", "2026-06-01", 45, &fresh_id),
              "an unknown id is refused");
        check(c.size() == 1, "and nothing was written");

        check(caseload_record_return(c, k.id, "2026-06-01", 45, &fresh_id), "the return is recorded");
        check(c.size() == 2, "as a second case, not an edit of the first");
        check(c[0].status == Status::Relisted, "the predecessor ends at relisted");
        check(c[1].supersedes == k.id, "and the successor names it");
        check(c[1].id == fresh_id && !fresh_id.empty(), "the new id comes back to the caller");
        check(c[1].status == Status::Found, "the successor starts over");
        check(c[1].provenance == Provenance::None, "the old proof does not transfer");
        check(c[1].outcome == Outcome::Listed, "our own fetch saw it there");
        check(c[1].last_verified == "2026-06-01", "and that IS a clean verification, unlike a pasted sighting");
        check(c[1].clean_absences == 0, "the absence streak does not survive the return");
        check(c[1].next_check == "2026-07-16", "the successor goes back on the normal rhythm");
        check(c[1].exposes == k.exposes, "the exposure list carries forward as a starting assumption");
        check(caseload_validate(c).empty(), "and the caseload still validates");

        check(!caseload_record_return(c, c[1].id, "2026-06-01", 45), "a live case has not returned from anything");

        const std::string f = tmp_path("delr_return_cases.json");
        check(caseload_save(f, c), "the relist pair saves");
        std::string err;
        Caseload load = caseload_load(f, &err);
        check(err.empty() && load.size() == 2, "and loads back");
        check(load[0].status == Status::Relisted && load[1].supersedes == load[0].id,
              "with the history intact");
        std::remove(f.c_str());
    }

    std::printf("\nabsence streak survives the pump\n");
    {
        Caseload c{mkcase("spokeo-20260115", "spokeo")};
        c[0].clean_absences = 3;
        const std::string f = tmp_path("delr_absence_cases.json");
        check(caseload_save(f, c), "saved");
        std::string err;
        Caseload back = caseload_load(f, &err);
        check(err.empty() && back.size() == 1 && back[0].clean_absences == 3,
              "the absence streak round-trips");
        c[0].clean_absences = -1;
        check(caseload_validate(c).size() == 1, "a negative streak is caught");
        std::remove(f.c_str());
    }

    std::printf("\naddress kinds\n");
    check(addr_kind("203.0.113.9")     == AddrKind::Public,    "a routable v4 is public");
    check(addr_kind("10.7.0.2")        == AddrKind::Private,   "10/8 is private");
    check(addr_kind("172.16.0.1")      == AddrKind::Private,   "172.16/12 is private");
    check(addr_kind("172.32.0.1")      == AddrKind::Public,    "172.32 is outside 172.16/12");
    check(addr_kind("192.168.1.1")     == AddrKind::Private,   "192.168/16 is private");
    check(addr_kind("100.64.0.1")      == AddrKind::Private,   "CGNAT is not a public identity");
    check(addr_kind("127.0.0.1")       == AddrKind::Loopback,  "127/8 is loopback");
    check(addr_kind("169.254.1.1")     == AddrKind::LinkLocal, "169.254/16 is link-local");
    check(addr_kind("0.0.0.0")         == AddrKind::Invalid,   "0.0.0.0 is not a host");
    check(addr_kind("239.1.1.1")       == AddrKind::Invalid,   "multicast is not a unicast host");
    check(addr_kind("010.1.1.1")       == AddrKind::Invalid,   "a leading zero is refused, not guessed");
    check(addr_kind("1.2.3")           == AddrKind::Invalid,   "three octets is not an address");
    check(addr_kind("1.2.3.256")       == AddrKind::Invalid,   "an octet over 255 is refused");
    check(addr_kind("1.2.3.4.5")       == AddrKind::Invalid,   "five octets is refused");
    check(addr_kind("")                == AddrKind::Invalid,   "empty is invalid");
    check(addr_kind("  203.0.113.9  ") == AddrKind::Public,    "surrounding whitespace survives");

    check(addr_kind("2001:db8::1")  == AddrKind::Public,    "a routable v6 is public");
    check(addr_kind("::1")          == AddrKind::Loopback,  "::1 is loopback");
    check(addr_kind("fd00::1")      == AddrKind::Private,   "fc00::/7 is private");
    check(addr_kind("fe80::1")      == AddrKind::LinkLocal, "fe80::/10 is link-local");
    check(addr_kind("ff02::1")      == AddrKind::Invalid,   "v6 multicast is not a host");
    check(addr_kind("::")           == AddrKind::Invalid,   ":: is not a host");
    check(addr_kind("gggg::1")      == AddrKind::Invalid,   "non-hex is refused");
    check(addr_kind("1:2:3:4:5:6:7:8:9") == AddrKind::Invalid, "nine groups is refused");
    check(addr_kind("1:2:3:4:5:6:7")     == AddrKind::Invalid, "seven groups without :: is refused");
    check(addr_kind("2001::db8::1")      == AddrKind::Invalid, "two :: is ambiguous, so refused");
    check(addr_kind("2001:db8::1:")      == AddrKind::Invalid, "a trailing colon is refused");
    check(addr_kind("::ffff:203.0.113.9") == AddrKind::Public, "a v4-mapped address takes the v4 kind");
    check(addr_kind("::ffff:10.7.0.2")    == AddrKind::Private, "and a private one stays private");

    std::printf("\naddress comparison\n");
    check(addr_canonical("2001:db8::1") == addr_canonical("2001:0db8:0000:0000:0000:0000:0000:0001"),
          "short and long v6 forms canonicalise the same");
    check(addr_same("2001:DB8::1", "2001:db8:0:0:0:0:0:1"), "case and zero-compression do not matter");
    check(addr_same("::ffff:203.0.113.9", "203.0.113.9"), "a v4-mapped address is that v4 address");
    check(addr_same("203.0.113.9", "203.0.113.9"), "an address equals itself");
    check(!addr_same("203.0.113.9", "203.0.113.10"), "a different address is different");
    check(!addr_same("garbage", "garbage"), "two unreadable strings are not a match");
    check(!addr_same("", ""), "and neither are two empty ones");
    check(addr_canonical("nonsense").empty(), "an unreadable address has no comparison form");

    std::printf("\ndns mode round-trip\n");
    for (auto m : {DnsMode::Unset, DnsMode::System, DnsMode::SystemVerified,
                   DnsMode::Pinned, DnsMode::Proxied})
        check(dns_mode_from(dns_mode_name(m)) == m,
              std::string("dns mode '") + dns_mode_name(m) + "' survives name->from");
    check(dns_mode_from("whatever") == DnsMode::Unset, "an unrecognised mode reads as unset, not as system");
    // The one that matters most in the file: a hand-edited "system" must not
    // find its way into the permissive mode by any generosity of the reader.
    check(dns_mode_from("system") == DnsMode::System,
          "a hand-edited 'system' stays the refused value and does not upgrade itself");
    check(dns_mode_from("system-verified") != DnsMode::System,
          "and the checked mode is a different value, not a spelling of it");
    check(dns_mode_from("systemverified") == DnsMode::Unset &&
              dns_mode_from("system verified") == DnsMode::Unset &&
              dns_mode_from("system_verified") == DnsMode::Unset,
          "no near-spelling of it resolves -- unknown means unset means refused");
    check(dns_mode_from("  SYSTEM-VERIFIED \n") == DnsMode::SystemVerified,
          "case and whitespace are still forgiven, as everywhere else here");

    std::printf("\negress policy validation\n");
    check(egress_policy_validate(mkpolicy()).empty(), "a workable policy reports no problems");
    {
        EgressPolicy p = mkpolicy(); p.interface_name.clear();
        check(egress_policy_validate(p).size() == 1, "no interface is caught");

        p = mkpolicy(); p.dns = DnsMode::System;
        check(!egress_policy_validate(p).empty(), "the system resolver is a policy error, not an option");

        p = mkpolicy(); p.dns = DnsMode::Pinned;
        check(!egress_policy_validate(p).empty(), "pinned with no resolver is caught");
        p.resolver = "10.7.0.1";
        check(egress_policy_validate(p).empty(), "pinned with a resolver is clean");

        p = mkpolicy(); p.accepted_exits.push_back(p.naked_exit);
        check(!egress_policy_validate(p).empty(), "our own address on the accepted list is caught");

        p = mkpolicy(); p.accepted_exits = {"192.168.1.5"};
        check(!egress_policy_validate(p).empty(), "a private accepted exit is caught");

        p = mkpolicy(); p.accepted_exits.clear(); p.naked_exit.clear();
        check(!egress_policy_validate(p).empty(), "nothing to compare against is caught");

        p = mkpolicy(); p.preflight_ttl_s = 0;
        check(!egress_policy_validate(p).empty(), "a zero preflight lifetime is caught");

        // ── s9 ───────────────────────────────────────────────────────────────
        p = mkpolicy(); p.dns = DnsMode::SystemVerified;
        check(!egress_policy_validate(p).empty(),
              "system-verified with no recorded no-tunnel resolver is caught -- "
              "without it the mode is 'system' with a better name");
        p.naked_resolvers = {"198.51.100.60"};
        check(egress_policy_validate(p).empty(),
              "and with one recorded it is a clean policy");

        p = mkpolicy(); p.naked_resolvers = {"192.168.1.1"};
        check(!egress_policy_validate(p).empty(),
              "a private recorded resolver is caught -- a resolver on the local "
              "network is this computer's own by another name");
        check(!naked_resolver_known(p),
              "and it does not count as a baseline");
        p.naked_resolvers.push_back("198.51.100.60");
        check(naked_resolver_known(p),
              "one usable entry beside a bad one is still a baseline");
        check(!egress_policy_validate(p).empty(),
              "and the bad entry is still reported, per entry, so it can be removed");

        p = mkpolicy(); p.echo_url.clear();
        check(!egress_policy_validate(p).empty(), "an empty address-check endpoint is caught");
        p = mkpolicy(); p.echo_url = "javascript:alert(1)";
        check(!egress_policy_validate(p).empty(), "and one that is not a url we would touch");
        p = mkpolicy(); p.canary_url.clear();
        check(!egress_policy_validate(p).empty(), "an empty lookup-check endpoint is caught");
        p = mkpolicy(); p.canary_url = "file:///etc/passwd";
        check(!egress_policy_validate(p).empty(), "and so is a scheme we would not fetch");
    }

    std::printf("\negress: the clear case\n");
    check(egress_check(mkpolicy(), mkobs(), kNow) == Verdict::Pass, "a good policy and a good observation pass");
    check(egress_may_fetch(mkpolicy(), mkobs(), kNow), "and the fetch is allowed");
    check(verdict_clear(Verdict::Pass), "only Pass is clear");
    check(!verdict_clear(Verdict::Stale), "stale is not clear");

    std::printf("\negress fails closed\n");
    check(egress_check(EgressPolicy{}, EgressObservation{}, kNow) == Verdict::Unconfigured,
          "an empty policy refuses");
    check(egress_check(mkpolicy(), EgressObservation{}, kNow) == Verdict::NoInterface,
          "an empty observation refuses");
    check(!egress_may_fetch(EgressPolicy{}, EgressObservation{}, 0), "and nothing may leave");
    {
        // A policy error outranks a perfect observation: no preflight can clear it.
        EgressPolicy p = mkpolicy(); p.dns = DnsMode::Unset;
        check(egress_check(p, mkobs(), kNow) == Verdict::DnsUnset, "an unset resolver refuses a perfect tunnel");
        p.dns = DnsMode::System;
        check(egress_check(p, mkobs(), kNow) == Verdict::DnsSystem, "and so does the system resolver");
        p = mkpolicy(); p.dns = DnsMode::Pinned;
        check(egress_check(p, mkobs(), kNow) == Verdict::ResolverMissing, "pinned with no resolver refuses");
        p = mkpolicy(); p.interface_name = "   ";
        check(egress_check(p, mkobs(), kNow) == Verdict::Unconfigured, "a whitespace interface name is no name");
        p = mkpolicy(); p.accepted_exits.clear(); p.naked_exit.clear();
        check(egress_check(p, mkobs(), kNow) == Verdict::ExitUnpinned,
              "with nothing to compare against, the preflight cannot answer and we refuse");

        // ── s9: the mode that is its own evidence ────────────────────────────
        p = mkpolicy(); p.dns = DnsMode::SystemVerified; p.proxy.clear();
        check(egress_check(p, mkobs(), kNow) == Verdict::ResolverBaselineMissing,
              "system-verified refuses a PERFECT tunnel while the resolver "
              "baseline is missing -- the canary is the whole of this mode's "
              "guarantee and it cannot recognise a leak it has never seen");
        p.naked_resolvers = {"192.168.1.1"};
        check(egress_check(p, mkobs(), kNow) == Verdict::ResolverBaselineMissing,
              "and an unusable baseline is not a baseline");
        p.naked_resolvers = {"198.51.100.60"};
        check(egress_check(p, mkobs(), kNow) == Verdict::Pass,
              "with one recorded, the mode passes on a tunnel that passes");
        // The proxy is not required here and its absence must not be borrowed
        // from the mode next door.
        check(p.proxy.empty(), "and it needed no proxy to do it");

        p = mkpolicy(); p.canary_url.clear();
        check(egress_check(p, mkobs(), kNow) == Verdict::CanaryDisabled,
              "an emptied lookup-check endpoint refuses in every mode, and says "
              "which box rather than blaming the preflight");
        p = mkpolicy(); p.echo_url.clear();
        check(egress_check(p, mkobs(), kNow) == Verdict::Pass,
              "the echo endpoint gets no matching policy verdict on purpose -- "
              "an empty one lands as ExitUnobserved when the preflight runs, "
              "and the echo is never the guarantee for any mode");
    }

    std::printf("\negress: can anything leave off-tunnel\n");
    {
        EgressObservation o = mkobs(); o.interface_present = false;
        check(egress_check(mkpolicy(), o, kNow) == Verdict::NoInterface, "a missing interface refuses");
        o = mkobs(); o.interface_up = false;
        check(egress_check(mkpolicy(), o, kNow) == Verdict::InterfaceDown, "a down interface refuses");
        o = mkobs(); o.bound = false;
        check(egress_check(mkpolicy(), o, kNow) == Verdict::NotBound,
              "an unbound socket refuses even with a healthy tunnel");
        o = mkobs(); o.bound_address = "192.168.1.20";
        check(egress_check(mkpolicy(), o, kNow) == Verdict::BindMismatch, "bound to the wrong address refuses");
        o = mkobs(); o.bound_address.clear();
        check(egress_check(mkpolicy(), o, kNow) == Verdict::BindMismatch, "bound to nothing in particular refuses");
        o = mkobs(); o.v6_default_offtunnel = true;
        check(egress_check(mkpolicy(), o, kNow) == Verdict::V6OffTunnel,
              "a v6 route around the tunnel refuses while the tunnel looks healthy");
        // The bind is judged before the far end: a dead tunnel we never bound to
        // reports the bind, because that is the thing that would have leaked.
        o = mkobs(); o.bound = false; o.observed_exit = mkpolicy().naked_exit;
        check(egress_check(mkpolicy(), o, kNow) == Verdict::NotBound, "the bind is reported before the exit");
    }

    std::printf("\negress: freshness\n");
    {
        EgressObservation o = mkobs();
        check(egress_check(mkpolicy(), o, 1000 + 300) == Verdict::Pass, "an observation exactly at the ttl still passes");
        check(egress_check(mkpolicy(), o, 1000 + 301) == Verdict::Stale, "one second past it does not");
        check(egress_check(mkpolicy(), o, 999) == Verdict::Stale, "an observation from the future is not evidence");
        o.observed_at_s = 0;
        check(egress_check(mkpolicy(), o, kNow) == Verdict::Stale, "a preflight that never ran reads as stale");
    }

    std::printf("\negress: is this the tunnel I meant\n");
    {
        EgressObservation o = mkobs(); o.observed_exit.clear();
        check(egress_check(mkpolicy(), o, kNow) == Verdict::ExitUnobserved, "no reported exit refuses");
        o = mkobs(); o.observed_exit = "not-an-address";
        check(egress_check(mkpolicy(), o, kNow) == Verdict::ExitUnobserved, "an unreadable exit is no exit");
        o = mkobs(); o.observed_exit = "192.168.1.30";
        check(egress_check(mkpolicy(), o, kNow) == Verdict::ExitPrivate, "a private exit means something intercepted us");
        o = mkobs(); o.observed_exit = "127.0.0.1";
        check(egress_check(mkpolicy(), o, kNow) == Verdict::ExitPrivate, "and so does a loopback one");
        o = mkobs(); o.observed_exit = "198.51.100.7";
        check(egress_check(mkpolicy(), o, kNow) == Verdict::ExitNaked, "our own address is the refusal this file exists for");
        o = mkobs(); o.observed_exit = "203.0.113.99";
        check(egress_check(mkpolicy(), o, kNow) == Verdict::ExitUnexpected, "an unrecognised exit refuses");

        // Naked outranks the accepted list, so a bad "trust this exit" click
        // cannot whitelist the one address the policy exists to refuse.
        EgressPolicy p = mkpolicy(); p.accepted_exits.push_back(p.naked_exit);
        o = mkobs(); o.observed_exit = p.naked_exit;
        check(egress_check(p, o, kNow) == Verdict::ExitNaked, "our own address is refused even if it is on the list");

        // Several exits accepted: a provider rotating nodes is normal.
        p = mkpolicy(); p.accepted_exits = {"203.0.113.9", "203.0.113.20"};
        o = mkobs(); o.observed_exit = "203.0.113.20";
        check(egress_check(p, o, kNow) == Verdict::Pass, "a second accepted exit passes");

        // No accepted list, naked known: public-and-not-us is the whole test.
        p = mkpolicy(); p.accepted_exits.clear();
        o = mkobs(); o.observed_exit = "203.0.113.99";
        check(egress_check(p, o, kNow) == Verdict::Pass, "with no list, any public address that is not ours passes");
        o.observed_exit = p.naked_exit;
        check(egress_check(p, o, kNow) == Verdict::ExitNaked, "but ours still refuses");
    }

    std::printf("\negress: did the lookup go with it\n");
    {
        EgressObservation o = mkobs(); o.canary = Canary::NotRun;
        check(egress_check(mkpolicy(), o, kNow) == Verdict::CanaryNotRun, "an untested lookup path refuses");
        o.canary = Canary::Failed;
        check(egress_check(mkpolicy(), o, kNow) == Verdict::CanaryFailed, "an inconclusive canary is not evidence of safety");
        o.canary = Canary::Leaked;
        check(egress_check(mkpolicy(), o, kNow) == Verdict::CanaryLeaked, "a leaking canary refuses");

        EgressPolicy p = mkpolicy(); p.dns = DnsMode::Pinned; p.resolver = "10.7.0.1";
        o = mkobs(); o.observed_resolver = "10.7.0.1";
        check(egress_check(p, o, kNow) == Verdict::Pass, "the pinned resolver answering passes");
        o.observed_resolver = "192.168.1.1";
        check(egress_check(p, o, kNow) == Verdict::ResolverMismatch, "a different resolver answering refuses");
        o.observed_resolver.clear();
        check(egress_check(p, o, kNow) == Verdict::ResolverMismatch, "and so does no resolver at all");
        // Under Proxied the resolver is the proxy's business; demanding an
        // address there would ask the observer to invent one.
        check(egress_check(mkpolicy(), o, kNow) == Verdict::Pass, "proxied lookups need no resolver address");
    }

    std::printf("\negress verdicts are nameable and say nothing\n");
    for (auto v : {Verdict::Pass, Verdict::Unconfigured, Verdict::DnsUnset, Verdict::DnsSystem,
                   Verdict::ResolverMissing, Verdict::ExitUnpinned, Verdict::NoInterface,
                   Verdict::InterfaceDown, Verdict::NotBound, Verdict::BindMismatch,
                   Verdict::V6OffTunnel, Verdict::Stale, Verdict::ExitUnobserved,
                   Verdict::ExitPrivate, Verdict::ExitNaked, Verdict::ExitUnexpected,
                   Verdict::CanaryNotRun, Verdict::CanaryFailed, Verdict::CanaryLeaked,
                   Verdict::ResolverMismatch, Verdict::ResolverBaselineMissing,
                   Verdict::CanaryDisabled}) {
        const std::string name = verdict_name(v), text = verdict_text(v);
        check(!name.empty() && !text.empty(), std::string("verdict '") + name + "' has a name and a sentence");
        check(text.find('.') != std::string::npos, std::string("verdict '") + name + "' text is a sentence");
    }
    {
        // The refusal a user screenshots must not carry their address.
        const EgressPolicy p = mkpolicy();
        bool leaks = false;
        for (auto v : {Verdict::ExitNaked, Verdict::ExitUnexpected, Verdict::ResolverMismatch}) {
            const std::string t = verdict_text(v);
            if (t.find(p.naked_exit) != std::string::npos ||
                t.find(p.accepted_exits[0]) != std::string::npos) leaks = true;
        }
        check(!leaks, "no verdict sentence contains an address");

        const std::string ref = egress_log_ref(p, Verdict::ExitNaked);
        check(ref == "egress:wg0/exit-naked", "the log ref names the interface and the verdict");
        check(ref.find(p.naked_exit) == std::string::npos, "and carries no address");
        check(egress_log_ref(EgressPolicy{}, Verdict::Unconfigured) == "egress:-/unconfigured",
              "an unnamed interface still logs safely");
    }

    std::printf("\negress refusal on a case\n");
    {
        Case k = mkcase("spokeo-20260115", "spokeo");
        k.consecutive_failures = 2;
        k.last_verified = "2026-02-01";
        const Case n = apply_egress_refusal(k, "2026-03-01");
        check(n.outcome == Outcome::Indeterminate, "a refusal records indeterminate, never not-found");
        check(n.reason == Reason::NoTunnel, "with the reason that has been waiting for a producer");
        check(n.consecutive_failures == 2, "the listing's failure streak does not move: this was our fault");
        check(n.last_attempt == "2026-03-01", "we did try, so the attempt date moves");
        check(n.last_verified == "2026-02-01", "but nothing was verified");
        check(n.next_check == "2026-03-02", "and it comes back tomorrow, not in 45 days");
        check(n.status == k.status, "a refusal is not a judgment about the listing");
        check(apply_egress_refusal(k, "2026-03-01", 0).next_check == "2026-03-02",
              "a retry of zero days still comes back, tomorrow");
        check(apply_egress_refusal(k, "2026-03-01", 7).next_check == "2026-03-08", "a longer retry is honoured");
    }

    std::printf("\negress policy pump (round-trip fidelity)\n");
    {
        const std::string f = tmp_path("delr_selftest_egress.json");

        EgressPolicy p = mkpolicy();
        p.accepted_exits.push_back("203.0.113.44");   // providers rotate exits
        p.naked_resolvers = {"198.51.100.60", "198.51.100.61"};   // and pool their resolvers
        p.tunnel_devs = {"surfshark_ipv6"};   // and split a tunnel across devices
        p.preflight_ttl_s = 120;
        check(egress_policy_save(f, p), "a policy saves");

        std::string perr;
        const EgressPolicy back = egress_policy_load(f, &perr);
        check(perr.empty(), "and loads without complaint");
        check(back.interface_name == p.interface_name, "the interface survives");
        check(back.accepted_exits == p.accepted_exits,
              "the whole exit LIST survives, in order -- a policy that lost the "
              "second exit would refuse the app the next time the provider rotated");
        check(back.naked_exit == p.naked_exit, "the baseline survives");
        check(back.naked_device == p.naked_device,
              "and so does what it was recorded ON -- without it the setup path "
              "cannot tell a tunnel that is up from one that is not, and the box "
              "goes back to empty on every launch");
        check(back.naked_resolvers == p.naked_resolvers,
              "and so does the whole resolver LIST -- a policy that dropped the "
              "second entry would wave a leak through the sibling resolver while "
              "reporting itself verified");
        check(back.tunnel_devs == p.tunnel_devs,
              "the tunnel's other devices survive -- losing them re-refuses a "
              "split tunnel with V6OffTunnel, which is the bug this fixed");
        check(back.echo_url == p.echo_url && back.canary_url == p.canary_url,
              "the preflight endpoints survive, which is what they became "
              "settings for");
        check(back.dns == p.dns, "the dns mode survives");
        check(back.proxy == p.proxy, "the proxy survives with its 'h' intact");
        check(back.preflight_ttl_s == p.preflight_ttl_s, "and so does the ttl");
        check(egress_policy_validate(back).empty(),
              "and what comes back off disk still passes validation");
        check(egress_check(back, mkobs(), kNow) == Verdict::Pass,
              "and still passes the check it passed before it was written");

        // The file is 0600, and the reason is that it contains naked_exit.
        std::error_code pec;
        const auto perms = std::filesystem::status(f, pec).permissions();
        check(!pec && (perms & (std::filesystem::perms::group_all |
                                std::filesystem::perms::others_all)) ==
                          std::filesystem::perms::none,
              "the policy file is readable by nobody but its owner -- it holds "
              "this machine's home address");

        // Fidelity includes the values we refuse. A hand-edited 'system' must
        // arrive as System and be refused BY NAME, not sanitised into Unset and
        // refused for the wrong reason.
        const std::string sysf = tmp_path("delr_selftest_egress_system.json");
        { std::ofstream o(sysf);
          o << R"({"version":1,"egress":{"interface":"wg0","dns":"system"}})"; }
        const EgressPolicy sysp = egress_policy_load(sysf);
        check(sysp.echo_url == EgressPolicy{}.echo_url &&
                  sysp.canary_url == EgressPolicy{}.canary_url,
              "a policy written before s9 has no endpoint keys and keeps the "
              "shipped ones -- an absent key is a file from another version");
        check(sysp.naked_resolvers.empty(),
              "and no resolver baseline, which is what refuses system-verified");
        check(sysp.dns == DnsMode::System,
              "a hand-edited 'system' loads as system rather than being tidied away");
        check(egress_check(sysp, mkobs(), kNow) == Verdict::DnsSystem,
              "so the refusal names the actual problem: the host resolver");
        std::remove(sysf.c_str());

        // Anything nobody has heard of has no honest sentence to say about it.
        const std::string junkf = tmp_path("delr_selftest_egress_junk.json");
        { std::ofstream o(junkf);
          o << R"({"version":1,"egress":{"interface":"wg0","dns":"telepathy"}})"; }
        // Absent and present-but-empty are different facts. A person who
        // cleared the lookup-check box meant it, and a loader that helpfully
        // restored the default would switch a check back on behind them --
        // silently, since the only visible effect is that checks stop refusing.
        const std::string emptyf = tmp_path("delr_selftest_egress_empty.json");
        { std::ofstream o(emptyf);
          o << R"({"version":1,"egress":{"interface":"wg0","dns":"proxied",)"
               R"("proxy":"socks5h://127.0.0.1:1080",)"
               R"("naked_exit":"198.51.100.7","canary_url":""}})"; }
        const EgressPolicy emptyp = egress_policy_load(emptyf);
        check(emptyp.canary_url.empty(),
              "an endpoint cleared on purpose stays cleared across a load");
        check(emptyp.echo_url == EgressPolicy{}.echo_url,
              "while the key that was never written still defaults");
        check(egress_check(emptyp, mkobs(), kNow) == Verdict::CanaryDisabled,
              "and the emptied one refuses by name, which is the point of "
              "keeping the two apart");
        std::remove(emptyf.c_str());

        check(egress_policy_load(junkf).dns == DnsMode::Unset,
              "but an unrecognised mode falls to unset, which refuses");
        std::remove(junkf.c_str());

        // First run, and a broken file. Both must land on the policy that
        // refuses everything rather than on a half-filled one.
        std::string merr;
        const EgressPolicy missing =
            egress_policy_load(tmp_path("delr_no_such_egress.json"), &merr);
        check(merr.empty() && missing.interface_name.empty() &&
                  missing.dns == DnsMode::Unset,
              "a missing policy file is first-run, not an error -- and first-run "
              "is the policy that allows nothing");
        check(egress_check(missing, mkobs(), kNow) != Verdict::Pass,
              "which refuses");

        const std::string badf = tmp_path("delr_selftest_egress_bad.json");
        { std::ofstream o(badf); o << "{ this is not json"; }
        std::string berr;
        const EgressPolicy bad = egress_policy_load(badf, &berr);
        check(!berr.empty() && bad.interface_name.empty(),
              "and malformed JSON reports rather than half-loading");
        std::remove(badf.c_str());

        std::remove(f.c_str());
    }

    std::printf("\nthe v6 routing table\n");
    {
        // /proc/net/ipv6_route, as the kernel writes it: ten fields, hex,
        // dest / dest_len / src / src_len / next_hop / metric / refcnt / use /
        // flags / dev.
        const std::string zeros(32, '0');
        auto line = [&](const std::string& dest, const char* dlen, const char* flags,
                        const char* dev) {
            return dest + " " + dlen + " " + zeros + " 00 " + zeros +
                   " 00000400 00000000 00000000 " + flags + " " + dev + "\n";
        };
        const std::string via_wg0  = line(zeros, "00", "00000001", "wg0");
        const std::string via_eth0 = line(zeros, "00", "00000003", "eth0");
        // A /64 on the local segment is not a way out.
        const std::string lan = line("20010db8000000000000000000000000", "40", "00000001", "eth0");

        check(v6_default_route("", "wg0") == V6Route::None, "an empty table routes nothing");
        check(v6_default_route(lan, "wg0") == V6Route::None, "a /64 is not a default route");
        check(v6_default_route(via_wg0, "wg0") == V6Route::TunnelOnly,
              "a default route out the tunnel is the one we want");
        check(v6_default_route(via_eth0, "wg0") == V6Route::OffTunnel,
              "a default route out anything else is the leak");
        check(v6_default_route(via_wg0 + via_eth0, "wg0") == V6Route::OffTunnel,
              "and one leak outranks any number of good routes");
        check(v6_default_route(lan + via_wg0, "wg0") == V6Route::TunnelOnly,
              "other routes do not distract from the default");

        // ── a tunnel that is more than one device ────────────────────────────
        // Surfshark's WireGuard carries v4 on `surfshark_wg` and v6 on
        // `surfshark_ipv6`. The v6 default goes out a device that is not the
        // bind device, and reading that as a leak refuses a machine whose
        // traffic is entirely inside the tunnel. Found by pointing the app at
        // a real VPN for the first time; every hermetic check before this one
        // agreed with the assumption, because they were written by whoever
        // made it.
        const std::string via_v6dev = line(zeros, "00", "00000001", "surfshark_ipv6");
        check(v6_default_route(via_v6dev, "surfshark_wg") == V6Route::OffTunnel,
              "a sibling device is a leak when nobody said it was the tunnel");
        check(v6_default_route(via_v6dev, "surfshark_wg", {"surfshark_ipv6"}) ==
                  V6Route::TunnelOnly,
              "and is not, once it is named -- the refusal a real VPN hit");
        check(v6_default_route(via_v6dev + via_eth0, "surfshark_wg", {"surfshark_ipv6"}) ==
                  V6Route::OffTunnel,
              "naming a sibling widens the tunnel and does not blind the check");
        check(v6_default_route(via_eth0, "surfshark_wg", {"surfshark_ipv6"}) ==
                  V6Route::OffTunnel,
              "a device nobody named is still a leak with a list present");
        check(v6_default_route(via_wg0, "wg0", {}) == V6Route::TunnelOnly,
              "an empty list is exactly the behaviour that shipped before it");
        check(v6_default_route(via_v6dev, "", {"surfshark_ipv6"}) == V6Route::TunnelOnly,
              "the list stands alone -- it is not a modifier on the bind device");
        check(v6_default_route(via_v6dev, "surfshark_wg", {" surfshark_ipv6 "}) ==
                  V6Route::TunnelOnly,
              "and a name typed with spaces around it is the same name");
        check(v6_default_route(via_eth0, "surfshark_wg", {""}) == V6Route::OffTunnel,
              "an empty entry matches nothing rather than everything");
        check(v6_default_route(via_v6dev, "surfshark_wg", {"surfshark_ipv"}) ==
                  V6Route::OffTunnel,
              "matching is exact: a prefix of the name is not the name");

        // An unreachable default is the OPPOSITE of a leak, and it is what a
        // v6-less host looks like when the file exists anyway.
        check(v6_default_route(line(zeros, "00", "00000201", "lo"), "wg0") == V6Route::None,
              "a reject route on lo is refusal, not egress");
        check(v6_default_route(line(zeros, "00", "00000203", "eth0"), "wg0") == V6Route::None,
              "a reject route anywhere is refusal, not egress");
        check(v6_default_route(line(zeros, "00", "00000002", "eth0"), "wg0") == V6Route::None,
              "a route that is not up cannot carry anything");

        check(v6_default_route("not a routing table at all\n", "wg0") == V6Route::Unreadable,
              "content we cannot make ten fields of is unreadable, not clean");
        check(v6_default_route("garbage\n" + via_wg0, "wg0") == V6Route::TunnelOnly,
              "but one bad line among readable ones does not blind us");
        check(v6_default_route(via_eth0, "eth0") == V6Route::TunnelOnly,
              "the tunnel is whichever interface the policy named");
    }

    std::printf("\nwhich device would a packet leave by (the setup path's hint)\n");
    {
        const std::vector<DeviceAddress> host = {
            {"lo",   "127.0.0.1"},
            {"eth0", "192.168.1.40"},
            {"eth0", "fe80::a11"},
            {"wg0",  "10.7.0.2"},
        };

        check(device_for_address("10.7.0.2", host).state == Detect::Found,
              "the address the kernel would send from names a device");
        check(device_for_address("10.7.0.2", host).device == "wg0",
              "and it is the right one");
        check(device_for_address("192.168.1.40", host).device == "eth0",
              "the ordinary connection resolves the same way");

        // The whole point of asking the kernel: whichever device it picked is
        // the answer, with no opinion here about which one SHOULD have carried
        // it. A detector that preferred a name would be the guess this replaced.
        check(device_for_address(" 10.7.0.2 ", host).device == "wg0",
              "whitespace is not part of an address");
        check(device_for_address("fe80::a11%wg0", host).device == "eth0",
              "a scope id identifies an interface, not an address, and is not "
              "matched on");

        check(device_for_address("127.0.0.1", host).state == Detect::NoRoute,
              "an address only loopback claims means the packet went nowhere -- "
              "and `lo` must never be offered as a tunnel to paste into a box");
        check(device_for_address("203.0.113.9", host).state == Detect::Unmatched,
              "an address no interface here claims is unmatched, which is a "
              "different problem from having no route");
        check(device_for_address("", host).state == Detect::NotRun,
              "nothing to match on is not an answer");

        // Two devices, one address. Real enough to matter (a bridge, a stale
        // duplicate) and the one case where guessing would write the wrong name
        // into the field that decides where every socket binds.
        const std::vector<DeviceAddress> twice = {
            {"eth0", "192.168.1.40"}, {"eth1", "192.168.1.40"}};
        check(device_for_address("192.168.1.40", twice).state == Detect::Ambiguous,
              "two devices claiming one address is refused, not resolved by "
              "picking the first");
        const std::vector<DeviceAddress> dupe = {
            {"eth0", "192.168.1.40"}, {"eth0", "192.168.1.40"}};
        check(device_for_address("192.168.1.40", dupe).state == Detect::Found,
              "but the same device listed twice is one device, which getifaddrs "
              "does routinely");

        // `lo` never wins, even when something else is available -- it is
        // skipped rather than ranked.
        const std::vector<DeviceAddress> shared = {
            {"lo", "10.7.0.2"}, {"wg0", "10.7.0.2"}};
        check(device_for_address("10.7.0.2", shared).device == "wg0",
              "loopback is never the way out");
    }

    std::printf("\nis the detected device plausibly a tunnel\n");
    {
        const DeviceFound wg{Detect::Found, "wg0"};
        const DeviceFound eth{Detect::Found, "eth0"};
        const DeviceFound none{Detect::NoRoute, ""};

        check(tunnel_check(wg, "eth0") == TunnelCheck::Distinct,
              "a device that is not the one the baseline was recorded on is "
              "consistent with a tunnel");
        check(tunnel_check(eth, "eth0") == TunnelCheck::NotUp,
              "the very device the baseline was recorded on means the VPN is "
              "off -- the guard that stops `eth0` being written in as a tunnel");
        check(tunnel_check(eth, " eth0 ") == TunnelCheck::NotUp,
              "and trimming is not a way around it");
        check(tunnel_check(wg, "") == TunnelCheck::Unchecked,
              "with no baseline device there is nothing to compare against, and "
              "saying so beats implying a check that did not happen");
        check(tunnel_check(none, "eth0") == TunnelCheck::NoDevice,
              "nothing detected is nothing to judge");
        check(tunnel_check({Detect::Found, ""}, "eth0") == TunnelCheck::NoDevice,
              "a Found with no name is not a device, whatever it claims");

        // Distinct is NOT a claim that the tunnel works. The proof is a
        // preflight, two steps later, and nothing here may stand in for it.
        check(tunnel_check(wg, "eth0") != TunnelCheck::NotUp,
              "and `Distinct` only ever rules the one thing out");
    }

    std::printf("\nwhich address is the tunnel's own\n");
    {
        check(iface_pick_address({}) == "", "an interface with no addresses has none to bind");
        check(iface_pick_address({"fe80::a11%wg0"}) == "",
              "link-local is not an address you can leave through");
        check(iface_pick_address({"127.0.0.1"}) == "", "nor is loopback");
        check(iface_pick_address({"fe80::a11%wg0", "10.7.0.2"}) == "10.7.0.2",
              "the tunnel's v4 is the address both sides will name");
        check(iface_pick_address({"fd00::2", "10.7.0.2"}) == "10.7.0.2",
              "v4 wins outright, whatever the order");
        check(iface_pick_address({"fe80::a11%wg0", "fd00::2"}) == "fd00::2",
              "a v6-only tunnel still has one usable address");
        check(iface_pick_address({" 10.7.0.2 "}) == "10.7.0.2", "whitespace is not part of it");
        check(iface_pick_address({"010.7.0.2"}) == "",
              "an ambiguous address is not an address (leading zero)");
    }

    std::printf("\nwhat the preflight echo said\n");
    {
        check(echo_read("203.0.113.9\n") == "203.0.113.9", "a bare answer is the easy case");
        check(echo_read("{\"ip\":\"203.0.113.9\"}") == "203.0.113.9", "so is a scrap of JSON");
        check(echo_read("Your IP address is 203.0.113.9.") == "203.0.113.9",
              "a trailing full stop is punctuation, not an octet");
        check(echo_read("203.0.113.9 203.0.113.9") == "203.0.113.9",
              "the same address twice is still one answer");
        check(echo_read("2001:db8::1\n") == "2001:db8::1", "v6 reads the same way");
        check(echo_read("") == "", "an empty body says nothing");
        check(echo_read("could not determine your address") == "", "and neither does prose");
        // 1.0.0.1 is a valid address AND a version number. An echo that needs
        // disambiguating is pointed at the wrong service.
        check(echo_read("delr 1.0.0.1 -- you are 203.0.113.9") == "",
              "two readable addresses is ambiguity, and ambiguity is no answer");
        check(echo_read(std::string(5000, ' ') + "203.0.113.9") == "",
              "an answer buried past 4k is not an echo service");
        check(addr_kind(echo_read("10.0.0.4\n")) == AddrKind::Private,
              "a private echo is read, not discarded -- ExitPrivate is a real verdict");
    }

    std::printf("\nwhat the DNS canary said\n");
    {
        const std::string naked = "198.51.100.7";
        const std::vector<std::string> none;
        check(canary_read("203.0.113.10", naked, none) == Canary::Clean,
              "a public answer that is not ours went out with the tunnel");
        check(canary_read(naked, naked, none) == Canary::Leaked,
              "our own address asking is the leak this exists to see");
        check(canary_read("192.168.1.1", naked, none) == Canary::Leaked,
              "so is something inside the building answering");
        check(canary_read("127.0.0.1", naked, none) == Canary::Leaked, "and so is a local stub");
        check(canary_read("", naked, none) == Canary::Failed, "no answer is not evidence of safety");
        check(canary_read("nonsense", naked, none) == Canary::Failed, "nor is an unreadable one");
        check(canary_read("203.0.113.10", "", none) == Canary::Clean,
              "with no baseline, public and readable is the whole of what can be known");
        check(canary_read("198.51.100.7 ", naked, none) == Canary::Leaked, "whitespace does not launder it");

        // ── s9: the resolver baseline, which is what makes SystemVerified a
        // mode rather than a rename of System. Every one of these answers is a
        // perfectly ordinary public address; the ONLY thing that tells them
        // apart is having watched them with the tunnel off.
        const std::vector<std::string> isp = {"198.51.100.60", "198.51.100.61"};
        check(canary_read("198.51.100.60", naked, isp) == Canary::Leaked,
              "a resolver that answers with the tunnel down, answering with it "
              "up, is the ordinary leak -- and it is a public address that "
              "looks like nothing in particular");
        check(canary_read("198.51.100.61", naked, isp) == Canary::Leaked,
              "and so is the SECOND one in the pool, which is the entire reason "
              "this is a list: recognising only the sampled resolver would wave "
              "its sibling through while looking verified");
        check(canary_read("203.0.113.10", naked, isp) == Canary::Clean,
              "a resolver never seen without the tunnel is the tunnel's, as far "
              "as this can honestly tell");
        check(canary_read("198.51.100.60", "", isp) == Canary::Leaked,
              "the resolver baseline stands on its own -- no exit baseline needed");
        check(canary_read("198.51.100.60 ", naked, isp) == Canary::Leaked,
              "whitespace does not launder this one either");
        check(canary_read("::ffff:198.51.100.60", naked, isp) == Canary::Leaked,
              "nor does a v4-in-v6 spelling of the same address");

        // Both baselines are used ONLY to fail. A garbage baseline removes a way
        // to catch a leak; it can never manufacture one, and it can never
        // manufacture a pass either.
        const std::vector<std::string> junk = {"", "nonsense", "10.0.0.1", "127.0.0.1"};
        check(canary_read("203.0.113.10", naked, junk) == Canary::Clean,
              "unreadable and private entries in the baseline match nothing");
        check(canary_read("10.0.0.1", naked, junk) == Canary::Leaked,
              "and a private answer is still the leak, by the rule that was "
              "always there rather than by matching the junk");
    }

    std::printf("\nreadings become an observation\n");
    {
        const EgressPolicy p = mkpolicy();
        const std::string zeros(32, '0');
        const std::string via_wg0 = zeros + " 00 " + zeros + " 00 " + zeros +
                                    " 00000400 00000000 00000000 00000001 wg0\n";
        const std::string via_eth0 = zeros + " 00 " + zeros + " 00 " + zeros +
                                     " 00000400 00000000 00000000 00000003 eth0\n";

        // What the shim reports when everything is exactly right.
        ProbeReadings r;
        r.interface_present   = true;
        r.interface_up        = true;
        r.interface_addresses = {"fe80::a11%wg0", "10.7.0.2"};
        r.bound         = true;
        r.bound_address = "10.7.0.2";
        r.routes        = ProbeReadings::RouteSource::Text;
        r.routes_text   = via_wg0;
        r.echo_ran      = true;
        r.echo_body     = "203.0.113.9\n";
        r.canary_ran    = true;
        r.canary_answered_by = "203.0.113.10";
        r.observed_at_s = 1000;

        const EgressObservation o = observation_from(r, p);
        check(o.interface_address == "10.7.0.2", "the observation names the tunnel's own address");
        check(o.bound_address == "10.7.0.2", "and the address the socket actually got");
        check(!o.v6_default_offtunnel, "a tunnel-only route table is not a leak");
        check(o.observed_exit == "203.0.113.9", "the echo becomes the observed exit");
        check(o.canary == Canary::Clean, "the whoami becomes the canary");
        check(egress_check(p, o, 1100) == Verdict::Pass,
              "and honest readings from a healthy tunnel pass the policy");

        // A default-constructed observer has looked at nothing, and must not
        // pass on silence.
        const EgressObservation blind = observation_from(ProbeReadings{}, p);
        check(!blind.interface_present && !blind.bound, "unread fields stay unread");
        check(blind.v6_default_offtunnel, "and an unread routing table fails closed");
        check(blind.canary == Canary::NotRun, "a canary nobody ran did not pass");
        check(!egress_may_fetch(p, blind, 1100), "a blind observer never allows a fetch");

        // One thing wrong at a time, each landing on its own verdict.
        ProbeReadings v6 = r; v6.routes_text = via_wg0 + via_eth0;
        check(egress_check(p, observation_from(v6, p), 1100) == Verdict::V6OffTunnel,
              "a v6 default around the tunnel refuses, tunnel up and all");

        ProbeReadings unread = r; unread.routes = ProbeReadings::RouteSource::Unread;
        check(egress_check(p, observation_from(unread, p), 1100) == Verdict::V6OffTunnel,
              "a routing table nobody read refuses the same way");

        ProbeReadings nov6 = r; nov6.routes = ProbeReadings::RouteSource::Absent;
        check(egress_check(p, observation_from(nov6, p), 1100) == Verdict::Pass,
              "but a kernel with no v6 at all has nothing to leak, and is not punished");

        ProbeReadings naked = r; naked.echo_body = p.naked_exit;
        check(egress_check(p, observation_from(naked, p), 1100) == Verdict::ExitNaked,
              "an echo of our own address is the refusal the module exists for");

        ProbeReadings leak = r; leak.canary_answered_by = p.naked_exit;
        check(egress_check(p, observation_from(leak, p), 1100) == Verdict::CanaryLeaked,
              "a lookup that went out naked refuses, HTTP notwithstanding");

        ProbeReadings noecho = r; noecho.echo_ran = false;
        check(egress_check(p, observation_from(noecho, p), 1100) == Verdict::ExitUnobserved,
              "a preflight that did not echo produced no exit to judge");

        ProbeReadings nocanary = r; nocanary.canary_ran = false;
        check(egress_check(p, observation_from(nocanary, p), 1100) == Verdict::CanaryNotRun,
              "and one that did not look up names cannot say the lookup was clean");

        // ── s9: the whole SystemVerified path, readings to verdict ───────────
        // The point of these four is that the readings are IDENTICAL to the
        // healthy ones above except for the resolver that answered, and that
        // resolver is an ordinary public address. Before the baseline existed,
        // every one of these read as Clean.
        {
            EgressPolicy sv = mkpolicy();
            sv.dns = DnsMode::SystemVerified;
            sv.proxy.clear();
            sv.naked_resolvers = {"198.51.100.60", "198.51.100.61"};

            check(egress_check(sv, observation_from(r, sv), 1100) == Verdict::Pass,
                  "system-verified passes on a tunnel whose lookups are answered "
                  "by a resolver never seen without it");

            ProbeReadings isp = r; isp.canary_answered_by = "198.51.100.60";
            check(egress_check(sv, observation_from(isp, sv), 1100) == Verdict::CanaryLeaked,
                  "and refuses when the resolver that answers without the tunnel "
                  "answers with it up -- the ordinary leak, invisible until s9");

            ProbeReadings sibling = r; sibling.canary_answered_by = "198.51.100.61";
            check(egress_check(sv, observation_from(sibling, sv), 1100) == Verdict::CanaryLeaked,
                  "including via the sibling in the pool, which a single recorded "
                  "resolver would have waved through while reporting verified");

            EgressPolicy one = sv; one.naked_resolvers = {"198.51.100.60"};
            check(egress_check(one, observation_from(sibling, one), 1100) == Verdict::Pass,
                  "and that is not a hypothetical: with only the first recorded, "
                  "the very same leak passes");
        }

        ProbeReadings unbound = r; unbound.bound = false; unbound.bound_address = "";
        check(egress_check(p, observation_from(unbound, p), 1100) == Verdict::NotBound,
              "a socket that did not bind is the killswitch working");

        ProbeReadings other = r; other.interface_addresses = {"10.7.0.9"};
        check(egress_check(p, observation_from(other, p), 1100) == Verdict::BindMismatch,
              "and a bind to something that is not the tunnel's address is caught");

        // Pinned resolvers: the address we SENT to is the one the policy
        // compares, not the address the whoami reported back.
        EgressPolicy pin = p;
        pin.dns = DnsMode::Pinned;
        pin.resolver = "10.7.0.1";
        ProbeReadings pr = r;
        check(egress_check(pin, observation_from(pr, pin), 1100) == Verdict::ResolverMismatch,
              "an unreported resolver cannot satisfy a pinned policy");
        pr.resolver_used = "10.7.0.1";
        check(egress_check(pin, observation_from(pr, pin), 1100) == Verdict::Pass,
              "the resolver we asked is the one the policy checks");
        pr.resolver_used = "9.9.9.9";
        check(egress_check(pin, observation_from(pr, pin), 1100) == Verdict::ResolverMismatch,
              "and asking a different one is a mismatch even when the canary is clean");
    }

    // ── PageRules: what makes a fetched page a NotFound ──────────────────────

    std::printf("\npage text normalising\n");
    check(page_text("<p>No <b>results</b> found.</p>") == "no results found.",
          "tags come out and the words survive");
    check(page_text("a<br>b") == "a b",
          "a tag is a word boundary, not a deletion");
    check(page_text("No&nbsp;results") == "no results",
          "&nbsp; is a space like any other");
    check(page_text("Smith &amp; Sons") == "smith & sons",
          "an entity we know decodes; one we do not stays literal");
    check(page_text("R&D dept") == "r&d dept",
          "a bare ampersand is not an entity and is not eaten");
    check(page_text("<script>var s='no results found';</script>Listed") == "listed",
          "a marker inside a script matches the site's code, not its page");
    check(page_text("<style>.x{content:'no results'}</style>Listed") == "listed",
          "and the same for a stylesheet");
    check(page_text("<!-- no results found --><p>Listed</p>") == "listed",
          "a comment is skipped to its terminator, markup inside it and all");
    check(page_text("  A\n\t B  ") == "a b",
          "whitespace collapses and the ends are trimmed");
    check(page_text("").empty() && page_text("<div></div>").empty(),
          "a page with no text has no text");

    std::printf("\npage marker matching\n");
    {
        const std::string t = page_text("<h1>No Results Found</h1>");
        check(page_contains(t, "no results found"), "a marker matches its page");
        check(page_contains(t, "No&nbsp;Results"),
              "the marker is normalised too, so an author can write it either way");
        check(!page_contains(t, ""),
              "an empty marker matches nothing -- that is how a rules file "
              "silently starts reporting every page as a hit");
    }

    std::printf("\npage rule validation\n");
    {
        PageRule ok;
        ok.broker_id   = "acme";
        ok.fingerprint = {"acme people search"};
        ok.present     = {"view full report"};
        ok.absent      = {"no results found"};
        ok.reviewed    = "2026-08-01";

        check(rules_validate({ok}).empty(), "a complete rule validates clean");

        PageRule nofp = ok; nofp.fingerprint.clear();
        check(rules_validate({nofp}).size() == 1,
              "a rule with no fingerprint cannot tell the broker's page from a "
              "login wall, and is refused");

        PageRule noabs = ok; noabs.absent.clear();
        check(rules_validate({noabs}).size() == 1,
              "and one with no absent marker can never report a removal");

        PageRule tiny = ok; tiny.absent = {"no"};
        check(rules_validate({tiny}).size() == 1, "a two-letter marker is caught");

        PageRule both = ok; both.absent.push_back("View Full Report");
        check(rules_validate({both}).size() == 1,
              "a string in both lists guarantees Ambiguous forever while "
              "looking configured");

        PageRule dated = ok; dated.reviewed = "2026-13-40";
        check(rules_validate({dated}).size() == 1, "a malformed review date is caught");

        PageRule anon = ok; anon.broker_id.clear();
        check(!rules_validate({anon}).empty(), "a rule with no broker is caught");
        check(rules_validate({ok, ok}).size() == 1, "and so is a duplicate");

        Roster r = {mk("acme", Method::Web)};
        check(rules_validate({ok}, &r).empty(), "a rule for a known broker is fine");
        Roster other = {mk("notacme", Method::Web)};
        check(rules_validate({ok}, &other).size() == 1,
              "a rule for a broker the roster has never heard of is a dead entry");

        check(rules_find({ok}, "acme") != nullptr && rules_find({ok}, "nope") == nullptr,
              "lookup answers, and answers null when it has nothing");
    }

    std::printf("\nrule freshness\n");
    {
        PageRule r;
        r.reviewed = "2026-08-01";
        check(rule_age_days(r, "2026-08-17") == 16, "age is counted in days");
        check(!rule_stale(r, "2026-08-17"), "a rule read this month is not stale");
        check(rule_stale(r, "2027-09-01"), "one read two summers ago is");
        check(rule_age_days(r, "2026-07-01") == -1, "a review in the future is unusable");
        PageRule never;
        check(rule_age_days(never, "2026-08-17") == -1 && rule_stale(never, "2026-08-17"),
              "a rule nobody has ever read counts as stale, not as fresh");
    }

    std::printf("\npage verdicts: the status comes first\n");
    {
        PageRule r;
        r.broker_id = "acme";
        r.fingerprint = {"acme people search"};
        r.present = {"view full report"};
        r.absent  = {"no results found"};

        // The body says "no results found" every time below. A refused request
        // is not a page about the listing, and reading it as one is how a bot
        // wall becomes a removal.
        const std::string body =
            "<html><body><h1>Acme People Search</h1><p>No results found.</p></body></html>";

        check(page_check(&r, 403, body) == PageVerdict::HttpBlocked,
              "a 403's body belongs to the bot wall, not to the broker's page");
        check(page_check(&r, 429, body) == PageVerdict::HttpThrottled,
              "a 429 is the site asking us to slow down");
        check(page_check(&r, 503, body) == PageVerdict::HttpServerError,
              "a 5xx is their failure, not an answer");
        check(page_check(&r, 404, body) == PageVerdict::HttpDead,
              "a 404 is never a removal -- brokers retire slugs and re-serve "
              "the same record under a new one");
        check(page_check(&r, 410, body) == PageVerdict::HttpDead, "and 410 with it");
        check(page_check(&r, 302, body) == PageVerdict::HttpUnexpected,
              "an unfollowed redirect is not a page");
        check(page_check(&r, 0, body) == PageVerdict::NoResponse,
              "and no response at all is not a page either");
        check(page_check(&r, 200, body) == PageVerdict::Absent,
              "the same body at 200 is the answer we came for");
    }

    std::printf("\npage verdicts: is this even the right page\n");
    {
        PageRule r;
        r.broker_id = "acme";
        r.fingerprint = {"acme people search"};
        r.present = {"view full report"};
        r.absent  = {"no results found"};

        check(page_check(nullptr, 200, "anything") == PageVerdict::NoRule,
              "no rule means the listing was reached but not read");
        check(page_check(&r, 200, "") == PageVerdict::Empty,
              "a 200 with no text is not a page");
        check(page_check(&r, 200, "<html><body>Sign in to continue</body></html>")
                  == PageVerdict::Unfingerprinted,
              "a login wall does not carry the broker's chrome, and without the "
              "fingerprint it would have read as a clean absence");
        check(page_check(&r, 200,
                  "<h1>Acme People Search</h1><p>Welcome to our new site!</p>")
                  == PageVerdict::Silent,
              "the broker's page saying neither thing is a rotted rule, not a verdict");
        check(page_check(&r, 200,
                  "<h1>Acme People Search</h1><p>No results found.</p>"
                  "<a>View full report</a>") == PageVerdict::Ambiguous,
              "a page that reads as both is a broken rule -- picking a winner "
              "would be picking which lie to tell");
    }

    std::printf("\npage verdicts: the listing's own details\n");
    {
        PageRule r;
        r.broker_id = "acme";
        r.fingerprint = {"acme people search"};
        r.present = {"view full report"};
        r.absent  = {"no results found"};
        r.needs_needle = true;

        PageNeedles n;
        n.terms = {"jane q public", "centerville"};

        const std::string mine =
            "<h1>Acme People Search</h1><h2>Jane Q Public</h2>"
            "<p>Centerville, TN</p><a>View full report</a>";
        const std::string someone_else =
            "<h1>Acme People Search</h1><h2>John Smith</h2><a>View full report</a>";

        check(page_check(&r, 200, mine, n) == PageVerdict::Present,
              "the presence marker plus the person is a sighting");
        check(page_check(&r, 200, someone_else, n) == PageVerdict::NeedleAbsent,
              "the presence marker WITHOUT the person is the template, not the "
              "listing -- and on a per-person page that is an absence");
        check(page_check(&r, 200,
                  "<h1>Acme People Search</h1><p>Nothing here.</p>", n)
                  == PageVerdict::NeedleAbsent,
              "so is the broker's page with no markers and none of the details");
        check(page_check(&r, 200, mine) == PageVerdict::NoNeedles,
              "a rule that checks identity and gets no identity refuses rather "
              "than falling back to the marker");
        check(page_check(&r, 200,
                  "<h1>Acme People Search</h1><p>No results found.</p>", n)
                  == PageVerdict::Absent,
              "and the broker's own words about absence still outrank ours");

        PageNeedles all = n;
        all.require_all = true;
        check(page_check(&r, 200,
                  "<h1>Acme People Search</h1><h2>Jane Q Public</h2>"
                  "<a>View full report</a>", all) == PageVerdict::NeedleAbsent,
              "under require_all, a listing missing one term is not confirmed");

        PageRule loose = r;
        loose.needs_needle = false;
        check(page_check(&loose, 200, someone_else, n) == PageVerdict::Present,
              "a rule that did not opt into identity matching cannot use it, "
              "and reports the marker it was given");
        check(page_check(&loose, 200,
                  "<h1>Acme People Search</h1><p>Nothing here.</p>", n)
                  == PageVerdict::Silent,
              "nor may it infer an absence from details it was never told to check");
    }

    std::printf("\npage verdicts are nameable and say nothing\n");
    {
        const PageVerdict all[] = {
            PageVerdict::Present, PageVerdict::Absent, PageVerdict::NeedleAbsent,
            PageVerdict::NoRule, PageVerdict::NoNeedles, PageVerdict::Unfingerprinted,
            PageVerdict::Ambiguous, PageVerdict::Silent, PageVerdict::Empty,
            PageVerdict::NoResponse, PageVerdict::HttpDead, PageVerdict::HttpBlocked,
            PageVerdict::HttpThrottled, PageVerdict::HttpServerError,
            PageVerdict::HttpUnexpected, PageVerdict::HttpBlockedEgress,
            PageVerdict::HttpBlockedClient, PageVerdict::FunnelOnly};
        bool named = true, spoke = true, quiet = true;
        for (PageVerdict v : all) {
            const std::string nm = page_verdict_name(v);
            const std::string tx = page_verdict_text(v);
            if (nm.empty()) named = false;
            if (tx.size() < 12) spoke = false;
            // No URL, no host, no page content ever reaches a verdict string.
            if (tx.find("http") != std::string::npos ||
                tx.find("://") != std::string::npos ||
                tx.find(".com") != std::string::npos) quiet = false;
        }
        check(named, "every page verdict has a name");
        check(spoke, "and an actionable sentence to go with it");
        check(quiet, "and none of them carries a URL");
        check(page_verdict_is_clean(PageVerdict::Present) &&
              page_verdict_is_clean(PageVerdict::Absent) &&
              page_verdict_is_clean(PageVerdict::NeedleAbsent) &&
              !page_verdict_is_clean(PageVerdict::Silent) &&
              !page_verdict_is_clean(PageVerdict::HttpDead),
              "three verdicts answer the question; the rest are refusals");
    }

    std::printf("\nthe three walls, told apart (s15)\n");
    {
        // The vocabulary exists because one `Blocked` made three problems with
        // three different owners look like one. Each of these pins a wall the
        // first live checks actually hit.
        const std::string vpn_wall =
            "<html><body><h1>Access Denied</h1><p>We do not allow connections "
            "from VPN or proxy services.</p></body></html>";
        const std::string bot_wall =
            "<html><body><h1>Access Denied</h1><p>Please enable JavaScript to "
            "continue.</p></body></html>";
        const std::string mute_wall =
            "<html><body><h1>403 Forbidden</h1></body></html>";

        check(refusal_attribute(vpn_wall) == PageVerdict::HttpBlockedEgress,
              "a wall that names VPNs refused the address, and says so");
        check(refusal_attribute(bot_wall) == PageVerdict::HttpBlockedClient,
              "a wall that asks for JavaScript refused the client, not the exit");
        check(refusal_attribute(mute_wall) == PageVerdict::HttpBlocked,
              "a wall that says only 'forbidden' is left unattributed -- "
              "guessing here sends the user to fix the wrong thing");
        check(refusal_attribute("") == PageVerdict::HttpBlocked,
              "and a wall with no words at all stays unattributed");

        // The Ambiguous rule, applied to walls: two stories is no story.
        const std::string both =
            "<html><body><p>VPN traffic is not permitted and you must enable "
            "JavaScript.</p></body></html>";
        check(refusal_attribute(both) == PageVerdict::HttpBlocked,
              "a wall telling both stories is telling neither");

        // Generic refusal words are deliberately absent from both sets: they
        // are what plain Blocked already means.
        check(refusal_attribute("<p>Access denied. Request blocked.</p>")
                  == PageVerdict::HttpBlocked,
              "generic anger is not attribution");

        PageRule r;
        r.broker_id = "acme";
        r.fingerprint = {"acme people search"};
        r.present = {"view full report"};
        r.absent = {"no results found"};

        check(page_check(&r, 403, vpn_wall) == PageVerdict::HttpBlockedEgress &&
              page_check(&r, 403, bot_wall) == PageVerdict::HttpBlockedClient &&
              page_check(&r, 401, vpn_wall) == PageVerdict::HttpBlockedEgress,
              "page_check routes 401 and 403 through the wall's own words");

        // The listing markers must STILL never run over a wall's body, however
        // the wall is attributed. This is the s8 invariant and it survives.
        const std::string wall_saying_absent =
            "<html><body><h1>Acme People Search</h1><p>No results found.</p>"
            "<p>Please enable JavaScript.</p></body></html>";
        check(page_check(&r, 403, wall_saying_absent) == PageVerdict::HttpBlockedClient,
              "a wall echoing the broker's own absence copy is still a wall");

        // Whose fault, and therefore whose streak.
        Case k = mkcase("wall-1", "acme");
        k.consecutive_failures = 3;
        check(apply_page_verdict(k, PageVerdict::HttpBlockedEgress, "2026-03-01", 45)
                  .consecutive_failures == 3 &&
              apply_page_verdict(k, PageVerdict::HttpBlockedClient, "2026-03-01", 45)
                  .consecutive_failures == 3,
              "an attributed wall is our configuration, so it does not move the "
              "streak toward calling a live listing dead");
        check(apply_page_verdict(k, PageVerdict::HttpBlocked, "2026-03-01", 45)
                  .consecutive_failures == 4,
              "an UNattributed wall might really be the broker, and still counts");

        check(page_outcome(PageVerdict::HttpBlockedEgress).reason == Reason::EgressBlocked &&
              page_outcome(PageVerdict::HttpBlockedClient).reason == Reason::ClientBlocked &&
              page_outcome(PageVerdict::HttpBlocked).reason == Reason::Blocked,
              "each wall reaches the case under its own name");
        check(page_outcome(PageVerdict::HttpBlockedEgress).outcome == Outcome::Indeterminate &&
              page_outcome(PageVerdict::HttpBlockedClient).outcome == Outcome::Indeterminate &&
              page_outcome(PageVerdict::FunnelOnly).outcome == Outcome::Indeterminate,
              "and none of them rounds to NotFound -- a wall is not a removal");
    }

    std::printf("\na broker with no page to check\n");
    {
        PageRule funnel;
        funnel.broker_id = "funnelco";
        funnel.funnel_only = true;
        funnel.notes = "Lead-capture funnel; results live behind a form.";

        check(!rule_verifiable(&funnel),
              "a funnel-only broker refuses the fetch before it happens");
        check(rule_verifiable(nullptr),
              "but a MISSING rule is still fetchable -- unreadable and "
              "unpublishable are different refusals with different owners");

        // Whatever came back, it is not evidence. Even a perfectly good page.
        check(page_check(&funnel, 200,
                  "<html><body><p>View full report</p></body></html>")
                  == PageVerdict::FunnelOnly,
              "and a 200 from a funnel is still not a listing");
        check(page_check(&funnel, 403, "<p>Access denied</p>") == PageVerdict::FunnelOnly,
              "nor does the status code get a say");

        check(page_outcome(PageVerdict::FunnelOnly).reason == Reason::NoListingPage,
              "it reaches the case as NoListingPage");

        Case k = mkcase("funnel-1", "funnelco");
        k.consecutive_failures = 2;
        check(apply_page_verdict(k, PageVerdict::FunnelOnly, "2026-03-01", 45)
                  .consecutive_failures == 2,
              "nobody failed, so nothing is counted against anybody");

        // Validation: a funnel rule carries one fact and no markers.
        PageRules ok = {funnel};
        check(rules_validate(ok).empty(),
              "a funnel-only rule needs no fingerprint and no markers");

        PageRule contradictory = funnel;
        contradictory.needs_needle = true;
        PageRules bad = {contradictory};
        check(!rules_validate(bad).empty(),
              "and may not also demand needles from a page that does not exist");

        PageRule littered = funnel;
        littered.present = {"view full report"};
        PageRules junk = {littered};
        check(!rules_validate(junk).empty(),
              "markers that will never run are a mistake, said out loud");

        // Round-trip: the flag survives the pump.
        const std::string f = tmp_path("delr_funnel_rules.json");
        std::filesystem::remove(f);
        check(rules_save(f, ok), "a funnel-only rule saves");
        PageRules back = rules_load(f);
        check(back.size() == 1 && back[0].funnel_only,
              "and comes back still knowing there is no page");
        std::filesystem::remove(f);
    }

    std::printf("\nthe queues a person can actually act on\n");
    {
        // Three lists, three owners. Collapsing any two is how the app spent
        // six sessions blaming the page rules for a missing profile.
        Caseload c;
        auto add = [&](const char* id, Reason r) {
            Case k = mkcase(id, "acme");
            k.outcome = Outcome::Indeterminate;
            k.reason = r;
            c.push_back(k);
        };
        add("m1", Reason::NoRule);
        add("m2", Reason::PageUnreadable);
        add("w1", Reason::EgressBlocked);
        add("w2", Reason::ClientBlocked);
        add("w3", Reason::Blocked);
        add("d1", Reason::NoListingPage);
        Case live = mkcase("live", "acme");
        live.outcome = Outcome::Listed;
        c.push_back(live);

        check(caseload_unverifiable(c).size() == 2,
              "the maintenance queue holds only what a maintainer can fix");
        check(caseload_walled(c).size() == 3,
              "the walled list holds every refusal from the far end");
        check(caseload_unverifiable_by_design(c).size() == 1,
              "and a broker with no page to check sits in neither");

        // The one that matters: a wall must never look like a rotted rule.
        for (const Case* k : caseload_unverifiable(c))
            check(k->reason != Reason::EgressBlocked &&
                  k->reason != Reason::ClientBlocked &&
                  k->reason != Reason::NoListingPage,
                  std::string("no wall is filed as a maintenance job (") +
                      k->id + ")");
    }

    std::printf("\nwhat a page verdict means to a case\n");
    {
        check(page_outcome(PageVerdict::Present).outcome == Outcome::Listed,
              "a sighting is Listed");
        check(page_outcome(PageVerdict::Absent).outcome == Outcome::NotFound &&
              page_outcome(PageVerdict::NeedleAbsent).outcome == Outcome::NotFound,
              "both absences are NotFound");
        check(page_outcome(PageVerdict::Present).reason == Reason::None &&
              page_outcome(PageVerdict::Absent).reason == Reason::None,
              "a clean fetch carries no reason");

        bool all_indet = true;
        const PageVerdict refusals[] = {
            PageVerdict::NoRule, PageVerdict::NoNeedles, PageVerdict::Unfingerprinted,
            PageVerdict::Ambiguous, PageVerdict::Silent, PageVerdict::Empty,
            PageVerdict::NoResponse, PageVerdict::HttpDead, PageVerdict::HttpBlocked,
            PageVerdict::HttpThrottled, PageVerdict::HttpServerError,
            PageVerdict::HttpUnexpected};
        for (PageVerdict v : refusals)
            if (page_outcome(v).outcome != Outcome::Indeterminate) all_indet = false;
        check(all_indet, "and every refusal is Indeterminate, which never rounds");

        check(page_outcome(PageVerdict::NoRule).reason == Reason::NoRule &&
              page_outcome(PageVerdict::Silent).reason == Reason::PageUnreadable &&
              page_outcome(PageVerdict::HttpBlocked).reason == Reason::Blocked &&
              page_outcome(PageVerdict::HttpDead).reason == Reason::UrlDead,
              "and the reason says whose bug it is");
    }

    std::printf("\napplying a page verdict\n");
    {
        Case k = mkcase("c-page", "acme");
        k.consecutive_failures = 2;

        Case blocked = apply_page_verdict(k, PageVerdict::HttpBlocked, "2026-08-17", 45);
        check(blocked.consecutive_failures == 3,
              "a broker refusing us is a fact about the broker, and counts");
        check(blocked.last_attempt == "2026-08-17" && blocked.last_verified.empty(),
              "a refused check moves the attempt and not the verification");

        Case rotted = apply_page_verdict(k, PageVerdict::Silent, "2026-08-17", 45);
        check(rotted.consecutive_failures == 2,
              "a rotted rule is a fact about US, and must not accumulate into a "
              "case for abandoning the listing");
        check(rotted.reason == Reason::PageUnreadable, "and it is named as ours");
        check(rotted.next_check == "2026-10-01",
              "rescheduled at the normal cadence -- a tunnel may be up tomorrow, "
              "a rule maintainer will not be");

        Case norule = apply_page_verdict(k, PageVerdict::NoRule, "2026-08-17", 45);
        check(norule.consecutive_failures == 2 && norule.reason == Reason::NoRule,
              "a missing rule is the same shape of problem");

        Case seen = apply_page_verdict(k, PageVerdict::Present, "2026-08-17", 45);
        check(seen.outcome == Outcome::Listed && seen.consecutive_failures == 0 &&
              seen.clean_absences == 0 && seen.last_verified == "2026-08-17",
              "a sighting is a clean fetch: streak cleared, absences wiped");

        Case gone = apply_page_verdict(k, PageVerdict::NeedleAbsent, "2026-08-17", 45);
        check(gone.outcome == Outcome::NotFound && gone.clean_absences == 1,
              "an absence is a clean fetch too, and starts the streak");
        check(gone.status == Status::Found,
              "and does NOT promote -- believing it gone is a separate judgment");

        Case twice = apply_page_verdict(gone, PageVerdict::Absent, "2026-10-01", 45);
        check(twice.clean_absences == 2 &&
              promotion_for(twice) == Promotion::Removed,
              "two clean absences is the pattern the promotion rule waits for");
    }

    std::printf("\nthe maintenance queue\n");
    {
        Caseload c;
        Case a = mkcase("c-a", "acme");
        a = apply_page_verdict(a, PageVerdict::NoRule, "2026-08-17", 45);
        Case b = mkcase("c-b", "acme");
        b = apply_page_verdict(b, PageVerdict::Ambiguous, "2026-08-17", 45);
        Case d = mkcase("c-d", "acme");
        d = apply_page_verdict(d, PageVerdict::HttpBlocked, "2026-08-17", 45);
        Case e = mkcase("c-e", "acme");
        e = apply_page_verdict(e, PageVerdict::Absent, "2026-08-17", 45);
        c = {a, b, d, e};

        const std::vector<const Case*> stuck = caseload_unverifiable(c);
        check(stuck.size() == 2,
              "the queue holds the cases WE cannot read, not the ones they refused");
        check(stuck[0]->id == "c-a" && stuck[1]->id == "c-b",
              "and names them, so a report can say how many listings it is not "
              "actually verifying");
    }

    std::printf("\npage rules pump (round-trip fidelity)\n");
    {
        PageRule r;
        r.broker_id    = "acme";
        r.fingerprint  = {"acme people search", "acme inc"};
        r.present      = {"view full report", "background report for"};
        r.absent       = {"no results found"};
        r.needs_needle = true;
        r.reviewed     = "2026-08-01";
        r.notes        = "shape reference";

        const std::string f = tmp_path("delr_selftest_rules.json");
        check(rules_save(f, {r}), "rules save");
        std::string err;
        const PageRules back = rules_load(f, &err);
        check(err.empty() && back.size() == 1, "and load, cleanly");
        if (back.size() == 1) {
            const PageRule& g = back[0];
            check(g.broker_id == r.broker_id && g.fingerprint == r.fingerprint &&
                      g.present == r.present && g.absent == r.absent &&
                      g.needs_needle == r.needs_needle && g.reviewed == r.reviewed &&
                      g.notes == r.notes,
                  "every field survives the round trip -- what you save is what "
                  "you load");
        }
        std::remove(f.c_str());

        std::string err2;
        check(rules_load(tmp_path("delr_no_such_rules.json"), &err2).empty() &&
                  err2.empty(),
              "a missing rules file is a first run, not an error");

        const std::string bad = tmp_path("delr_selftest_rules_bad.json");
        { std::ofstream o(bad); o << "{ this is not json"; }
        std::string err3;
        check(rules_load(bad, &err3).empty() && !err3.empty(),
              "and malformed JSON reports rather than half-loading");
        std::remove(bad.c_str());
    }



    // ── net/Fetch -- the transport ────────────────────────────────────────
    // Everything here is hermetic. Not one check below opens a socket: the
    // url guard is pure, the error vocabulary is a table, the binding is a
    // projection of a policy, and the two calls that DO reach curl are
    // arranged to fail before it -- an empty bind address and a bad url both
    // return before a handle is opened. A selftest that needed the network
    // would be a selftest that fails on a train.
    {
        using namespace delr::net;

        std::printf("\nnet/Fetch -- is this a url we will point a socket at\n");
        check(fetch_url_ok("https://example.com/listing/7"), "https passes");
        check(fetch_url_ok("http://example.com"), "so does plain http");
        check(fetch_url_ok("HTTPS://Example.COM/x"), "the scheme is read case-blind");
        check(!fetch_url_ok(""), "empty is not a url");
        check(!fetch_url_ok("example.com"), "and neither is a bare host");
        check(!fetch_url_ok("file:///etc/passwd"),
              "file:// is refused -- libcurl speaks twenty protocols and a "
              "pasted string must not reach nineteen of them");
        check(!fetch_url_ok("ftp://example.com/x"), "ftp likewise");
        check(!fetch_url_ok("javascript:alert(1)"), "and a scheme that is not a transport");
        check(!fetch_url_ok("https://"), "a scheme with no host is not a url");
        check(!fetch_url_ok("https:///path"), "nor is an empty authority");
        check(!fetch_url_ok("https://user:pw@example.com/x"),
              "credentials in a listing url are refused -- a paste accident or "
              "somebody else's session, and neither goes on a bound handle");
        check(!fetch_url_ok("https://exa mple.com"), "a space is not part of a host");
        check(!fetch_url_ok("https://example.com/\x01"), "nor is a control character");
        check(!fetch_url_ok("https://" + std::string(3000, 'a')), "and there is a length past which we stop");

        std::printf("\nnet/Fetch -- the error vocabulary\n");
        const FetchError all[] = {
            FetchError::None, FetchError::NotBuilt, FetchError::BadUrl,
            FetchError::Refused, FetchError::DnsPinUnavailable,
            FetchError::BindFailed, FetchError::ProxyFailed, FetchError::Resolve,
            FetchError::Connect, FetchError::Tls, FetchError::Timeout,
            FetchError::TooLarge, FetchError::Protocol, FetchError::Other};
        bool named = true, spoken = true, distinct = true;
        for (std::size_t i = 0; i < sizeof all / sizeof all[0]; ++i) {
            const std::string n = fetch_error_name(all[i]);
            const std::string t = fetch_error_text(all[i]);
            if (n.empty()) named = false;
            if (t.size() < 12) spoken = false;
            for (std::size_t j = 0; j < i; ++j)
                if (n == fetch_error_name(all[j])) distinct = false;
        }
        check(named && distinct, "every error has its own log-safe name");
        check(spoken, "and a sentence a window can show");
        check(std::string(fetch_error_name(FetchError::None)) == "ok",
              "none is spelled ok in a log line");

        std::printf("\nnet/Fetch -- whose problem is it\n");
        check(fetch_error_is_ours(FetchError::Refused),
              "a refusal is ours: the policy said no, which says nothing "
              "about the listing");
        check(fetch_error_is_ours(FetchError::BindFailed),
              "a failed bind is ours: the killswitch fired");
        check(fetch_error_is_ours(FetchError::ProxyFailed),
              "a proxy that would not take the connection is ours");
        check(fetch_error_is_ours(FetchError::DnsPinUnavailable),
              "a libcurl that cannot pin is ours");
        check(fetch_error_is_ours(FetchError::NotBuilt),
              "a build with no transport in it is ours");
        check(!fetch_error_is_ours(FetchError::Timeout) &&
              !fetch_error_is_ours(FetchError::Connect) &&
              !fetch_error_is_ours(FetchError::Tls) &&
              !fetch_error_is_ours(FetchError::Resolve),
              "a timeout, a refused connection, a bad certificate and a name "
              "that will not resolve are NOT claimed as ours -- they are "
              "indistinguishable from a broker that is slow or blocking, and "
              "the honest reading of 'cannot tell' does not forgive the site");
        check(!fetch_error_is_ours(FetchError::TooLarge) &&
              !fetch_error_is_ours(FetchError::Protocol) &&
              !fetch_error_is_ours(FetchError::Other),
              "nor is an oversized page, an unreadable reply, or an "
              "unclassified failure");
        check(!fetch_error_is_ours(FetchError::BadUrl),
              "a url that will not parse is not an outage: retrying it "
              "tomorrow would be a lie about what needs fixing");
        check(!fetch_error_is_ours(FetchError::None),
              "and a successful fetch is nobody's problem");

        // The composition the check button makes: an OURS failure takes the
        // egress path, which leaves the failure streak where it was. Asserted
        // here rather than trusted, because the two halves live in different
        // libraries and nothing else pairs them.
        {
            Case k = mkcase("ours-1", "alpha");
            k.consecutive_failures = 2;
            k.last_verified = "2026-02-01";
            const Case after = apply_egress_refusal(k, "2026-03-01");
            check(after.outcome == Outcome::Indeterminate &&
                  after.reason == Reason::NoTunnel,
                  "our own failure records indeterminate/no-tunnel");
            check(after.consecutive_failures == 2,
                  "and does not touch the streak, so an expired subscription "
                  "cannot argue a case toward abandoned");
            check(after.next_check == "2026-03-02",
                  "and comes back tomorrow rather than in 45 days");
        }

        std::printf("\nnet/Fetch -- how the handle is tied to the tunnel\n");
        {
            EgressPolicy p = mkpolicy();          // Proxied, with a socks5h proxy
            EgressObservation o = mkobs();
            FetchBinding b = fetch_binding_from(p, o);
            check(b.bind_address == o.bound_address,
                  "the bind address is the one the observer reported, not the "
                  "interface name and not the policy's idea of it");
            check(b.proxy == p.proxy && b.dns_servers.empty(),
                  "proxied sends the proxy and pins no resolver");

            p.dns = DnsMode::Pinned;
            p.resolver = "10.7.0.1";
            p.proxy.clear();
            b = fetch_binding_from(p, o);
            check(b.dns_servers == "10.7.0.1" && b.proxy.empty(),
                  "and pinned does the opposite");

            p.dns = DnsMode::Unset;
            b = fetch_binding_from(p, o);
            check(b.proxy.empty() && b.dns_servers.empty(),
                  "an undecided policy configures neither -- the refusal comes "
                  "from egress_check, not from a half-built handle");
        }

        std::printf("\nnet/Fetch -- the gate is the function\n");
        {
            FetchRequest req;
            req.url = "https://example.com/listing/7";

            EgressPolicy p = mkpolicy();
            EgressObservation o;                  // default: nothing observed
            FetchResult r = fetch(req, p, o, 1000);
            check(r.error == FetchError::Refused,
                  "an empty observation refuses the fetch rather than trying it");
            check(r.verdict == Verdict::NoInterface,
                  "and carries the verdict, so the caller need not re-run the check");
            check(r.body.empty() && r.status == 0,
                  "a refusal produces no status and no body -- nothing happened");

            EgressPolicy bad;                     // unconfigured
            r = fetch(req, bad, mkobs(), 1000);
            check(r.error == FetchError::Refused && r.verdict == Verdict::Unconfigured,
                  "a policy problem refuses before any observation is consulted");

            p.dns = DnsMode::Proxied;
            p.proxy = "socks5://127.0.0.1:1080";   // the missing 'h'
            r = fetch(req, p, mkobs(), 1000);
            check(r.error == FetchError::Refused && r.verdict == Verdict::ProxyMissing,
                  "a socks5 proxy without the 'h' never reaches a socket -- it "
                  "would resolve the roster here and send it clean");

            req.url = "file:///etc/passwd";
            r = fetch(req, mkpolicy(), mkobs(), 1000);
            check(r.error == FetchError::Refused || r.error == FetchError::BadUrl,
                  "and a url we will not touch is refused either way");
        }

        std::printf("\nnet/Fetch -- there is no unbound path through the file\n");
        {
            FetchRequest req;
            req.url = "https://example.com/";
            FetchBinding nowhere;                  // no address
            const FetchResult r = fetch_unchecked(req, nowhere);
            check(r.error == FetchError::BindFailed || r.error == FetchError::NotBuilt,
                  "even the preflight's own call refuses without an address to "
                  "bind to -- the killswitch holds inside the preflight");

            FetchBinding b;
            b.bind_address = "127.0.0.1";
            req.url = "ftp://example.com/x";
            const FetchResult r2 = fetch_unchecked(req, b);
            check(r2.error == FetchError::BadUrl || r2.error == FetchError::NotBuilt,
                  "and the url guard runs before the handle is opened");
        }

        std::printf("\nnet/Observer -- the readings, without the network\n");
        {
            check(iface_read("").present == false,
                  "an unnamed interface reads as absent, not as an error");
            check(iface_read("delr_no_such_iface0").present == false,
                  "so does one that is not there");

            const IfaceReading lo = iface_read("lo");
            check(lo.present && lo.up && !lo.addresses.empty(),
                  "loopback is present, up, and has an address -- the one "
                  "interface every machine running this test has");

            check(bind_probe("").empty(), "no address, no bind");
            check(bind_probe("not-an-address").empty(), "nor from something unreadable");
            check(bind_probe("127.0.0.1") == "127.0.0.1",
                  "binding to loopback reports back what the kernel gave us");
            check(bind_probe("203.0.113.9").empty(),
                  "and binding to an address this machine does not have fails, "
                  "which is exactly what a dropped tunnel looks like");

            const auto names = iface_list();
            check(std::find(names.begin(), names.end(), "lo") != names.end(),
                  "the interface list finds loopback");
            check(std::adjacent_find(names.begin(), names.end()) == names.end(),
                  "and lists each interface once, however many families it has "
                  "-- a settings window showing 'lo, lo, eth0' is a settings "
                  "window nobody trusts");

            // The baseline is the one deliberately naked request in the
            // program, so its refusals are worth pinning down. All of these
            // return before a socket exists; nothing here opens one.
            // An ObserverConfig is built from the policy now, and a policy is
            // where the endpoints live. Built by hand it is EMPTY, which is the
            // direction everything in this subsystem fails in.
            check(ObserverConfig{}.echo_url.empty() && ObserverConfig{}.canary_url.empty(),
                  "an observer config built from nothing asks nothing");
            {
                EgressPolicy ep = mkpolicy();
                const ObserverConfig from_policy = observer_config(ep);
                check(from_policy.echo_url == ep.echo_url &&
                          from_policy.canary_url == ep.canary_url,
                      "and one built from a policy carries BOTH endpoints -- the "
                      "failure this exists to prevent is a caller that copies the "
                      "echo, forgets the canary, and turns the lookup check off "
                      "in one window and not the others");
            }

            ObserverConfig quiet = observer_config(mkpolicy());
            check(baseline_read("delr_no_such_iface0", quiet).address.empty(),
                  "no baseline from an interface that is not there");
            check(baseline_read("delr_no_such_iface0", quiet).resolver.empty(),
                  "and no resolver baseline either -- both halves ride the one "
                  "naked request and neither happens without an interface");
            check(!baseline_read("delr_no_such_iface0", quiet).notes.empty(),
                  "and it says so, in a sentence carrying no address");

            ObserverConfig nothing;   // both endpoints empty
            const BaselineResult none = baseline_read("lo", nothing);
            check(none.address.empty() && none.resolver.empty(),
                  "and none of either with nothing to ask");
            check(!none.notes.empty(), "which is said rather than returned silently");

            std::string text;
            check(routes_read(&text, tmp_path("delr_no_such_routes")) ==
                      ProbeReadings::RouteSource::Absent,
                  "a missing routing table is a kernel without v6, not a failure");

            const std::string rf = tmp_path("delr_selftest_routes");
            { std::ofstream o(rf); o << "00000000000000000000000000000000 00 "
                                        "00000000000000000000000000000000 00 "
                                        "00000000000000000000000000000000 "
                                        "00000400 00000000 00000000 00000003 wg0\n"; }
            text.clear();
            check(routes_read(&text, rf) == ProbeReadings::RouteSource::Text &&
                      !text.empty(),
                  "and a readable one comes back verbatim, for the core to judge");
            std::remove(rf.c_str());
        }
    }


    // ── where files live ─────────────────────────────────────────────────────
    // These functions read the environment, so the checks WRITE the
    // environment and put it back. That is why `paths` uses getenv rather than
    // g_get_user_data_dir: glib caches its answer on first call, and a check
    // that sets XDG_DATA_HOME would get the answer from before the check ran.
    {
        std::printf("\npaths (assets beside the program, state under XDG)\n");
        namespace P = delr::paths;

        struct Saved {
            const char* name; std::string val; bool had;
            explicit Saved(const char* n) : name(n) {
                const char* v = std::getenv(n);
                had = (v != nullptr);
                if (had) val = v;
            }
            ~Saved() {
                if (had) ::setenv(name, val.c_str(), 1); else ::unsetenv(name);
            }
        };
        Saved s_state("DELR_STATE"), s_xdg("XDG_DATA_HOME"), s_home("HOME"),
              s_cases("DELR_CASES"), s_egress("DELR_EGRESS"),
              s_roster("DELR_ROSTER"), s_rules("DELR_RULES"),
              s_assets("DELR_ASSETS");
        for (const char* n : {"DELR_STATE", "XDG_DATA_HOME", "HOME",
                              "DELR_CASES", "DELR_EGRESS", "DELR_ROSTER",
                              "DELR_RULES", "DELR_ASSETS"})
            ::unsetenv(n);

        // The refusal, first, because it is the interesting one. With no home
        // of any kind there is no fourth guess that is not worse than saying
        // so -- the working directory is the bug this module removed.
        check(P::state_dir().empty(),
              "with no DELR_STATE, no XDG_DATA_HOME and no HOME there is no state dir");
        check(P::cases_file().empty() && P::egress_file().empty(),
              "and the state files are empty rather than relative to nowhere");
        std::string derr;
        check(!P::ensure_state_dir(&derr) && !derr.empty(),
              "ensure refuses, with a sentence rather than a silent false");

        ::setenv("HOME", "/home/nobody", 1);
        check(P::state_dir() == "/home/nobody/.local/share/delr",
              "HOME alone gives the XDG default location");
        check(P::cases_file() == "/home/nobody/.local/share/delr/cases.json",
              "and the caseload sits inside it");

        ::setenv("XDG_DATA_HOME", "/xdg", 1);
        check(P::state_dir() == "/xdg/delr", "XDG_DATA_HOME outranks HOME");
        ::setenv("DELR_STATE", "/override", 1);
        check(P::state_dir() == "/override", "and DELR_STATE outranks both");

        // Set-but-empty is not an answer. `DELR_CASES=` in a shell profile
        // would otherwise resolve to the empty path and every save would fail
        // at open() with nothing to read about why.
        ::setenv("DELR_STATE", "", 1);
        check(P::state_dir() == "/xdg/delr",
              "an empty variable is unset, not an empty path");
        ::unsetenv("DELR_STATE");

        ::setenv("DELR_CASES", "/tmp/somewhere/else.json", 1);
        check(P::cases_file() == "/tmp/somewhere/else.json",
              "a per-file override wins outright");
        check(P::egress_file() == "/xdg/delr/egress.json",
              "and moves only its own file, not its neighbours");
        ::unsetenv("DELR_CASES");

        // Assets are the OTHER kind of file and do not follow state anywhere.
        // The roster and the rules are downloaded, identical on every machine,
        // and belong in git; putting them under XDG would mean a user's home
        // holding a copy of something the repo already ships.
        ::setenv("DELR_ASSETS", "/opt/delr/data", 1);
        check(P::roster_file() == "/opt/delr/data/brokers.json" &&
                  P::rules_file() == "/opt/delr/data/pagerules.json",
              "assets resolve against the asset dir, not the state dir");
        ::setenv("XDG_DATA_HOME", "/elsewhere", 1);
        check(P::roster_file() == "/opt/delr/data/brokers.json",
              "and moving the state dir does not move them");
        ::unsetenv("DELR_ASSETS");

        // ── the migration ────────────────────────────────────────────────────
        const std::string mdir = tmp_path("delr_selftest_paths");
        std::error_code mec;
        std::filesystem::remove_all(mdir, mec);
        std::filesystem::create_directories(mdir + "/old", mec);
        std::filesystem::create_directories(mdir + "/new", mec);
        const std::string from = mdir + "/old/egress.json";
        const std::string to   = mdir + "/new/egress.json";

        check(P::migrate_one(from, to) == P::Migration::NothingToDo,
              "nothing to migrate when the legacy file is not there");

        { std::ofstream o(from); o << "{\"version\":1}\n"; }
        std::filesystem::permissions(
            from,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, mec);
        check(P::migrate_one(from, to) == P::Migration::Moved,
              "a legacy file moves to the new location");
        check(!std::filesystem::exists(from),
              "and STOPS EXISTING at the old one -- a move that leaves a copy "
              "in the source tree has not fixed anything");
        check(std::filesystem::exists(to), "and is readable at the new one");
        // 0600 has to survive the move. Re-applying it afterwards would leave
        // a window in which naked_exit is on disk world-readable.
        const auto mperm = std::filesystem::status(to, mec).permissions();
        check((mperm & std::filesystem::perms::group_read) ==
                      std::filesystem::perms::none &&
                  (mperm & std::filesystem::perms::others_read) ==
                      std::filesystem::perms::none,
              "with its mode intact, so 0600 is never briefly not 0600");

        // Both present: the new one wins and the old one is LEFT. Deleting a
        // file the user might want is not recoverable, and this function does
        // not get to make that call quietly.
        { std::ofstream o(from); o << "{\"version\":1,\"older\":true}\n"; }
        check(P::migrate_one(from, to) == P::Migration::KeptTarget,
              "when both exist the new location wins");
        check(std::filesystem::exists(from),
              "and the older copy is left rather than deleted behind the user's back");
        std::filesystem::remove_all(mdir, mec);
    }


    // ── the registry importer ────────────────────────────────────────────────
    // A fixture rather than the real 337KB export: hermetic, and it can hold
    // the awkward shapes on purpose. Every oddity below was found in the 2025
    // file, including the non-breaking space in the first header.
    {
        std::printf("\ncsv parsing (as encountered, not as specified)\n");

        const std::string bom = "\xEF\xBB\xBF";
        const std::string fixture =
            bom + ",annotation band the agency writes for its own website,,,,\r\n"
            "Data broker name:,Data broker primary website:,"
            "Data broker primary contact email address:,"
            "Data Broker's primary website that contains details on how consumers can "
            "exercise their CA Consumer Privacy Act rights:,"
            "The data broker or any of its subsidiaries is regulated by the federal Fair "
            "Credit Reporting Act (FCRA):,"
            "The data broker collects consumers\xE2\x80\x99 precise geolocation: \r\n"
            "Alpha People Search,https://www.alpha.example,privacy@alpha.example,"
            "https://alpha.example/rights,No,Yes\r\n"
            "\"Beta, Inc. and its affiliates\",\"https://www.beta.example;\n"
            "https://www.betalookup.example; \nhttps://www.betaphone.example\","
            "privacy@beta.example,https://beta.example/rights,Yes,No\r\n"
            "Beta Screening LLC,https://www.beta.example,screening@beta.example,"
            "https://beta.example/rights,Yes,No\r\n"
            "Gamma Data,https://www.gamma.example,\"legal@gamma.example; "
            "second@gamma.example\",https://gamma.example/rights,No,No\r\n"
            "\"Delta \"\"The\"\" Co\",https://www.delta.example,privacy@delta.example,"
            "https://delta.example/rights,No,No";

        const CsvTable t = csv_parse(fixture);
        check(t.size() == 7, "every row is seen, including the last with no newline");
        check(t[1][0] == "Data broker name:",
              "the BOM is stripped rather than becoming part of the first header");
        check(t[3][1].find("betalookup") != std::string::npos,
              "a newline inside quotes stays inside the field");
        check(t[3][0] == "Beta, Inc. and its affiliates",
              "a comma inside quotes is not a column break");
        check(t[6][0] == "Delta \"The\" Co", "a doubled quote is one literal quote");

        std::printf("\nheader matching (text, never column index)\n");
        // The header the real file ships has a UTF-8 non-breaking space inside
        // it -- invisible everywhere, and it made the importer refuse the
        // state's own export until squash() started treating any non-ASCII
        // byte as a gap. This is that bug, pinned.
        std::string nbsp_fixture = fixture;
        const std::size_t at = nbsp_fixture.find("Data broker name:");
        nbsp_fixture.replace(at, 17, "Data broker\xC2\xA0name:");
        ImportReport nrep;
        const Roster nr = registry_parse(nbsp_fixture, nrep);
        check(nrep.failure.empty() && nr.size() == 5,
              "a non-breaking space inside a header does not hide the column");

        ImportReport srep;
        registry_parse("no,header,here\n1,2,3\n", srep);
        check(!srep.failure.empty(),
              "a file that is not the registry is refused, not half-read");

        std::printf("\nregistry rows become brokers\n");
        ImportReport rep;
        const Roster in = registry_parse(fixture, rep);
        check(rep.failure.empty(), "the fixture parses");
        check(rep.rows_read == 5 && in.size() == 5, "one entry per registrant");
        check(rep.rows_skipped == 0, "no registrant is dropped");

        const Broker* beta = nullptr;
        for (const auto& b : in) if (b.name.rfind("Beta,", 0) == 0) beta = &b;
        check(beta != nullptr && beta->hosts.size() == 3,
              "a registrant filing three domains in one cell gets three hosts");
        check(rep.multi_host_rows == 1, "and is counted as publishing under more than one");
        check(beta != nullptr && beta->site == "https://www.beta.example",
              "the first filed URL is the display homepage");

        // The sibling filer. Beta Screening is a separate company at Beta's
        // domain, and dropping it would delete a registrant that receives its
        // own requests at its own address.
        const Broker* screen = nullptr;
        for (const auto& b : in) if (b.name.rfind("Beta Screening", 0) == 0) screen = &b;
        check(screen != nullptr, "a registrant whose only domain is taken is still listed");
        check(screen != nullptr && screen->hosts.empty(),
              "with no host of its own, so the first filer keeps the match");
        check(rep.shared_host_rows == 1, "and the sharing is reported rather than silent");
        check(screen != nullptr && screen->opt_out_email == "screening@beta.example",
              "and remains contactable at its own address");

        const Broker* gamma = nullptr;
        for (const auto& b : in) if (b.name == "Gamma Data") gamma = &b;
        check(gamma != nullptr && gamma->opt_out_email == "legal@gamma.example",
              "two addresses in one cell yields the first, not a string with a "
              "semicolon in it");

        check(in[0].method == Method::Email,
              "method is email: it is the channel that works from any state");
        check(!in[0].opt_out_url.empty(), "and the rights URL rides along regardless");
        check(in[0].ca_registered, "every row of this file is a CA registrant");
        check(in[0].collects_geo && !in[0].fcra_regulated,
              "the two declared flags are read from their own columns");
        check(beta != nullptr && beta->fcra_regulated,
              "the FCRA declaration is read, not one of the four follow-up columns");
        check(roster_validate(in).empty(), "the imported roster validates");

        std::printf("\nids (stable across a refresh, or cases orphan)\n");
        check(in[0].id == "alpha", "an id is the domain stem, not the TLD");
        check(screen != nullptr && !screen->id.empty() && screen->id != beta->id,
              "two registrants at one domain do not collide on one id");
        std::vector<std::string> taken{"acme"};
        check(mint_id("acme.com", "Acme Two LLC", taken) == "acme-acme-two-llc",
              "a collision is broken by the registrant's name, which reads in a log");
        taken.push_back("acme-acme-two-llc");
        check(mint_id("acme.com", "Acme Two LLC", taken) == "acme-2",
              "and by an ordinal when the names are identical too");

        std::printf("\nsplitting a cell full of URLs\n");
        check(split_urls("https://a.example; \nhttps://b.example").size() == 2,
              "semicolons and newlines both separate");
        check(split_urls("https://a.example\nhttps://b.example").size() == 2,
              "and so does a bare newline with no punctuation");
        check(split_urls("   ").empty(), "an empty cell yields nothing");
        check(import_host_of("https://www.Alpha.example/x") == "alpha.example",
              "hosts are minted in the SAME normal form the matcher compares");

        std::printf("\nmerge (the second run is the one that can do damage)\n");
        Roster hand;
        Broker drop;
        drop.id = "calprivacy-drop";
        drop.name = "California DROP";
        drop.site = "https://privacy.ca.gov/data-brokers/";
        drop.method = Method::Drop;
        drop.notes = "hand written";
        hand.push_back(drop);

        ImportReport m1;
        const Roster first = roster_merge(hand, in, m1);
        check(first.size() == 6, "the hand-written entry and every registrant survive");
        check(m1.merged_new == 5 && m1.merged_kept == 1,
              "an entry this registry does not mention is KEPT, never deleted");

        ImportReport m2;
        const Roster second = roster_merge(first, in, m2);
        check(m2.merged_new == 0 && m2.merged_updated == 5,
              "re-importing the same file adds nothing -- it updates in place");
        check(m2.merged_renamed == 0, "and renames nothing");
        check(second.size() == first.size(), "so the roster does not grow every year");

        bool ids_held = second.size() == first.size();
        for (std::size_t i = 0; ids_held && i < first.size(); ++i)
            ids_held = first[i].id == second[i].id;
        check(ids_held,
              "every id survives the refresh -- a re-minted id orphans every case "
              "filed against that broker");

        // The rename. A registrant filing under a new legal name is the exact
        // case that must NOT mint a new id.
        Roster renamed = in;
        renamed[0].name = "Alpha People Search LLC";
        ImportReport m3;
        const Roster after = roster_merge(first, renamed, m3);
        check(m3.merged_renamed == 1 && m3.merged_new == 0,
              "a renamed registrant is matched by host, not by name");
        const Broker* still = roster_find(after, "alpha");
        check(still != nullptr && still->name == "Alpha People Search LLC",
              "so the id holds while the name updates");

        // Human fields are the human's. The state does not know them and a
        // refresh must not blank them.
        Roster annotated = first;
        for (auto& b : annotated)
            if (b.id == "alpha") { b.notes = "wants a photo ID"; b.requires_id = true;
                                   b.recheck_days = 90; }
        ImportReport m4;
        const Roster kept = roster_merge(annotated, in, m4);
        const Broker* a2 = roster_find(kept, "alpha");
        check(a2 != nullptr && a2->notes == "wants a photo ID" && a2->requires_id &&
                  a2->recheck_days == 90,
              "notes, requires_id and recheck_days survive a refresh untouched");

        // A roster written before Broker::hosts existed has everything in
        // `site`. It must be adopted by its registry row rather than duplicated.
        Roster legacy;
        Broker old;
        old.id = "alpha-legacy";
        old.name = "Alpha";
        old.site = "https://alpha.example";
        old.method = Method::Web;
        old.opt_out_url = "https://alpha.example/optout";
        legacy.push_back(old);
        ImportReport m5;
        const Roster upgraded = roster_merge(legacy, in, m5);
        check(m5.merged_new == 4 && m5.merged_updated == 1,
              "a pre-hosts entry is adopted by its registry row, not duplicated");
        const Broker* up = roster_find(upgraded, "alpha-legacy");
        check(up != nullptr && up->hosts.size() == 1,
              "and gains the host list it never had, keeping its own id");

        std::printf("\nmatching, which is what the host list is FOR\n");
        const Roster r = first;
        const Broker* hit = broker_for_url(r, "https://www.betalookup.example/John-Smith/TN");
        check(hit != nullptr && hit->name.rfind("Beta,", 0) == 0,
              "a listing on a registrant's OTHER domain matches that registrant");
        check(broker_for_url(r, "https://search.betaphone.example/x") != nullptr,
              "and so does a subdomain of one");
        check(broker_for_url(r, "https://notbeta.example/x") == nullptr,
              "while a host that merely ends the same way does not");

        std::printf("\nvalidation guards the lookup table\n");
        Roster dup = in;
        dup[0].hosts.push_back(dup[1].hosts.front());
        check(!roster_validate(dup).empty(),
              "two entries claiming one host is caught -- otherwise the match is "
              "decided by roster order");
        Roster badhost = in;
        badhost[0].hosts.push_back("https://alpha.example/x");
        check(!roster_validate(badhost).empty(),
              "a URL where a bare hostname belongs is caught, not left to never match");
    }

    // ── core/Profile -- table four ───────────────────────────────────────────
    {
        std::printf("\ncore/Profile -- parsing what the user typed\n");

        // One term per line, and everything the surface would otherwise have
        // had an opinion about happens HERE.
        const std::vector<std::string> t = terms_parse(
            "  John Smith \n\njohn  smith\nJOHN SMITH\nJane Smith\n   \n");
        check(t.size() == 2, "blank lines and duplicates collapse; two terms survive");
        check(t.front() == "John Smith",
              "the first spelling is the one kept -- order is the user's");
        check(terms_parse("a\tb\n  c   d  ") ==
                  (std::vector<std::string>{"a b", "c d"}),
              "inner whitespace collapses, so a term pasted from a web page "
              "compares equal to the same term typed");
        check(terms_join(t) == "John Smith\nJane Smith",
              "and the join is the inverse, so the box repaints what it parsed");
        check(terms_parse(terms_join(t)) == t, "round trip");

        std::printf("\nphones become the forms a page PRINTS\n");
        check(phone_digits("(615) 555-0100") == "6155550100", "punctuation is not a phone number");
        check(phone_digits("+1 615 555 0100") == "6155550100", "nor is a country code");
        check(phone_digits("555-0100") == "5550100", "seven digits is still a phone");
        check(phone_digits("12") .empty(), "two is not");
        const std::vector<std::string> v = phone_variants("6155550100");
        auto has = [&](const std::string& s) {
            return std::find(v.begin(), v.end(), s) != v.end();
        };
        check(has("6155550100") && has("615-555-0100") && has("(615) 555-0100"),
              "one number yields the bare, dashed and parenthesised forms");
        check(v.size() >= 6, "and the rest of the closed set with them");
        check(phone_variants("").empty(), "nothing in, nothing out");

        std::printf("\nwhat may be used to confirm a page is yours\n");
        check(needle_usable("Ng"), "a two-letter surname is a real name and a real needle");
        check(!needle_usable("J"), "a middle initial matches the whole internet");
        check(!needle_usable("1984"), "and so does a four-digit year -- every copyright line");
        check(needle_usable("37211"), "a zip is the shortest number that is about a person");
        check(!needle_usable("  "), "whitespace is not a term");

        Profile p;
        p.full_name = "John Smith";
        p.also_known_as = {"Jack Smith"};
        p.emails = {"john@example.com", "jsmith@work.example.com"};
        p.contact_email = "john@example.com";
        p.phones = {"(615) 555-0100"};
        p.usernames = {"jsmith77"};
        p.places = {"Nashville, TN"};
        p.birth_year = 1984;

        const PageNeedles n = needles_for(p);
        auto needled = [&](const std::string& s) {
            return std::find(n.terms.begin(), n.terms.end(), s) != n.terms.end();
        };
        check(needled("John Smith") && needled("Jack Smith") && needled("jsmith77")
                  && needled("Nashville, TN"),
              "names, aliases, usernames and places all become needles");
        check(needled("615-555-0100"),
              "and a phone arrives as the string a page would print");
        check(!needled("john@example.com"),
              "an email does NOT -- brokers mask them, so the needle could not "
              "match a page that really is yours");
        check(!needled("1984"),
              "and neither does a birth year: it disambiguates for a reader, "
              "not for a substring matcher");
        check(!n.require_all,
              "ANY, because a listing that omits your city is still your listing");

        std::printf("\nthe needles reach page_check, which is the point of all this\n");
        PageRule r;
        r.broker_id = "beta";
        r.fingerprint = {"Beta Lookup"};
        r.present     = {"View full report"};
        r.absent      = {"No results found"};
        r.needs_needle    = true;
        const std::string mine =
            "<html><body>Beta Lookup -- View full report for John Smith of "
            "Nashville, TN</body></html>";
        const std::string other =
            "<html><body>Beta Lookup -- View full report for Alice Jones of "
            "Portland, OR</body></html>";
        check(page_check(&r, 200, mine, n) == PageVerdict::Present,
              "a page carrying the profile's name confirms the listing");
        check(page_check(&r, 200, other, n) == PageVerdict::NeedleAbsent,
              "and one carrying somebody else's does not");
        check(page_check(&r, 200, mine, needles_for(Profile{}))
                  == PageVerdict::NoNeedles,
              "an EMPTY profile still yields NoNeedles -- which is what every "
              "check has done since s8, and is why this table exists");
        const std::string phoned =
            "<html><body>Beta Lookup -- View full report -- (615) 555-0100"
            "</body></html>";
        check(page_check(&r, 200, phoned, n) == PageVerdict::Present,
              "a page that prints only the phone number, formatted, still matches");

        std::printf("\nvalidation, and the invariant that keeps filing honest\n");
        check(profile_validate(p).empty(), "a well-formed profile has nothing to say");
        Profile bad = p;
        bad.contact_email = "elsewhere@example.com";
        check(!profile_validate(bad).empty(),
              "an opt-out cannot be filed from an address the profile never listed");
        bad = p; bad.emails.push_back("not an address");
        check(!profile_validate(bad).empty(), "an email without an @ is caught");
        bad = p; bad.birth_year = 84;
        check(!profile_validate(bad).empty(), "and a two-digit year");
        bad = p; bad.phones.push_back("abc");
        check(!profile_validate(bad).empty(), "and a phone with no digits in it");
        bad = p; bad.places.push_back("nashville, tn");
        check(!profile_validate(bad).empty(),
              "a hand-edited file listing one place twice is reported");

        std::printf("\nnothing about a profile reaches a log\n");
        const std::string ref = profile_log_ref(p);
        bool leaked = false;
        for (const char* pii : {"John", "Smith", "jsmith77", "Nashville",
                                "example.com", "555", "1984"})
            if (ref.find(pii) != std::string::npos) leaked = true;
        check(!leaked, "log_ref carries counts and not one character of the person");
        check(ref.find("needles") != std::string::npos,
              "while still saying enough to debug with");
        const std::string sum = profile_summary(p);
        check(sum.find("Smith") == std::string::npos,
              "and the window's summary line is counts too");
        check(profile_summary(Profile{}).find("No profile") != std::string::npos,
              "an empty profile says so plainly rather than showing a zero");

        std::printf("\nthe pump\n");
        check(profile_is_empty(Profile{}), "a default profile is empty");
        check(!profile_is_empty(p), "and a filled one is not");
        const std::string f = tmp_path("delr_profile_test.json");
        std::filesystem::remove(f);
        std::string err = "x";
        const Profile missing = profile_load(f, &err);
        check(profile_is_empty(missing) && err.empty(),
              "a missing file is a first run, not an error");
        check(profile_save(f, p), "save writes");
        const Profile back = profile_load(f, &err);
        check(err.empty() && back.full_name == p.full_name
                  && back.emails == p.emails && back.phones == p.phones
                  && back.places == p.places && back.usernames == p.usernames
                  && back.also_known_as == p.also_known_as
                  && back.birth_year == p.birth_year
                  && back.contact_email == p.contact_email,
              "and every field comes back the way it went in");
        check(needles_for(back).terms == n.terms,
              "so the needles a reload derives are the needles the write had");
        {
            // The one property that matters more than the content: this file
            // holds the person, and it may not be readable by anyone else.
            const auto mode = std::filesystem::status(f).permissions();
            check((mode & (std::filesystem::perms::group_all
                           | std::filesystem::perms::others_all))
                      == std::filesystem::perms::none,
                  "and the file is 0600 -- no group, no other");
        }
        {
            std::ofstream junk(f, std::ios::trunc);
            junk << "{ not json";
        }
        err.clear();
        check(profile_is_empty(profile_load(f, &err)) && !err.empty(),
              "malformed JSON yields an empty profile AND says why");
        std::filesystem::remove(f);

        std::printf("\npaths\n");
        check(delr::paths::profile_file().find("profile.json") != std::string::npos,
              "the profile lives beside the caseload, under state");
        check(delr::paths::profile_file() != delr::paths::cases_file(),
              "and not on top of it");
    }

    // ── core/Journal -- table five: what this program DID, and when ──────────
    {
        std::printf("\n\ncore/Journal -- the run history the case could not hold\n");

        check(std::string(kind_name(Kind::Checked)) == "checked"
                  && kind_from("checked") == Kind::Checked
                  && kind_from("opened") == Kind::Opened
                  && kind_from("declined") == Kind::Declined
                  && kind_from("changed") == Kind::Changed,
              "every kind round-trips through its name");
        check(kind_from("filed") == Kind::Other,
              "and a kind from a newer delr lands as Other rather than nowhere");

        std::printf("\nthe builders read the case, not a second opinion\n");
        {
            Case k = mkcase("c1", "spokeo");
            const Entry o = entry_opened(k, "2026-02-01");
            check(o.kind == Kind::Opened && o.case_id == "c1"
                      && o.broker_id == "spokeo" && o.date == "2026-02-01",
                  "opening a case records the case and the day");
            check(o.outcome == Outcome::Never && o.reason == Reason::None,
                  "and claims nothing about a check that has not happened");

            k = apply_check(k, Outcome::Indeterminate, Reason::EgressBlocked,
                            "2026-03-01", 3);
            const Entry c = entry_checked(k, "2026-03-01");
            check(c.kind == Kind::Checked && c.outcome == Outcome::Indeterminate
                      && c.reason == Reason::EgressBlocked,
                  "a checked entry carries the outcome the case just took");

            const Entry d = entry_declined(k, Reason::NoTunnel, "2026-03-02");
            check(d.kind == Kind::Declined && d.reason == Reason::NoTunnel,
                  "a decline carries why nothing left this machine");
            check(d.outcome == Outcome::Never,
                  "and NO outcome -- a decline is not a failed look, it is no look");

            Case r = k;
            r.provenance = Provenance::SelfVerified;
            const Entry ch = entry_changed(r, Status::Found, Status::Removed,
                                           "2026-04-01");
            check(ch.kind == Kind::Changed && ch.from == Status::Found
                      && ch.to == Status::Removed
                      && ch.provenance == Provenance::SelfVerified,
                  "a change carries the transition AND who says so");
            const Entry rel = entry_changed(r, Status::Removed, Status::Relisted,
                                            "2026-05-01", "c1-2");
            check(rel.other_id == "c1-2",
                  "and a relist points at its successor -- one event, two rows");
        }

        std::printf("\nnothing about an entry reaches a log\n");
        {
            Entry e = entry_checked(mkcase("c1", "spokeo"), "2026-03-01");
            e.seq = 7;
            const std::string ref = log_ref(e);
            check(ref == "entry:7/checked@c1", "the ref is seq, kind and case id");
            check(ref.find("spokeo") == std::string::npos
                      && ref.find("2026") == std::string::npos
                      && ref.find("https") == std::string::npos,
                  "and carries no broker, no date and no url");
        }

        std::printf("\nthe queries a snapshot cannot answer\n");
        {
            Journal j;
            std::int64_t n = 1;
            const auto push = [&](Entry e) { e.seq = n++; j.push_back(e); };

            Case a = mkcase("a", "spokeo");
            push(entry_opened(a, "2026-01-01"));
            a = apply_check(a, Outcome::Listed, Reason::None, "2026-01-10", 45);
            push(entry_checked(a, "2026-01-10"));
            a = apply_check(a, Outcome::Indeterminate, Reason::EgressBlocked,
                            "2026-02-01", 3);
            push(entry_checked(a, "2026-02-01"));
            push(entry_declined(a, Reason::NoTunnel, "2026-02-05"));
            a = apply_check(a, Outcome::Indeterminate, Reason::EgressBlocked,
                            "2026-02-20", 3);
            push(entry_checked(a, "2026-02-20"));

            Case b = mkcase("b", "acxiom");
            push(entry_opened(b, "2026-01-03"));

            check(journal_for_case(j, "a").size() == 5,
                  "a case's own history comes back, and only its own");
            check(journal_for_case(j, "nobody").empty(),
                  "and an unknown case has no history rather than everyone's");
            check(journal_for_case(j, "").empty(),
                  "an empty id matches nothing -- it is not a wildcard");

            check(journal_last(j, "a", Kind::Checked)->date == "2026-02-20",
                  "the last check is the last one, not the first");
            check(journal_first(j, "a", Kind::Checked)->date == "2026-01-10",
                  "and the first is the first -- the one a deadline runs from");
            check(journal_last(j, "b", Kind::Checked) == nullptr,
                  "a case that has never been checked says so with a null");
            check(journal_last(j, "a", Kind::Other) == nullptr,
                  "the last entry of a kind we do not understand is not a question");

            check(journal_since(j, "2026-02-01").size() == 3,
                  "since a date means on or after it");
            check(journal_since(j, "not-a-date").empty(),
                  "and a malformed filter yields NOTHING, never everything");

            std::printf("\nhow long has this broker been refusing us\n");
            check(journal_walled_since(j, "a") == "2026-02-01",
                  "the current run of walls starts where it started");
            check(journal_walled_since(j, "b").empty(),
                  "a case that was never walled has no such date");

            // The decline sat in the middle of the run. Our own tunnel being
            // down is not evidence about the broker, so it neither broke the
            // run nor started it -- the same reason apply_egress_refusal
            // leaves the failure streak alone.
            check(journal_walled_since(j, "a") != "2026-02-05",
                  "and our own outage does not restart the broker's clock");

            {
                Journal k = j;
                Case c = mkcase("a", "spokeo");
                c = apply_check(c, Outcome::NotFound, Reason::None, "2026-03-01", 45);
                Entry clean = entry_checked(c, "2026-03-01");
                clean.seq = 99;
                k.push_back(clean);
                check(journal_walled_since(k, "a").empty(),
                      "a clean look ends the run -- the wall is over, not older");

                Case d = mkcase("a", "spokeo");
                d = apply_check(d, Outcome::Indeterminate, Reason::Blocked,
                                "2026-03-10", 3);
                Entry again = entry_checked(d, "2026-03-10");
                again.seq = 100;
                k.push_back(again);
                check(journal_walled_since(k, "a") == "2026-03-10",
                      "and a new wall after it is a NEW run, not the old one resumed");
            }

            check(reason_is_wall(Reason::Blocked) && reason_is_wall(Reason::EgressBlocked)
                      && reason_is_wall(Reason::ClientBlocked)
                      && reason_is_wall(Reason::NoListingPage),
                  "all four walls count as walls, unattributed included");
            check(!reason_is_wall(Reason::NoRule) && !reason_is_wall(Reason::NoTunnel)
                      && !reason_is_wall(Reason::PageUnreadable),
                  "and none of our own bugs is allowed to look like one");

            std::printf("\nthe honest denominator\n");
            const Tally t = journal_tally(j, "a");
            check(t.checked == 3, "three fetches ran");
            check(t.declined == 1, "one act sent nothing");
            check(t.clean == 1, "and only one of them actually saw the page");
            check(t.walled == 2, "two were the far end refusing");
            check(journal_tally(j).checked == 3,
                  "an empty id tallies the whole journal rather than nothing");
        }

        std::printf("\nvalidation, after the fact and never instead of it\n");
        {
            Journal j;
            Case k = mkcase("a", "spokeo");
            Entry e1 = entry_opened(k, "2026-01-01"); e1.seq = 1;
            k = apply_check(k, Outcome::Listed, Reason::None, "2026-01-10", 45);
            Entry e2 = entry_checked(k, "2026-01-10"); e2.seq = 2;
            j = {e1, e2};
            check(journal_validate(j).empty(), "a well-formed journal has nothing to say");

            Journal back = j; back[1].seq = 1;
            check(!journal_validate(back).empty(), "a seq that does not increase is caught");

            Journal dated = j; dated[1].date = "2025-12-01";
            check(!journal_validate(dated).empty(), "and a date that goes backwards");

            Journal noreason = j;
            noreason[1].outcome = Outcome::Indeterminate;
            noreason[1].reason  = Reason::None;
            check(!journal_validate(noreason).empty(),
                  "an indeterminate without a reason -- the same invariant Case guards");

            Journal spurious = j; spurious[1].reason = Reason::Blocked;
            check(!journal_validate(spurious).empty(),
                  "and a reason on an outcome that was not indeterminate");

            Journal nodecl = j;
            nodecl[1].kind = Kind::Declined;
            nodecl[1].outcome = Outcome::Never;
            nodecl[1].reason = Reason::None;
            check(!journal_validate(nodecl).empty(), "a decline with no reason is caught");

            Journal nomove = j;
            nomove[1].kind = Kind::Changed;
            nomove[1].outcome = Outcome::Never;
            nomove[1].from = Status::Found;
            nomove[1].to   = Status::Found;
            check(!journal_validate(nomove).empty(),
                  "and a change to what it already was is not a change");

            Journal orphan = j; orphan[1].case_id.clear();
            check(!journal_validate(orphan).empty(), "an entry about no case is caught");
        }

        std::printf("\nthe pump -- append-only, and line-tolerant on purpose\n");
        {
            const std::string f = tmp_path("delr_journal_test.ndjson");
            std::filesystem::remove(f);

            std::string err;
            int skipped = 0;
            check(journal_load(f, &err, &skipped).empty() && err.empty(),
                  "a missing file is a first run, not an error");

            Case k = mkcase("a", "spokeo");
            Entry e1 = entry_opened(k, "2026-01-01");
            check(journal_record(f, e1) && e1.seq == 1,
                  "the first entry lands and takes seq 1");

            k = apply_check(k, Outcome::Indeterminate, Reason::ClientBlocked,
                            "2026-01-10", 3);
            Entry e2 = entry_checked(k, "2026-01-10");
            check(journal_record(f, e2) && e2.seq == 2,
                  "and the next one takes the next seq without being told");

            Journal j = journal_load(f, &err, &skipped);
            check(err.empty() && skipped == 0 && j.size() == 2,
                  "both come back, in order, with nothing skipped");
            check(j[0].kind == Kind::Opened && j[1].kind == Kind::Checked
                      && j[1].reason == Reason::ClientBlocked
                      && j[1].broker_id == "spokeo" && j[1].date == "2026-01-10",
                  "and every field survives the round trip");
            check(journal_validate(j).empty(), "what the pump wrote validates");

            {
                // The property the whole file format was chosen for.
                std::ifstream in(f);
                std::string l1, l2;
                std::getline(in, l1); std::getline(in, l2);
                check(!l1.empty() && !l2.empty()
                          && l1.find('\n') == std::string::npos,
                      "one entry per line -- an appended row, not a rewritten file");
            }
            {
                const auto mode = std::filesystem::status(f).permissions();
                check((mode & (std::filesystem::perms::group_all
                               | std::filesystem::perms::others_all))
                          == std::filesystem::perms::none,
                      "and the file is 0600 -- no group, no other");
            }

            std::printf("\na torn tail costs one line, never the file\n");
            {
                std::ofstream tear(f, std::ios::app);
                tear << "{\"seq\": 3, \"kind\": \"chec";   // killed mid-write
            }
            skipped = 0; err.clear();
            Journal torn = journal_load(f, &err, &skipped);
            check(torn.size() == 2 && skipped == 1 && err.empty(),
                  "the half-written line is skipped and the good ones survive");

            Case k3 = mkcase("a", "spokeo");
            Entry e3 = entry_declined(k3, Reason::NoTunnel, "2026-01-20");
            check(journal_record(f, e3) && e3.seq == 3,
                  "the next append still lands on top of the damage");
            skipped = 0;
            Journal healed = journal_load(f, &err, &skipped);
            check(healed.size() == 3 && skipped == 1,
                  "and it healed the tear into ONE bad line rather than fusing to it");
            check(healed[2].kind == Kind::Declined && healed[2].reason == Reason::NoTunnel,
                  "the entry written after the damage is intact");

            std::printf("\nan entry from a newer delr survives an older binary\n");
            {
                const std::string g = tmp_path("delr_journal_fwd.ndjson");
                std::filesystem::remove(g);
                {
                    std::ofstream fut(g);
                    fut << R"({"seq":1,"date":"2026-06-01","case_id":"a",)"
                           R"("broker_id":"spokeo","kind":"filed","outcome":"never",)"
                           R"("reason":"none","from":"unknown","to":"unknown",)"
                           R"("provenance":"none","other_id":""})" << "\n";
                }
                Journal fwd = journal_load(g);
                check(fwd.size() == 1 && fwd[0].kind == Kind::Other,
                      "a kind we have never heard of decodes as Other");
                check(fwd[0].kind_raw == "filed",
                      "with its label kept -- the row is not flattened");
                check(fwd[0].case_id == "a" && fwd[0].date == "2026-06-01",
                      "and everything we DO understand about it is still there");

                const std::string h = tmp_path("delr_journal_fwd2.ndjson");
                std::filesystem::remove(h);
                Entry pass = fwd[0];
                check(journal_append(h, pass, 1), "it can be written back out");
                Journal again = journal_load(h);
                check(again.size() == 1 && again[0].kind_raw == "filed",
                      "and comes back as 'filed', not as 'other' -- proof is not destroyed");
                std::filesystem::remove(g);
                std::filesystem::remove(h);
            }

            std::printf("\nthe append never refuses on content\n");
            {
                // Deliberately invalid: no case, no reason, backwards date. It
                // still lands. The moment an event happens is the worst
                // possible moment to decide it was not worth recording.
                Entry bad;
                bad.kind = Kind::Checked;
                bad.date = "1999-01-01";
                bad.outcome = Outcome::Indeterminate;
                check(journal_record(f, bad), "a malformed entry is still recorded");
                Journal with_bad = journal_load(f);
                check(!journal_validate(with_bad).empty(),
                      "and validation is what notices, AFTER it is safely on disk");
            }

            std::filesystem::remove(f);
        }

        std::printf("\npaths\n");
        check(delr::paths::journal_file().find("journal.ndjson") != std::string::npos,
              "the journal lives under state with the caseload");
        check(delr::paths::journal_file() != delr::paths::cases_file()
                  && delr::paths::journal_file() != delr::paths::profile_file(),
              "and on top of nothing");
    }

    std::printf("\n%d pass / %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

}  // namespace delr::selftest
