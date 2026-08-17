// Handlers -- slot bodies. What a user action actually does.
#include "Shell.hpp"
#include "Registry.hpp"
#include "Log.hpp"

#include <glibmm/miscutils.h>
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

void Shell::on_dump_registry() { registry::dump(); }

}  // namespace delr
