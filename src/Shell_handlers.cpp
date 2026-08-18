// Handlers -- slot bodies. What a user action actually does.
#include "Shell.hpp"
#include "Registry.hpp"
#include "Log.hpp"

#include <glibmm/miscutils.h>
#include <glibmm/datetime.h>
#include <gtkmm/label.h>
#include <cstdlib>

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

    std::string host = k.url;
    const auto scheme = host.find("://");
    if (scheme != std::string::npos) host = host.substr(scheme + 3);
    const auto slash = host.find('/');
    if (slash != std::string::npos) host = host.substr(0, slash);

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

    while (auto* row = m_cases_list.get_row_at_index(0)) m_cases_list.remove(*row);
    for (const auto& k : m_caseload) {
        auto* row = Gtk::make_managed<Gtk::Label>(case_row_text(k));
        row->set_xalign(0.0f);
        row->set_margin(6);
        m_cases_list.append(*row);
    }

    if (!err.empty()) {
        m_cases_status.set_text("Caseload failed to parse: " + err);
        m_cases_exposure.set_text("");
        return;
    }

    if (m_caseload.empty()) {
        // The honest empty state. Discovery is deliberately not automated, so
        // an empty caseload is the correct first-run condition and not an error
        // to apologise for.
        m_cases_status.set_text("No cases yet.");
        m_cases_exposure.set_text(
            "Find your own listing in your own browser, then add its URL here. "
            "delr does not search brokers for you -- querying them with your "
            "name is a signal to them, not an observation of them.");
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
}

void Shell::on_dump_registry() { registry::dump(); }

}  // namespace delr
