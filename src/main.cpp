#include "App.hpp"
#include "selftest.hpp"

#include <cstring>

// Thin bootstrap (CANON). Everything real is in App / Shell / the core.
// `--selftest` exercises the GTK-free core and exits without touching GTK.
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--selftest") == 0)
            return delr::selftest::run();

    return delr::App::create()->run(argc, argv);
}
