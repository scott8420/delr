// AddCaseDialog -- arranges labels around a core::IntakeReport. No judgments
// here; see the header.
#include "AddCaseDialog.hpp"
#include "Log.hpp"

#include <gtkmm/eventcontrollerkey.h>

namespace delr {
namespace {

// The exposure axis, in the order a person reads their own listing: who you
// are, then how to reach you, then who you know.
const std::pair<core::Field, const char*> kFields[] = {
    {core::Field::Name,           "Name"},
    {core::Field::Aliases,        "Aliases"},
    {core::Field::Age,            "Age"},
    {core::Field::Dob,            "Date of birth"},
    {core::Field::Address,        "Address"},
    {core::Field::AddressHistory, "Address history"},
    {core::Field::Phone,          "Phone"},
    {core::Field::Email,          "Email"},
    {core::Field::Relatives,      "Relatives"},
    {core::Field::Employer,       "Employer"},
    {core::Field::Other,          "Other"},
};

}  // namespace

AddCaseDialog::AddCaseDialog()
    : m_root("addcase.root", Gtk::Orientation::VERTICAL, 8),
      m_broker_row("addcase.broker.row", Gtk::Orientation::HORIZONTAL, 8),
      m_buttons("addcase.buttons", Gtk::Orientation::HORIZONTAL, 8),
      m_prompt("addcase.prompt"),
      m_verdict("addcase.verdict"),
      m_broker_label("addcase.broker.label"),
      m_exposes_label("addcase.exposes.label"),
      m_url("addcase.url"),
      m_note("addcase.note"),
      m_broker("addcase.broker"),
      m_exposes("addcase.exposes"),
      m_cancel("addcase.cancel", "Cancel"),
      m_add("addcase.add", "Add case") {
    set_name("addcase");
    build();
    bind();
}

AddCaseDialog::~AddCaseDialog() = default;

void AddCaseDialog::build() {
    set_title("Add a case");
    set_default_size(560, -1);
    set_modal(true);
    // Hide, don't destroy: the window outlives its closings (see the header).
    set_hide_on_close(true);

    m_root.set_margin(16);

    m_prompt.set_text(
        "Paste the address of a listing you found in your own browser. "
        "delr does not search brokers for you.");
    m_prompt.set_wrap(true);
    m_prompt.set_xalign(0.0f);
    m_prompt.add_css_class("dim-label");

    m_url.set_placeholder_text("https://…");
    m_url.set_hexpand(true);
    // The URL is PII, and an input-purpose of URL is what tells the platform
    // not to offer it to a spell-checker or an autofill store.
    m_url.set_input_purpose(Gtk::InputPurpose::URL);

    m_verdict.set_wrap(true);
    m_verdict.set_xalign(0.0f);

    m_broker_label.set_text("Broker");
    m_broker.set_hexpand(true);
    m_broker_names = Gtk::StringList::create({});
    m_broker.set_model(m_broker_names);
    m_broker_row.append(m_broker_label);
    m_broker_row.append(m_broker);

    m_exposes_label.set_text("What does the listing show about you?");
    m_exposes_label.set_xalign(0.0f);
    m_exposes.set_selection_mode(Gtk::SelectionMode::NONE);
    m_exposes.set_max_children_per_line(4);
    m_exposes.set_row_spacing(2);
    m_exposes.set_column_spacing(8);
    for (const auto& f : kFields) {
        // Registered, not transient: these exist for the window's whole life
        // and a name that resolves in the registry is worth having on each.
        auto* cb = Gtk::make_managed<widgets::CheckButton>(
            std::string("addcase.expose.") + core::field_name(f.first), f.second);
        m_exposes.append(*cb);
        m_field_boxes.emplace_back(f.first, cb);
    }

    m_note.set_placeholder_text("Note (optional) — stays on this machine");

    m_add.add_css_class("suggested-action");
    m_add.set_sensitive(false);
    m_buttons.set_halign(Gtk::Align::END);
    m_buttons.append(m_cancel);
    m_buttons.append(m_add);

    m_root.append(m_prompt);
    m_root.append(m_url);
    m_root.append(m_verdict);
    m_root.append(m_broker_row);
    m_root.append(m_exposes_label);
    m_root.append(m_exposes);
    m_root.append(m_note);
    m_root.append(m_buttons);
    set_child(m_root);
}

void AddCaseDialog::bind() {
    m_url.signal_changed().connect(sigc::mem_fun(*this, &AddCaseDialog::refresh));
    m_url.signal_activate().connect([this] { if (m_add.get_sensitive()) on_add(); });
    m_note.signal_activate().connect([this] { if (m_add.get_sensitive()) on_add(); });

    m_broker.property_selected().signal_changed().connect([this] {
        if (m_setting) return;          // our own selection, not the user's
        m_broker_pinned = true;         // from here the user's choice wins
        refresh();
    });

    m_cancel.signal_clicked().connect([this] { set_visible(false); });
    m_add.signal_clicked().connect(sigc::mem_fun(*this, &AddCaseDialog::on_add));
    signal_hide().connect(sigc::mem_fun(*this, &AddCaseDialog::on_hidden));

    // Escape closes. A plain Gtk::Window has no such binding of its own, and a
    // modal window you cannot dismiss with the key everyone reaches for is a
    // trap.
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
            if (keyval == GDK_KEY_Escape) { set_visible(false); return true; }
            return false;
        }, false);
    add_controller(keys);
}

void AddCaseDialog::open(Gtk::Window& parent, const core::Roster& roster,
                         const core::Caseload& caseload, const std::string& today) {
    m_roster   = roster;      // snapshots -- see the header
    m_caseload = caseload;
    m_today    = today;

    // Rebuild the broker list from the roster as it is right now: it may have
    // been reloaded from disk since the last time this window opened.
    m_broker_ids.clear();
    std::vector<Glib::ustring> names;
    for (const auto& b : m_roster) {
        m_broker_ids.push_back(b.id);
        names.push_back(b.name.empty() ? b.id : b.name);
    }
    m_broker_names = Gtk::StringList::create(names);
    m_setting = true;
    m_broker.set_model(m_broker_names);
    m_broker.set_selected(GTK_INVALID_LIST_POSITION);
    m_setting = false;
    m_broker_pinned = false;

    set_transient_for(parent);
    refresh();
    present();
    m_url.grab_focus();
}

const core::Broker* AddCaseDialog::chosen_broker() const {
    const guint sel = m_broker.get_selected();
    if (sel == GTK_INVALID_LIST_POSITION || sel >= m_broker_ids.size()) return nullptr;
    return core::roster_find(m_roster, m_broker_ids[sel]);
}

void AddCaseDialog::select_broker(const std::string& id) {
    for (std::size_t i = 0; i < m_broker_ids.size(); ++i) {
        if (m_broker_ids[i] != id) continue;
        if (m_broker.get_selected() == i) return;
        m_setting = true;
        m_broker.set_selected(static_cast<guint>(i));
        m_setting = false;
        return;
    }
}

// The whole of the dialog's logic: ask Intake, then say what it said.
void AddCaseDialog::refresh() {
    const std::string raw = std::string(m_url.get_text());
    const auto rep = core::intake_inspect(m_roster, m_caseload, raw);

    // Follow the matcher until the user overrules it.
    if (!m_broker_pinned && rep.broker) select_broker(rep.broker->id);

    const core::Broker* chosen = chosen_broker();
    std::string say;
    bool can_add = false;

    if (rep.problem == core::UrlProblem::Empty) {
        say = "";
    } else if (rep.problem != core::UrlProblem::None) {
        say = core::url_problem_text(rep.problem);
    } else if (rep.duplicate()) {
        // Named by id, never by URL -- this label is on screen and screenshots
        // happen (the same reason the Cases page shows the host and not the path).
        say = "Already tracked as case " + rep.existing->id + ". Nothing to add.";
    } else if (rep.relist) {
        say = "Case " + rep.existing->id + " on this listing is marked removed, so this "
              "is a relist. Adding it opens a new case that supersedes it and ends that "
              "one as relisted — both facts stay on the record.";
        can_add = true;
    } else if (rep.broker) {
        say = std::string("Matched ") + rep.broker->name + " from " + rep.host + ".";
        can_add = chosen != nullptr;
    } else if (chosen) {
        say = rep.host + " is not in the roster — filing under " + chosen->name + ".";
        can_add = true;
    } else {
        say = rep.host + " is not in the roster. Choose the broker below.";
    }

    // A relist is filed under the old case's broker, so the dropdown does not
    // gate it.
    if (rep.relist) can_add = true;

    m_verdict.set_text(say);
    m_add.set_label(rep.relist ? "Open relist case" : "Add case");
    m_add.set_sensitive(can_add);
}

void AddCaseDialog::on_add() {
    const std::string raw = std::string(m_url.get_text());
    auto rep = core::intake_inspect(m_roster, m_caseload, raw);
    if (rep.problem != core::UrlProblem::None || rep.duplicate()) return;

    core::Case fresh;
    if (rep.relist) {
        fresh = core::intake_relist_case(rep, m_caseload, m_today);
    } else {
        // The user's choice is the filing, matched or not.
        rep.broker = chosen_broker();
        if (!rep.broker) return;

        std::vector<core::Field> exposes;
        for (const auto& fb : m_field_boxes)
            if (fb.second->get_active()) exposes.push_back(fb.first);

        fresh = core::intake_new_case(rep, m_caseload, m_today, exposes,
                                      std::string(m_note.get_text()));
    }
    if (fresh.id.empty()) return;   // intake refused; nothing to hand on

    if (auto lg = log::get(log::Area::Cases))
        lg->info("intake: new case {}{}", core::log_ref(fresh),
                 fresh.supersedes.empty() ? "" : " (relist)");

    m_committed.emit(fresh);
    set_visible(false);             // clears, via on_hidden
}

// Hiding clears. What was typed here is a URL with a name in it, and the copy
// of the caseload taken at open() is the whole PII table; neither has any
// business sitting in a hidden window until the next time it opens.
void AddCaseDialog::on_hidden() {
    m_url.set_text("");
    m_note.set_text("");
    for (const auto& fb : m_field_boxes) fb.second->set_active(false);
    m_verdict.set_text("");
    m_add.set_sensitive(false);
    m_caseload.clear();
    m_roster.clear();
    m_broker_ids.clear();
    m_setting = true;
    m_broker_names = Gtk::StringList::create({});
    m_broker.set_model(m_broker_names);
    m_setting = false;
    m_broker_pinned = false;
}

}  // namespace delr
