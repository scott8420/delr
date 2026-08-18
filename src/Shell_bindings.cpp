// Bindings -- what's wired to what. Actions only; the bodies are handlers.
#include "Shell.hpp"

namespace delr {

void Shell::bind_actions() {
    add_action("add-case",      sigc::mem_fun(*this, &Shell::on_add_case));
    add_action("reload-roster", sigc::mem_fun(*this, &Shell::on_reload_roster));
    add_action("reload-cases",  sigc::mem_fun(*this, &Shell::on_reload_cases));
    add_action("reload-rules",  sigc::mem_fun(*this, &Shell::on_reload_rules));
    // Kept, rather than fired and forgotten: this action is the check
    // button's enabled state, and `refresh_check_button` turns it on and off.
    m_check_action =
        add_action("check-now", sigc::mem_fun(*this, &Shell::on_check_now));
    m_check_action->set_enabled(false);   // nothing selected, no policy read yet
    add_action("egress",        sigc::mem_fun(*this, &Shell::on_egress_settings));
    add_action("dump-registry", sigc::mem_fun(*this, &Shell::on_dump_registry));
    add_action("quit",          sigc::mem_fun(*this, &Shell::on_quit));

    // The dialog hands back a case; the Shell owns the file and the repaint.
    // Wired here rather than in the dialog's ctor because this is where "what
    // is wired to what" lives, and the answer is a fact about the Shell.
    m_add_dialog.signal_committed().connect(
        sigc::mem_fun(*this, &Shell::on_case_committed));

    // Same seam: the settings window edits a copy and hands one back; the
    // Shell owns the path and the pump.
    m_egress_dialog.signal_saved().connect(
        sigc::mem_fun(*this, &Shell::on_egress_saved));

    // The check's two wires. The row selection decides WHICH case the button
    // acts on, so picking a row is what turns the button on; the dispatcher is
    // how a finished worker gets back onto the main thread, and connecting it
    // here rather than where the thread starts keeps "what is wired to what"
    // answerable from one file.
    m_cases_list.signal_selected_rows_changed().connect(
        sigc::mem_fun(*this, &Shell::on_case_selected));
    m_check_done.connect(sigc::mem_fun(*this, &Shell::on_check_done));

    // ── keys ────────────────────────────────────────────────────────────────
    // Accelerators belong to the APPLICATION, not the window -- the action is
    // still win.*, but only the application can bind a key to it.
    //
    // Bound HERE and not in the menu model, and that is the reason they are
    // worth the lines: GTK reads these back when it builds the popover, so
    // every item shows its key beside it. A menu that teaches its own
    // shortcuts is the cheapest form of the thing s12 found missing -- the
    // app saying out loud what it can do.
    //
    // Conventional keys, deliberately: Ctrl+N to make one, Ctrl+comma for
    // settings, F5 to reload, Ctrl+Q to leave. A privacy tool asking someone
    // to learn a private idiom spends trust it needs elsewhere.
    if (auto app = get_application()) {
        app->set_accels_for_action("win.add-case",   {"<Control>n"});
        app->set_accels_for_action("win.check-now",  {"<Control>Return"});
        app->set_accels_for_action("win.egress",     {"<Control>comma"});
        // The caseload is the table a user edits by hand; the other two get
        // the same key with a modifier rather than three unrelated keys.
        app->set_accels_for_action("win.reload-cases",  {"F5"});
        app->set_accels_for_action("win.reload-roster", {"<Shift>F5"});
        app->set_accels_for_action("win.quit",       {"<Control>q"});
    }
}

}  // namespace delr
