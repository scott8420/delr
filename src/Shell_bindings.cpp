// Bindings -- what's wired to what. Actions only; the bodies are handlers.
#include "Shell.hpp"

namespace delr {

void Shell::bind_actions() {
    add_action("reload-roster", sigc::mem_fun(*this, &Shell::on_reload_roster));
    add_action("reload-cases",  sigc::mem_fun(*this, &Shell::on_reload_cases));
    add_action("dump-registry", sigc::mem_fun(*this, &Shell::on_dump_registry));
}

}  // namespace delr
