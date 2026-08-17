#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace Gtk { class Widget; }

// The live address book (CANON). name -> widget*, populated by Named<W> at
// construction, cleared at destruction. Mirrors what exists RIGHT NOW, not what
// the source could build. Payoff is debuggability: a backtrace frame names
// itself, find() resolves a name from gdb, dump() walks the live tree.
namespace delr::registry {

void add(std::string_view name, Gtk::Widget* w);
void remove(Gtk::Widget* w);
Gtk::Widget* find(std::string_view name);
void dump();
std::size_t size();

}  // namespace delr::registry
