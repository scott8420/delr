// Bindings -- what's wired to what. Actions only; the bodies are handlers.
#include "Shell.hpp"

namespace delr {

void Shell::bind_actions() {
    add_action("add-case",      sigc::mem_fun(*this, &Shell::on_add_case));
    add_action("reload-roster", sigc::mem_fun(*this, &Shell::on_reload_roster));
    add_action("reload-cases",  sigc::mem_fun(*this, &Shell::on_reload_cases));
    add_action("dump-registry", sigc::mem_fun(*this, &Shell::on_dump_registry));

    // The dialog hands back a case; the Shell owns the file and the repaint.
    // Wired here rather than in the dialog's ctor because this is where "what
    // is wired to what" lives, and the answer is a fact about the Shell.
    m_add_dialog.signal_committed().connect(
        sigc::mem_fun(*this, &Shell::on_case_committed));

    // Accelerators belong to the APPLICATION, not the window -- the action is
    // still win.*, but only the application can bind a key to it.
    if (auto app = get_application())
        app->set_accels_for_action("win.add-case", {"<Control>n"});
}

}  // namespace delr
