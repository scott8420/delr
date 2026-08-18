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
#include <gtkmm/stack.h>
#include <gtkmm/stacksidebar.h>

// Widen on demand, not for symmetry (CANON).
namespace delr::widgets {

using Box            = Named<Gtk::Box>;
using Button         = Named<Gtk::Button>;
using CheckButton    = Named<Gtk::CheckButton>;
using DropDown       = Named<Gtk::DropDown>;
using Entry          = Named<Gtk::Entry>;
using FlowBox        = Named<Gtk::FlowBox>;
using Label          = Named<Gtk::Label>;
using ListBox        = Named<Gtk::ListBox>;
using MenuButton     = Named<Gtk::MenuButton>;
using ScrolledWindow = Named<Gtk::ScrolledWindow>;
using Stack          = Named<Gtk::Stack>;
using StackSidebar   = Named<Gtk::StackSidebar>;

}  // namespace delr::widgets
