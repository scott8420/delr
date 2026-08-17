#pragma once

// The core's selftest, reachable as `delr --selftest`. Returns 0 when every
// check passes. Lives in the app binary deliberately -- a second executable
// nobody launches is clutter; a flag on the real one gets exercised.
namespace delr::selftest {
int run();
}
