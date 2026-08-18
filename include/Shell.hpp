#pragma once
#include "widgets/Widgets.hpp"
#include "core/Broker.hpp"
#include "core/Case.hpp"

#include <gtkmm/applicationwindow.h>
#include <giomm/menu.h>

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Shell -- the app window, AND the split-class exemplar.
//
// Split across TUs by CATEGORY OF CODE, not by feature (CANON: vocabulary
// first, file layout second + the header is the index). The split costs ~nothing
// while the parts are small; retrofitting it after a class grows to thousands of
// lines is a multi-session migration, so the structure is installed on day one.
// Every method carries a trailing `// category:` tag naming the file its body
// lives in. Read the tag, open that file.
//
// Vocabulary
// ----------
//   Zones    -- construction of named regions: frame, header, sidebar+stack,
//               pages, menu model.            File: src/Shell_zones.cpp
//   Bindings -- what's wired to what: actions, accelerators.
//                                             File: src/Shell_bindings.cpp
//   Handlers -- slot bodies (on_*): what a user action actually does.
//                                             File: src/Shell_handlers.cpp
//   Glue     -- ctor + build_ui(): the ordered construct -> name/register ->
//               bind pass.                    File: src/Shell.cpp
//
// (Helpers arrives as src/Shell_helpers.cpp when the first shared sync routine
// does -- widen on demand, not for symmetry.)
// ─────────────────────────────────────────────────────────────────────────────
namespace delr {

class Shell : public Gtk::ApplicationWindow {
public:
    Shell();
    ~Shell() override;

    void build_ui();                          // category: glue

private:
    // ── zones ──────────────────────────────────────────────────────────────
    void build_shell();                       // category: zone: window + sidebar + stack
    void build_pages();                       // category: zone: roster + cases pages
    Glib::RefPtr<Gio::Menu> build_menu();     // category: zone: hamburger model

    // ── bindings ───────────────────────────────────────────────────────────
    void bind_actions();                      // category: bindings: actions

    // ── handlers ───────────────────────────────────────────────────────────
    void on_reload_roster();                  // category: handler: re-read the roster from disk
    void on_reload_cases();                   // category: handler: re-read the caseload from disk
    void on_dump_registry();                  // category: handler: print the live widget tree

    // Where the JSON lives. UI-side path resolution -- the core takes a plain
    // string and stays free of GTK (the seam).
    std::string roster_file() const;
    std::string cases_file() const;

    // Today, as ISO "YYYY-MM-DD". Also UI-side, and for the same reason: the
    // core does date ARITHMETIC but never asks what day it is. A pure function
    // that reads the clock isn't pure, and "what's due" would stop being
    // testable the moment the core could answer it itself.
    std::string today() const;

    // One row's worth of display text for a case. Deliberately a helper rather
    // than inline in the handler: the roster page will want the same treatment
    // when it grows columns, and this is the shape that gets shared.
    std::string case_row_text(const core::Case& k) const;

    // Titlebar.
    widgets::MenuButton m_menu_button;

    // Body: sidebar | stack (each surface is a stack page).
    widgets::Box          m_body;
    widgets::StackSidebar m_sidebar;
    widgets::Stack        m_stack;

    // Roster page: a count label + a scrolling list, rebuilt on reload.
    widgets::Box            m_roster_page;
    widgets::Label          m_roster_status;
    widgets::ScrolledWindow m_roster_scroll;
    widgets::ListBox        m_roster_list;

    // Cases page: a status line, an exposure roll-up, and the scrolling list.
    widgets::Box            m_cases_page;
    widgets::Label          m_cases_status;
    widgets::Label          m_cases_exposure;
    widgets::ScrolledWindow m_cases_scroll;
    widgets::ListBox        m_cases_list;

    // Core state (model side of the one real seam).
    core::Roster   m_roster;
    core::Caseload m_caseload;
};

}  // namespace delr
