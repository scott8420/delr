cmake_minimum_required(VERSION 3.20)
project(delr VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)

# ── delr_core -- the GTK-free core (the model side of the one real seam) ──────
# No gtkmm on this library's link line, BY DESIGN: a stray UI include under
# core/ fails to compile, so the seam is enforced by the build graph and the
# core stays headless-testable.
add_library(delr_core STATIC
    src/core/Broker.cpp
    # Table two: the caseload. PII, unlike the roster -- see the header.
    src/core/Case.cpp
    # The paste-a-URL path: parse, match, mint, commit. Everything between a
    # pasted URL and a row in the caseload that can be decided without a
    # display -- which is all of it.
    src/core/Intake.cpp
    # May this request leave, and out of which hole. Policy only -- no sockets,
    # no resolver, no clock: the whole leak-or-not decision is a pure function
    # of a configured policy and an observation, and so is exercised headless.
    src/core/Egress.cpp
)
target_include_directories(delr_core PUBLIC include)
# Vendored nlohmann/json (header-only, MIT), PRIVATE -- an implementation
# detail of the pump; no consumer inherits the include.
target_include_directories(delr_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ── delr -- the GTK app ───────────────────────────────────────────────────────
# gtkmm is OPTIONAL, deliberately: delr_core must build anywhere (CI, a headless
# container, a machine with no desktop stack). Missing gtkmm skips the app
# target with a status line rather than failing configure.
pkg_check_modules(GTKMM gtkmm-4.0)
find_package(spdlog QUIET)

if(GTKMM_FOUND AND spdlog_FOUND)
    # The Shell is a SPLIT CLASS across TUs by category (see the index in
    # include/Shell.hpp); all parts compile into the one delr::Shell.
    add_executable(delr
        src/main.cpp
        src/App.cpp
        # Follows the desktop's light/dark preference (GTK4 will not do it for
        # us and we do not link libadwaita). Needs no new dependency: the D-Bus
        # and GSettings calls are GIO, which gtkmm-4.0 already brings in.
        src/Appearance.cpp
        src/Shell.cpp
        # The paste-a-URL dialog. A window of its own rather than a fifth Shell
        # category: it has its own lifetime (hide on close) and its own state,
        # and the Shell's split is by category of code, not by feature.
        src/AddCaseDialog.cpp
        src/Shell_zones.cpp
        src/Shell_bindings.cpp
        src/Shell_handlers.cpp
        src/Registry.cpp
        src/Log.cpp
        # The core's checks ride in the app binary behind `--selftest` rather
        # than as a second executable. Compiled in, never run automatically.
        src/selftest.cpp
    )
    target_include_directories(delr PRIVATE include ${GTKMM_INCLUDE_DIRS})
    target_link_libraries(delr      PRIVATE ${GTKMM_LIBRARIES} delr_core spdlog::spdlog)
    target_link_directories(delr    PRIVATE ${GTKMM_LIBRARY_DIRS})
    target_compile_options(delr     PRIVATE ${GTKMM_CFLAGS_OTHER})
else()
    message(STATUS "delr: gtkmm-4.0 and/or spdlog not found -- building delr_core only")
    message(STATUS "delr: run ./setup-dev.sh to install the dev packages")
endif()
