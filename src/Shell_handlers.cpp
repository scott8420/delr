// Handlers -- slot bodies. What a user action actually does.
#include "Shell.hpp"
#include "Registry.hpp"
#include "Log.hpp"
#include "netcheck.hpp"

#include <glibmm/miscutils.h>
#include <glibmm/datetime.h>
#include <gtkmm/label.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace delr {

// UI-side path resolution: DELR_ROSTER wins, else ./data/brokers.json beside
// the working directory. The core never learns where the file came from.
std::string Shell::roster_file() const {
    if (const char* env = std::getenv("DELR_ROSTER")) return env;
    return Glib::build_filename(Glib::get_current_dir(), "data", "brokers.json");
}

void Shell::on_reload_roster() {
    const std::string file = roster_file();
    std::string err;
    m_roster = core::roster_load(file, &err);

    auto lg = log::get(log::Area::Roster);
    if (lg) {
        if (!err.empty()) lg->error("roster parse failed: {}", err);
        else              lg->info("roster loaded: {} broker(s)", m_roster.size());
    }

    // Validation is loud by design -- a bad roster is a bug in the DATA, and
    // the data is the asset (CANON: dead mapping entries lie about structure).
    const auto problems = core::roster_validate(m_roster);
    for (const auto& p : problems) if (lg) lg->warn("roster: {}", p);

    // Repaint the list.
    while (auto* row = m_roster_list.get_row_at_index(0)) m_roster_list.remove(*row);
    for (const auto& b : m_roster) {
        auto* row = Gtk::make_managed<Gtk::Label>(
            b.name + "   [" + core::method_name(b.method) + "]" +
            (b.requires_id ? "  (photo ID)" : "") +
            (b.ca_registered ? "  (CA registered)" : ""));
        row->set_xalign(0.0f);
        row->set_margin(6);
        m_roster_list.append(*row);
    }

    if (!err.empty())
        m_roster_status.set_text("Roster failed to parse: " + err);
    else
        m_roster_status.set_text(
            std::to_string(m_roster.size()) + " broker(s) from " + file +
            (problems.empty() ? "" : "  --  " + std::to_string(problems.size()) +
                                     " validation problem(s), see log"));
}

// The caseload's path. Same reasoning as the roster's, different table -- and
// unlike the roster this file is PII, so when encryption-at-rest lands it lands
// behind core::caseload_load and this line does not change.
std::string Shell::cases_file() const {
    if (const char* env = std::getenv("DELR_CASES")) return env;
    return Glib::build_filename(Glib::get_current_dir(), "data", "cases.json");
}

// Table three's path. Same shape as the two above; its own env override
// because it is its own file, and it is its own file because it rots on a
// designer's schedule rather than a company's.
std::string Shell::rules_file() const {
    if (const char* env = std::getenv("DELR_RULES")) return env;
    return Glib::build_filename(Glib::get_current_dir(), "data", "pagerules.json");
}

void Shell::on_reload_rules() {
    const std::string file = rules_file();
    std::string err;
    m_rules = core::rules_load(file, &err);

    auto lg = log::get(log::Area::Cases);
    if (lg) {
        if (!err.empty()) lg->error("page rules parse failed: {}", err);
        else              lg->info("page rules loaded: {} rule(s)", m_rules.size());
    }

    // Validated AGAINST THE ROSTER, which is the whole reason this reload runs
    // after that one: a rule naming a broker nobody has is a rule that will
    // never fire, and it is the same class of bug as a dead mapping entry.
    const auto problems = core::rules_validate(m_rules, &m_roster);
    for (const auto& p : problems) if (lg) lg->warn("rules: {}", p);

    // No status line of its own. A rule is not a thing the user does anything
    // with; what they need to know is which of THEIR cases cannot be read, and
    // that sentence is the maintenance line on the cases page.
    if (!m_caseload.empty()) on_reload_cases();
}

// The clock lives on the UI side. core::Case does date ARITHMETIC and never
// asks what day it is -- a "pure" function that reads the clock isn't pure, and
// "which cases are due" would stop being testable the moment the core could
// answer that itself. The UI knows today; the core knows how to count.
std::string Shell::today() const {
    return Glib::DateTime::create_now_local().format("%Y-%m-%d");
}

// One row of display text. The URL's HOST only, never the full path -- the path
// is the part carrying the name ("/John-Smith/Tennessee/..."), and a screenshot
// of this window is a thing that happens.
std::string Shell::case_row_text(const core::Case& k) const {
    const auto* b = core::roster_find(m_roster, k.broker_id);
    std::string who = b ? b->name : k.broker_id;

    // The host comes from core::url_host now that the intake path needs the
    // same answer -- one parser, so the row and the matcher can never disagree
    // about what site a listing is on.
    const std::string host = core::url_host(k.url);

    std::string line = who + "   [" + core::status_name(k.status) + "]";

    // Provenance is shown ONLY for a removed case, and it is shown plainly,
    // because the difference between "they say so" and "we looked" is the
    // entire reason this app exists.
    if (k.status == core::Status::Removed)
        line += "  (" + std::string(core::provenance_name(k.provenance)) + ")";

    line += "   last check: ";
    if (k.outcome == core::Outcome::Never) {
        line += "never";
    } else {
        line += core::outcome_name(k.outcome);
        if (k.outcome == core::Outcome::Indeterminate)
            line += " (" + std::string(core::reason_name(k.reason)) + ")";
        if (!k.last_attempt.empty()) line += " on " + k.last_attempt;
    }
    if (k.consecutive_failures > 0)
        line += "  [" + std::to_string(k.consecutive_failures) + " failed]";
    // The evidence toward believing it gone, shown next to the evidence
    // against. One clean absence is an event and two is a pattern, and a row
    // that shows only "not found" hides which of those this is.
    if (k.clean_absences > 0)
        line += "  [" + std::to_string(k.clean_absences) + " clean]";
    if (!k.next_check.empty()) line += "   due " + k.next_check;
    line += "   " + host;
    return line;
}

void Shell::on_reload_cases() {
    const std::string file = cases_file();
    std::string err;
    m_caseload = core::caseload_load(file, &err);

    auto lg = log::get(log::Area::Cases);
    if (lg) {
        if (!err.empty()) lg->error("caseload parse failed: {}", err);
        else              lg->info("caseload loaded: {} case(s)", m_caseload.size());
    }

    const auto problems = core::caseload_validate(m_caseload);
    for (const auto& p : problems) if (lg) lg->warn("caseload: {}", p);

    // Per-case logging goes through log_ref() and nothing else. The row text
    // above is for the window; the log gets an id and an outcome.
    for (const auto& k : m_caseload)
        if (lg) lg->debug("{} status={} outcome={}", core::log_ref(k),
                          core::status_name(k.status), core::outcome_name(k.outcome));

    // Which case was selected, by ID rather than by row index: a repaint after
    // a relist has one more row than it started with, and an index would then
    // move the user's selection onto a different listing without telling them.
    std::string was_selected;
    if (auto* sel = m_cases_list.get_selected_row()) {
        const int i = sel->get_index();
        if (i >= 0 && static_cast<std::size_t>(i) < m_caseload.size())
            was_selected = m_caseload[static_cast<std::size_t>(i)].id;
    }

    while (auto* row = m_cases_list.get_row_at_index(0)) m_cases_list.remove(*row);
    for (const auto& k : m_caseload) {
        auto* row = Gtk::make_managed<Gtk::Label>(case_row_text(k));
        row->set_xalign(0.0f);
        row->set_margin(6);
        m_cases_list.append(*row);
    }

    if (!was_selected.empty()) {
        for (std::size_t i = 0; i < m_caseload.size(); ++i) {
            if (m_caseload[i].id != was_selected) continue;
            if (auto* row = m_cases_list.get_row_at_index(static_cast<int>(i)))
                m_cases_list.select_row(*row);
            break;
        }
    }
    refresh_check_button();

    if (!err.empty()) {
        m_cases_status.set_text("Caseload failed to parse: " + err);
        m_cases_exposure.set_text("");
        m_cases_maintenance.set_text("");
        return;
    }

    if (m_caseload.empty()) {
        // The honest empty state. Discovery is deliberately not automated, so
        // an empty caseload is the correct first-run condition and not an error
        // to apologise for.
        m_cases_status.set_text("No cases yet.");
        m_cases_maintenance.set_text("");
        m_cases_exposure.set_text(
            "Find your own listing in your own browser, then add its URL with + "
            "in the title bar (Ctrl+N). delr does not search brokers for you -- "
            "querying them with your name is a signal to them, not an "
            "observation of them.");
        return;
    }

    const std::string now = today();
    const auto due = core::caseload_due(m_caseload, now);

    int indeterminate = 0;
    for (const auto& k : m_caseload)
        if (k.outcome == core::Outcome::Indeterminate) ++indeterminate;

    std::string status = std::to_string(m_caseload.size()) + " case(s), " +
                         std::to_string(due.size()) + " due as of " + now;
    // Unchecked cases lead the status line rather than hiding in the list. A
    // run where half the checks were blocked must not read as a clean sweep.
    if (indeterminate > 0)
        status += "   --   " + std::to_string(indeterminate) + " unchecked";
    if (!problems.empty())
        status += "   --   " + std::to_string(problems.size()) +
                  " validation problem(s), see log";
    m_cases_status.set_text(status);

    // The roll-up: what of yours is exposed, and on how many live listings.
    const auto by = core::exposure_by_field(m_caseload, false);
    if (by.empty()) {
        m_cases_exposure.set_text("No exposed fields recorded on live cases.");
    } else {
        std::string line = "Exposed:";
        for (std::size_t i = 0; i < by.size(); ++i) {
            line += (i ? ",  " : "  ");
            line += std::string(core::field_name(by[i].field)) + " x" +
                    std::to_string(by[i].count);
        }
        m_cases_exposure.set_text(line);
    }

    // ── The maintenance queue ────────────────────────────────────────────────
    // `caseload_unverifiable()` is the core's own answer to "which of these did
    // we fetch and fail to READ" -- a missing rule, a rotted one, a page that
    // did not fingerprint. Asked rather than recomputed here, because it is
    // the honest denominator behind any count this window prints, and two
    // definitions of "unverifiable" would be one too many.
    //
    // Named by BROKER, not by case: the fix is a rule, and a rule is per
    // broker. A list of case ids would tell the user nothing they can act on.
    const auto stuck = core::caseload_unverifiable(m_caseload);
    if (stuck.empty()) {
        m_cases_maintenance.set_text("");
    } else {
        std::vector<std::string> who;
        for (const core::Case* k : stuck) {
            const auto* b = core::roster_find(m_roster, k->broker_id);
            const std::string name = b ? b->name : k->broker_id;
            if (std::find(who.begin(), who.end(), name) == who.end())
                who.push_back(name);
        }
        std::string line = std::to_string(stuck.size()) +
                           " listing(s) could be fetched but not read -- ";
        for (std::size_t i = 0; i < who.size(); ++i) line += (i ? ", " : "") + who[i];
        line +=
            ". That is a page rule missing or out of date, not an answer about "
            "the listing: those checks count as unverified and never as gone.";
        m_cases_maintenance.set_text(line);
    }
}

// A row was picked, or unpicked. The only thing that hangs off it is whether
// there is something for the check button to act on.
void Shell::on_case_selected() { refresh_check_button(); }

// ── Intake ───────────────────────────────────────────────────────────────────

void Shell::on_add_case() {
    // Both tables go in by value; the dialog works from a snapshot and hands
    // back a case. It never touches the file.
    m_add_dialog.open(*this, m_roster, m_caseload, today());
}

void Shell::on_case_committed(core::Case fresh) {
    auto lg = log::get(log::Area::Cases);

    // The dialog minted the id against the snapshot it was given. The live
    // caseload is the authority, and it may have moved underneath -- a reload
    // while the dialog was open, a second window later. Re-mint on collision
    // rather than refusing the user's work.
    if (core::caseload_find(m_caseload, fresh.id)) {
        const std::string was = fresh.id;
        fresh.id = core::next_case_id(m_caseload, fresh.broker_id, today());
        if (lg) lg->warn("intake: id {} was taken, minted {}", was, fresh.id);
    }

    if (!core::caseload_commit(m_caseload, fresh)) {
        if (lg) lg->error("intake: commit refused for {}", core::log_ref(fresh));
        m_cases_status.set_text("Could not add that case -- see the log.");
        return;
    }

    // The caseload's directory may not exist yet (DELR_CASES can point
    // anywhere). Creating it is path work, so it is UI-side, like resolving it.
    const std::string file = cases_file();
    std::error_code ec;
    std::filesystem::create_directories(Glib::path_get_dirname(file), ec);

    if (!core::caseload_save(file, m_caseload)) {
        // The case is in memory but not on disk. Say so plainly: a silent
        // failure here loses the one thing the user just did by hand.
        if (lg) lg->error("intake: save failed");
        m_cases_status.set_text("Added, but the caseload could not be saved -- see the log.");
        return;
    }
    if (lg) lg->info("intake: committed {}, caseload now {} case(s)",
                     core::log_ref(fresh), m_caseload.size());

    // Repaint FROM DISK rather than from memory: the reload proves the write
    // round-tripped, and the trace and the window then agree by construction
    // instead of by assumption.
    on_reload_cases();
    m_stack.set_visible_child(m_cases_page);
}

// ── The tunnel policy ────────────────────────────────────────────────────────

// Same shape as the two above, third table. Unlike the roster and like the
// caseload this file is PII -- it holds `naked_exit` -- and the pump writes it
// 0600 for that reason. Path resolution stays here because path resolution is
// the UI's job, which is the same seam that keeps the clock out of the core.
std::string Shell::egress_file() const {
    // Deferred to `netcheck::policy_path()` rather than repeated here, so
    // `delr --netcheck` with no arguments reads the same file this window
    // writes. Two definitions of a default path is two programs.
    const std::string p = netcheck::policy_path();
    if (Glib::path_is_absolute(p)) return p;
    return Glib::build_filename(Glib::get_current_dir(), p);
}

void Shell::on_reload_egress() {
    std::string err;
    m_egress = core::egress_policy_load(egress_file(), &err);

    const auto problems = core::egress_policy_validate(m_egress);
    // The validator's answer, cached for the check button. Whether a check may
    // be OFFERED is the same question as whether the policy is coherent, and
    // that question already has an owner.
    m_egress_ok = err.empty() && problems.empty();

    // The pristine state: nothing has ever been configured. Distinguished from
    // "configured, and wrong" because they are different events and want
    // different levels -- a first launch is not a warning, and a policy that
    // used to work and no longer validates is.
    const bool untouched = m_egress.interface_name.empty() &&
                           m_egress.dns == core::DnsMode::Unset &&
                           m_egress.accepted_exits.empty() &&
                           m_egress.naked_exit.empty() &&
                           // s9's other half of the baseline. A user who
                           // recorded their lookups and nothing else has
                           // configured something, and logging that as a first
                           // launch would file a real state under "pristine".
                           m_egress.naked_resolvers.empty();

    auto lg = log::get(log::Area::App);
    if (lg) {
        if (!err.empty()) {
            lg->error("egress policy parse failed: {}", err);
        } else if (untouched) {
            lg->info("egress policy: none saved yet, so nothing will be checked");
        } else {
            // NOT `egress_log_ref` with a verdict here. That function pairs an
            // interface with a JUDGMENT, and nothing has been judged at load
            // time -- passing `Pass` to fill the slot printed "egress:-/pass"
            // over a policy that refuses everything, which is the one thing a
            // log in this program must never do. The interface name and the
            // mode are both safe to say; neither is an address.
            lg->info("egress policy loaded: interface={} lookups={} problems={}",
                     m_egress.interface_name.empty() ? "-" : m_egress.interface_name,
                     core::dns_mode_name(m_egress.dns), problems.size());
        }
    }

    // The validator already says "egress: " at the front of every line. The
    // area tag is on the left of the log line as well, so a second prefix here
    // made it "egress: egress: ...".
    if (lg && !untouched)
        for (const auto& p : problems) lg->warn("{}", p);

    refresh_check_button();

    if (!err.empty()) {
        m_egress_status.set_text("Tunnel settings failed to parse — see the log.");
    } else if (problems.empty()) {
        m_egress_status.set_text(
            "Tunnel: configured. Checks will run a preflight first and refuse "
            "if it does not pass.");
    } else if (untouched) {
        // The honest first-run state, and not an error to apologise for: a
        // policy nobody has written is a policy that allows nothing, which is
        // the safe reading of "I don't know".
        m_egress_status.set_text(
            "Tunnel: not set up yet, so nothing will be checked. Open “Tunnel "
            "and privacy” in the menu — checks go out through a tunnel or they "
            "do not go out at all.");
    } else {
        m_egress_status.set_text(
            "Tunnel: " + std::to_string(problems.size()) +
            " thing(s) still to sort out before anything can be checked. See "
            "“Tunnel and privacy” in the menu.");
    }
}

void Shell::on_egress_settings() { m_egress_dialog.open(*this, m_egress); }

void Shell::on_egress_saved(core::EgressPolicy p) {
    m_egress = std::move(p);

    const std::string file = egress_file();
    std::error_code ec;
    std::filesystem::create_directories(Glib::path_get_dirname(file), ec);

    auto lg = log::get(log::Area::App);
    if (!core::egress_policy_save(file, m_egress)) {
        if (lg) lg->error("egress policy save failed");
        m_egress_status.set_text(
            "Tunnel settings could not be saved — see the log.");
        return;
    }
    if (lg) lg->info("egress policy saved");

    // Repaint FROM DISK, exactly as the caseload does: the reload proves the
    // write round-tripped, so the trace and the window agree by construction
    // rather than by assumption.
    on_reload_egress();
}

void Shell::on_dump_registry() { registry::dump(); }

}  // namespace delr
