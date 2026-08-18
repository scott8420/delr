// Shell -- glue only: the ctor and the ordered build_ui() pass.
// Everything else lives in Shell_{zones,bindings,handlers}.cpp; the header is
// the index (CANON).
#include "Shell.hpp"
#include "Log.hpp"

namespace delr {

Shell::Shell()
    : m_add_button("shell.add", "Add case"),
      m_menu_button("shell.menu"),
      m_body("shell.body", Gtk::Orientation::HORIZONTAL),
      m_sidebar("shell.sidebar"),
      m_stack("shell.pages"),
      m_roster_page("shell.roster", Gtk::Orientation::VERTICAL),
      m_roster_status("shell.roster.status"),
      m_roster_scroll("shell.roster.scroll"),
      m_roster_list("shell.roster.list"),
      m_cases_page("shell.cases", Gtk::Orientation::VERTICAL),
      m_egress_status("shell.cases.egress"),
      m_cases_status("shell.cases.status"),
      m_cases_exposure("shell.cases.exposure"),
      m_cases_maintenance("shell.cases.maintenance"),
      m_check_row("shell.cases.check", Gtk::Orientation::HORIZONTAL, 8),
      m_check_button("shell.cases.check.button"),
      m_check_state("shell.cases.check.state"),
      m_cases_scroll("shell.cases.scroll"),
      m_cases_list("shell.cases.list") {
    set_name("shell");
}

// The dialog is a member and outlives nothing: if it is still on screen when
// the main window goes away, hide it first rather than destroying a visible
// top-level.
Shell::~Shell() {
    m_add_dialog.set_visible(false);
    m_egress_dialog.set_visible(false);
    // A check may still be in flight. Joining is not politeness: the worker
    // writes into members of this object, and letting it outlive the window
    // is a write into freed memory. It cannot be cancelled -- it is two round
    // trips on a timeout -- so the window waits for it.
    if (m_check_worker.joinable()) m_check_worker.join();
}

// The ordered pass: construct -> name/register (done by Named<W> in the ctor)
// -> bind. Nothing here does real work; each step delegates to its category.
void Shell::build_ui() {
    if (auto lg = log::get(log::Area::Shell)) lg->info("building ui");
    build_shell();
    build_pages();
    bind_actions();
    on_reload_roster();   // first paint from disk
    on_reload_rules();    // before the cases: the maintenance line reads it
    on_reload_cases();
    on_reload_egress();   // last: it decides whether the check button is live
}

}  // namespace delr
