#include "App.hpp"
#include "Paths.hpp"
#include "importer.hpp"
#include "netcheck.hpp"
#include "selftest.hpp"

#include <cstdio>
#include <cstring>
#include <string>

// Thin bootstrap (CANON). Everything real is in App / Shell / the core.
// `--selftest` exercises the GTK-free core and exits without touching GTK.
int main(int argc, char** argv) {
    // `--selftest` is answered BEFORE the state directory is touched. The
    // checks are hermetic -- they must not create a directory in the running
    // user's home, and they must not migrate that user's real files as a side
    // effect of being run.
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--selftest") == 0) return delr::selftest::run();

    // Before ANY file is read, by any mode. The migration has to precede the
    // first load or the app starts with an empty policy and asks the user to
    // re-record a baseline -- which means physically disconnecting their VPN
    // to recover a file that was on disk the whole time.
    {
        std::string err;
        if (!delr::paths::ensure_state_dir(&err))
            std::fprintf(stderr, "delr: %s\n", err.c_str());
        for (const auto& line : delr::paths::migrate_legacy_state())
            std::fprintf(stderr, "delr: %s\n", line.c_str());
    }

    for (int i = 1; i < argc; ++i) {
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

        // The roster from the state's own CSV. A maintainer refreshing the
        // shipped asset and a user who would rather build it themselves run
        // the same command; the second is the reason it is a mode of the
        // binary rather than a script in the repo.
        if (std::strcmp(argv[i], "--import-registry") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr,
                             "usage: delr --import-registry <registry.csv> [<out.json>]\n");
                return 2;
            }
            const std::string csv = argv[i + 1];
            const std::string out = (i + 2 < argc) ? argv[i + 2] : std::string{};
            return delr::importer::run(csv, out);
        }
    }

    return delr::App::create()->run(argc, argv);
}
