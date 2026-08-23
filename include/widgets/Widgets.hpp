#pragma once
#include "widgets/Named.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/stack.h>
#include <gtkmm/stacksidebar.h>
#include <gtkmm/textview.h>

// Widen on demand, not for symmetry (CANON).
namespace delr::widgets {

using Box            = Named<Gtk::Box>;
using Button         = Named<Gtk::Button>;
using CheckButton    = Named<Gtk::CheckButton>;
using DropDown       = Named<Gtk::DropDown>;
using Entry          = Named<Gtk::Entry>;
using FlowBox        = Named<Gtk::FlowBox>;
using Label          = Named<Gtk::Label>;

// A label that REPORTS rather than explains -- a verdict, a status line, a
// preflight result, the sentence after a check.
//
// Selectable and wrapping by construction, and that is the whole type. A
// `set_selectable(true)` at each of the thirteen sites that need it is thirteen
// chances to forget one, and the one that gets forgotten is always the one
// somebody needs to paste into a bug report at the moment they need it. Same
// reasoning as `Named<W>` itself: make the discipline unskippable by putting it
// in the constructor rather than in a convention.
//
// NOT a blanket change to `Label`. Static explanatory prose is not evidence,
// selecting it by accident while clicking is a nuisance, and every selectable
// label is another focus stop between the user and the button they wanted. The
// distinction the type draws is exactly the one that matters: if the app
// OBSERVED it, the user can copy it.
class Reported : public Named<Gtk::Label> {
public:
    template <class... Args>
    explicit Reported(std::string_view name, Args&&... args)
        : Named<Gtk::Label>(name, std::forward<Args>(args)...) { shape(); }

    template <class... Args>
    explicit Reported(unregistered_t u, std::string_view name, Args&&... args)
        : Named<Gtk::Label>(u, name, std::forward<Args>(args)...) { shape(); }

private:
    void shape() {
        set_selectable(true);
        set_wrap(true);
        set_xalign(0.0f);
        // A selectable label takes focus, and GTK gives a focused label a
        // visible caret. Harmless while reading, but it means these must not
        // be the first thing focused when a window opens -- see the note in
        // Shell.hpp about who gets initial focus.
    }
};
using ListBox        = Named<Gtk::ListBox>;
using MenuButton     = Named<Gtk::MenuButton>;
using ScrolledWindow = Named<Gtk::ScrolledWindow>;
using Separator      = Named<Gtk::Separator>;
using Stack          = Named<Gtk::Stack>;
using StackSidebar   = Named<Gtk::StackSidebar>;
// The profile is five lists of strings the user types one per line, and a
// multi-line box is what that IS. Widened on demand, like everything above it.
using TextView       = Named<Gtk::TextView>;

}  // namespace delr::widgets
