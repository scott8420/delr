#pragma once
#include <string>

// `delr --netcheck [<interface> [socks5h://host:port]]` -- one real preflight,
// reported without a single address. See src/netcheck.cpp.
//
// With no arguments it runs the SAVED policy, which is the check Scott wants
// when the window says it is configured and a fetch still refuses: it answers
// "is what got written to disk the thing that passes" from a terminal, with no
// display and nothing to screenshot. With an interface it runs an ad-hoc
// policy, which is what it did before there was a file.
namespace delr::netcheck {

// Where the saved policy lives. Defined HERE and called by `Shell::egress_file`
// rather than the other way around, so the default path has one definition
// instead of two that can drift. It belongs in a `paths` module the day a third
// caller appears; two does not earn one.
std::string policy_path();

int run(const std::string& interface_name, const std::string& proxy = {});
int run_saved();

}  // namespace delr::netcheck
