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
    # The CPPA registry becomes the roster. Pure -- text in, Roster out -- so
    # the transformation that produces table one is itself checked, and so a
    # user can rebuild the roster from the state's own CSV rather than trusting
    # ours.
    src/core/RosterImport.cpp
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
    # Table three: what a fetched page has to SAY for a listing to count as
    # gone. Per-broker, ships beside the roster, and rots on a designer's
    # schedule rather than a company's -- so it is its own file, its own
    # version, and its own review cadence.
    src/core/PageRules.cpp
    # The pure half of the observer: everything between the bytes a syscall
    # hands back and the observation Egress judges. The syscalls themselves
    # land above this library; what their output MEANS is decided here, where
    # the selftest can reach it.
    src/core/Probe.cpp
)
target_include_directories(delr_core PUBLIC include)
# Vendored nlohmann/json (header-only, MIT), PRIVATE -- an implementation
# detail of the pump; no consumer inherits the include.
target_include_directories(delr_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ── libcurl -- the transport, and the only producer the egress policy has ─────
# The first non-GUI runtime dependency in the tree, taken deliberately: it is
# the one library that gives BOTH halves of core/Egress a producer.
#   CURLOPT_INTERFACE      -> bind to the tunnel's address. The killswitch is
#                             structural: if the tunnel is gone the bind fails
#                             and nothing leaves, rather than quietly leaving
#                             the other way.
#   socks5h:// (the 'h')   -> names resolved AT THE FAR END. DnsMode::System is
#                             a hard refusal in Egress precisely because a
#                             local resolver leaks the whole check roster while
#                             the HTTP side looks clean.
#
# Licence: the curl licence, an MIT/X11 derivative -- same family as delr's
# own. Keep the notice when libcurl is SHIPPED with a build (AppImage, Flatpak,
# Windows); linking the distro's system libcurl distributes nothing and carries
# no obligation. See LICENSES.md. The TLS backend, not curl, is where the
# licence surface actually varies: prefer the OpenSSL build (Apache-2.0).
#
# OPTIONAL, like gtkmm above and for the same reason: delr_core must still
# build on a machine with nothing installed, so CI and a headless container can
# run the selftest. Missing libcurl skips the fetch and says so.
find_package(CURL QUIET)
if(CURL_FOUND)
    message(STATUS "delr: libcurl ${CURL_VERSION_STRING} -- the fetch can be built")
else()
    message(STATUS "delr: libcurl not found -- verification fetch unavailable")
    message(STATUS "delr: run ./setup-dev.sh to install the dev packages")
endif()

# -- delr_net -- the one impure layer, and the only one -----------------------
# A SECOND library rather than a folder in the app target, and the reason is
# the same one delr_core exists for: the seam is enforced by the build graph.
# There is no gtkmm on this line, so a UI include under net/ fails to compile
# and the fetch stays reachable from a headless selftest. And libcurl is on
# THIS line rather than delr_core's, so the core still builds on a machine
# with nothing installed.
#
# Built ALWAYS, even with no libcurl. Without it the same functions compile to
# a stub that returns NotBuilt and says so, which keeps the target list from
# forking on a dependency and keeps the url guard, the error vocabulary and
# the egress gate exercisable everywhere. A build with no network is a real
# configuration -- CI runs one -- and it should be a quieter program, not a
# different one.
add_library(delr_net STATIC
    # The transport. Makes no judgments: whether it may leave was decided in
    # core/Egress, and what the page means is decided in core/PageRules.
    src/net/Fetch.cpp
    # The syscall shim. getifaddrs, a bind, a getsockname, one file read and
    # two round trips -- everything those bytes MEAN was settled in core/Probe
    # before this file existed.
    src/net/Observer.cpp
)
target_link_libraries(delr_net PUBLIC delr_core)
if(CURL_FOUND)
    target_link_libraries(delr_net PRIVATE CURL::libcurl)
    target_compile_definitions(delr_net PRIVATE DELR_HAVE_CURL=1)
endif()

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
        # Path resolution: assets beside the program, state under XDG. GTK-free
        # and glib-free on purpose -- see include/Paths.hpp.
        src/Paths.cpp
        # Follows the desktop's light/dark preference (GTK4 will not do it for
        # us and we do not link libadwaita). Needs no new dependency: the D-Bus
        # and GSettings calls are GIO, which gtkmm-4.0 already brings in.
        src/Appearance.cpp
        src/Shell.cpp
        # The paste-a-URL dialog. A window of its own rather than a fifth Shell
        # category: it has its own lifetime (hide on close) and its own state,
        # and the Shell's split is by category of code, not by feature.
        src/AddCaseDialog.cpp
        # The tunnel settings window -- the s7 blocker. A window of its own for
        # the same reason as the one above: its own lifetime, its own state,
        # and a worker thread the Shell has no business owning.
        src/EgressDialog.cpp
        src/Shell_zones.cpp
        src/Shell_bindings.cpp
        src/Shell_handlers.cpp
        # The fifth Shell category: what runs OFF the main thread. The check --
        # preflight, fetch, read the page, record it -- is two round trips plus
        # a page fetch, and a window frozen for that long is a window people
        # force-quit. A category rather than a feature: the scheduled run lands
        # here too.
        src/Shell_work.cpp
        src/Registry.cpp
        src/Log.cpp
        # One real preflight from the command line, because the settings
        # surface that would otherwise run it is s7. Prints no addresses.
        src/netcheck.cpp
        # The roster from the state's CSV, on demand. I/O only -- every
        # judgement is in core/RosterImport where the checks can reach it.
        src/importer.cpp
        # The core's checks ride in the app binary behind `--selftest` rather
        # than as a second executable. Compiled in, never run automatically.
        src/selftest.cpp
    )
    target_include_directories(delr PRIVATE include ${GTKMM_INCLUDE_DIRS})
    target_link_libraries(delr      PRIVATE ${GTKMM_LIBRARIES} delr_core delr_net spdlog::spdlog)
    target_link_directories(delr    PRIVATE ${GTKMM_LIBRARY_DIRS})
    target_compile_options(delr     PRIVATE ${GTKMM_CFLAGS_OTHER})
else()
    message(STATUS "delr: gtkmm-4.0 and/or spdlog not found -- building delr_core only")
    message(STATUS "delr: run ./setup-dev.sh to install the dev packages")
endif()
