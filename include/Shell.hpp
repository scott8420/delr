#pragma once
#include "AddCaseDialog.hpp"
#include "EgressDialog.hpp"
#include "widgets/Widgets.hpp"
#include "core/Broker.hpp"
#include "core/Case.hpp"
#include "core/Egress.hpp"
#include "core/Intake.hpp"
#include "core/PageRules.hpp"
#include "net/Fetch.hpp"

#include <gtkmm/applicationwindow.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <glibmm/dispatcher.h>

#include <cstdint>
#include <string>
#include <thread>

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
//   Work     -- what runs OFF the main thread, and what happens when it lands.
//               A category and not a feature: "the check" is one job today and
//               a scheduled run is the next, and both belong to the same
//               question -- which is who may touch a widget.
//                                             File: src/Shell_work.cpp
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
    void on_add_case();                       // category: handler: open the paste-a-URL dialog
    void on_case_committed(core::Case fresh);  // category: handler: commit + save + repaint
    void on_reload_roster();                  // category: handler: re-read the roster from disk
    void on_reload_cases();                   // category: handler: re-read the caseload from disk
    void on_reload_rules();                   // category: handler: re-read the page rules from disk
    void on_reload_egress();                  // category: handler: re-read the tunnel policy
    void on_case_selected();                  // category: handler: a row was picked
    void on_egress_settings();                // category: handler: open the tunnel settings
    void on_egress_saved(core::EgressPolicy p);  // category: handler: persist + repaint
    void on_dump_registry();
    // Closing the one window ends the application. It also JOINS a check that
    // is still in flight (see ~Shell), so quitting mid-fetch waits for the
    // request to finish rather than tearing down memory the worker is writing
    // into. That wait is bounded by the fetch timeout and is not a hang.
    void on_quit();                  // category: handler: print the live widget tree

    // ── work ───────────────────────────────────────────────────────────────
    // One case, checked from a button. The threading is the shape EgressDialog
    // built for its preflight -- a single job slot, the button insensitive
    // while it is occupied, the worker handed copies and touching no widget --
    // and it is copied rather than reinvented, as that header asked.
    void on_check_now();                      // category: work: start a check
    void on_check_done();                     // category: work: apply what came back
    void start_check(const core::Case& k);    // category: work: fill the slot and go
    bool checking() const { return m_checking; }
    // Enables the button. Selection + a policy the validator does not complain
    // about; the judgment is `egress_policy_validate`'s, not this file's.
    //
    // It gates the ACTION and not the widget, and that is not a style choice:
    // a GTK4 button with an action name has its sensitivity driven BY the
    // action, so a `set_sensitive()` here would be silently overwritten the
    // next time the action reported its state. One owner of "can this be
    // pressed", same rule as everywhere else in this codebase.
    void refresh_check_button();              // category: work: the one gate

    // Where the JSON lives. UI-side path resolution -- the core takes a plain
    // string and stays free of GTK (the seam).
    std::string roster_file() const;
    std::string cases_file() const;
    // What a fetched page has to say for a listing to count as gone. Ships
    // beside the roster and rots on its own schedule -- its own file, its own
    // env override, for the same reason it is its own table.
    std::string rules_file() const;
    // The tunnel policy. Mode 0600 on disk, because it holds this machine's
    // own address -- see core/Egress's pump.
    std::string egress_file() const;

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
    widgets::Button     m_add_button;
    widgets::MenuButton m_menu_button;

    // The paste-a-URL surface. A MEMBER, built once and hidden on close rather
    // than constructed per use (the Cairn lifetime pattern) -- see its header
    // for why hiding also clears.
    AddCaseDialog m_add_dialog;

    // The tunnel settings. A member for the same reason, and it also holds
    // `naked_exit` while open -- so it clears on hide, like the one above.
    EgressDialog m_egress_dialog;

    // Body: sidebar | stack (each surface is a stack page).
    widgets::Box          m_body;
    widgets::StackSidebar m_sidebar;
    widgets::Stack        m_stack;

    // Roster page: what the app IS, then the table it reads.
    //
    // The explainer is on THIS page rather than on Cases, and that is Scott's
    // call from s12. The roster answers "who are these companies"; the
    // question standing behind it is "and what is this program", and the two
    // belong together. It is also the page the app opens on, so it is the
    // first thing anyone reads.
    widgets::Box            m_roster_page;
    widgets::Label          m_roster_lede;
    widgets::Label          m_roster_how;
    widgets::Label          m_roster_status;
    widgets::Separator      m_roster_rule;
    widgets::ScrolledWindow m_roster_scroll;
    widgets::ListBox        m_roster_list;

    // Cases page: a status line, an exposure roll-up, and the scrolling list.
    widgets::Box            m_cases_page;
    // The tunnel's state, above everything else on this page. s8 puts the
    // check button here, and a page offering to check with no tunnel
    // configured would be offering something that cannot happen.
    widgets::Label          m_egress_status;
    widgets::Label          m_cases_status;
    widgets::Label          m_cases_exposure;
    // The maintenance queue: listings we fetched and could not READ. Its own
    // line rather than a footnote in the status, because it is the honest
    // denominator for every "N listings verified" this app will ever say.
    widgets::Label          m_cases_maintenance;
    // Check a case, and what the last one said. The button acts on the
    // SELECTED row: one case checked deliberately, not a run.
    widgets::Box            m_check_row;
    widgets::Button         m_check_button;
    widgets::Label          m_check_state;
    widgets::ScrolledWindow m_cases_scroll;
    widgets::ListBox        m_cases_list;

    // Core state (model side of the one real seam).
    core::Roster    m_roster;
    core::Caseload  m_caseload;
    core::PageRules m_rules;

    // ── the job slot ───────────────────────────────────────────────────────
    // Written on the main thread before the worker starts, read by the worker,
    // written by the worker, read on the main thread after `m_check_done`
    // fires. Nothing else crosses, and only one job exists at a time.
    //
    // The PAGE ITSELF never appears here. `page_check` runs on the worker, so
    // the broker's body -- the single most sensitive string this program ever
    // holds -- lives in one stack frame and dies with it. What crosses the
    // thread boundary is a verdict.
    bool                    m_checking = false;
    std::thread             m_check_worker;
    Glib::Dispatcher        m_check_done;

    std::string             m_job_case_id;    // the case to write back to
    std::string             m_job_url;        // PII: the listing. Cleared on landing.
    core::EgressPolicy      m_job_policy;
    core::PageRule          m_job_rule;
    bool                    m_job_has_rule = false;
    core::EgressObservation m_job_obs;
    core::Verdict           m_job_verdict = core::Verdict::Unconfigured;
    net::FetchError         m_job_error = net::FetchError::NotBuilt;
    bool                    m_job_ours  = true;   // pessimistic: nothing judged yet
    core::PageVerdict       m_job_page  = core::PageVerdict::NoResponse;
    long                    m_job_ms    = 0;

    // True when the saved policy validates clean. Asked of the validator at
    // load time and cached for the button; never re-derived here.
    bool m_egress_ok = false;

    // Held because the check button's sensitivity lives here -- see above.
    Glib::RefPtr<Gio::SimpleAction> m_check_action;

    // Loaded at startup, edited through the dialog, saved by the Shell. A
    // default-constructed policy refuses everything, so a first run with no
    // file on disk is a program that will not fetch -- which is correct.
    core::EgressPolicy m_egress;
};

}  // namespace delr
