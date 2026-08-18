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
#include "core/Intake.hpp"

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

    std::printf("\n%d pass / %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

}  // namespace delr::selftest
