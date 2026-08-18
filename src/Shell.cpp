// Shell -- glue only: the ctor and the ordered build_ui() pass.
// Everything else lives in Shell_{zones,bindings,handlers}.cpp; the header is
// the index (CANON).
#include "Shell.hpp"
#include "Log.hpp"

namespace delr {

Shell::Shell()
    : m_menu_button("shell.menu"),
      m_body("shell.body", Gtk::Orientation::HORIZONTAL),
      m_sidebar("shell.sidebar"),
      m_stack("shell.pages"),
      m_roster_page("shell.roster", Gtk::Orientation::VERTICAL),
      m_roster_status("shell.roster.status"),
      m_roster_scroll("shell.roster.scroll"),
      m_roster_list("shell.roster.list"),
      m_cases_page("shell.cases", Gtk::Orientation::VERTICAL),
      m_cases_status("shell.cases.status"),
      m_cases_exposure("shell.cases.exposure"),
      m_cases_scroll("shell.cases.scroll"),
      m_cases_list("shell.cases.list") {
    set_name("shell");
}

Shell::~Shell() = default;

// The ordered pass: construct -> name/register (done by Named<W> in the ctor)
// -> bind. Nothing here does real work; each step delegates to its category.
void Shell::build_ui() {
    if (auto lg = log::get(log::Area::Shell)) lg->info("building ui");
    build_shell();
    build_pages();
    bind_actions();
    on_reload_roster();   // first paint from disk
    on_reload_cases();
}

}  // namespace delr
