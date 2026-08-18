#include "App.hpp"
#include "netcheck.hpp"
#include "selftest.hpp"

#include <cstdio>
#include <cstring>
#include <string>

// Thin bootstrap (CANON). Everything real is in App / Shell / the core.
// `--selftest` exercises the GTK-free core and exits without touching GTK.
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--selftest") == 0)
            return delr::selftest::run();

        // The preflight, on demand, before anything can configure one. Prints
        // no addresses -- see src/netcheck.cpp.
        if (std::strcmp(argv[i], "--netcheck") == 0) {
            // No interface: run the policy that was SAVED. This is the mode
            // that answers "is the thing on disk the thing that passes", which
            // is the question a settings window cannot answer about itself.
            if (i + 1 >= argc) return delr::netcheck::run_saved();
            const std::string iface = argv[i + 1];
            const std::string proxy = (i + 2 < argc) ? argv[i + 2] : std::string{};
            return delr::netcheck::run(iface, proxy);
        }
    }

    return delr::App::create()->run(argc, argv);
}
