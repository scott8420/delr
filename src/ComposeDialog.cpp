// ComposeDialog -- arranges labels around a core::Request. No judgments here;
// see the header.
#include "ComposeDialog.hpp"
#include "Log.hpp"

#include <gdkmm/clipboard.h>

namespace delr {

ComposeDialog::ComposeDialog()
    : m_root("compose.root", Gtk::Orientation::VERTICAL, 8),
      m_options_row("compose.options.row", Gtk::Orientation::HORIZONTAL, 12),
      m_buttons("compose.buttons", Gtk::Orientation::HORIZONTAL, 8),
      m_prompt("compose.prompt"),
      m_options_label("compose.options.label"),
      m_edit_note("compose.edit.note"),
      m_no_send("compose.nosend"),
      m_channel_line("compose.channel"),
      m_cautions("compose.cautions"),
      m_standing("compose.standing"),
      m_body("compose.body"),
      m_body_scroll("compose.body.scroll"),
      m_aliases("compose.opt.aliases", "Other names"),
      m_places("compose.opt.places", "Places"),
      m_phones("compose.opt.phones", "Phone numbers"),
      m_emails("compose.opt.emails", "Other addresses"),
      m_close("compose.close", "Close"),
      m_copy("compose.copy", "Copy request"),
      m_sent("compose.sent", "I sent this") {
    set_name("compose");
    build();
    bind();
}

ComposeDialog::~ComposeDialog() = default;

void ComposeDialog::build() {
    set_title("Compose a request");
    set_default_size(680, 640);
    set_modal(true);
    set_hide_on_close(true);

    m_root.set_margin(16);

    m_prompt.set_wrap(true);
    m_prompt.set_xalign(0.0f);
    m_prompt.add_css_class("dim-label");
    m_prompt.set_text(
        "delr drafts this. You send it, from your own mail, in your own name — "
        "which is also what makes it a request they have to treat as yours.");

    m_options_label.set_xalign(0.0f);
    m_options_label.set_wrap(true);
    m_options_label.set_text("Include beyond what their page already shows:");

    // Off, and staying off. The default request tells a data broker nothing
    // they did not publish themselves; each of these hands them something new.
    m_options_row.append(m_aliases);
    m_options_row.append(m_places);
    m_options_row.append(m_phones);
    m_options_row.append(m_emails);

    m_edit_note.set_xalign(0.0f);
    m_edit_note.set_wrap(true);
    m_edit_note.add_css_class("dim-label");
    m_edit_note.set_text(
        "The text below is yours to edit before you send it. Changing an "
        "option above redraws it, and your edits go with it.");

    m_body.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    m_body.set_monospace(true);
    m_body_scroll.set_child(m_body);
    m_body_scroll.set_vexpand(true);
    m_body_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);

    // The three sentences the app produced rather than printed, and therefore
    // the three that have to be copyable.
    m_channel_line.set_margin_top(4);
    m_standing.add_css_class("dim-label");

    // The missing button, explained. A gap where Send should be teaches a user
    // that the feature is broken; a sentence teaches them what the program is.
    m_no_send.set_xalign(0.0f);
    m_no_send.set_wrap(true);
    m_no_send.set_text(
        "There is no Send button here, on purpose. delr holds no mail password "
        "and never will — the last inch is yours.");

    m_copy.set_tooltip_text("Copy the text below, as it now reads, to the clipboard");
    m_sent.set_tooltip_text(
        "Record that you sent this. delr cannot see that you did — this writes "
        "down that you said so, and starts the clock on their reply.");

    m_buttons.set_halign(Gtk::Align::END);
    m_buttons.append(m_close);
    m_buttons.append(m_copy);
    m_buttons.append(m_sent);

    m_root.append(m_prompt);
    m_root.append(m_channel_line);
    m_root.append(m_standing);
    m_root.append(m_cautions);
    m_root.append(m_options_label);
    m_root.append(m_options_row);
    m_root.append(m_edit_note);
    m_root.append(m_body_scroll);
    m_root.append(m_no_send);
    m_root.append(m_buttons);
    set_child(m_root);
}

void ComposeDialog::bind() {
    m_close.signal_clicked().connect([this] { set_visible(false); });
    m_copy.signal_clicked().connect(sigc::mem_fun(*this, &ComposeDialog::on_copy));
    m_sent.signal_clicked().connect(sigc::mem_fun(*this, &ComposeDialog::on_filed));
    signal_hide().connect(sigc::mem_fun(*this, &ComposeDialog::on_hidden));

    for (auto* c : {&m_aliases, &m_places, &m_phones, &m_emails})
        c->signal_toggled().connect([this] {
            if (!m_setting) recompose();
        });
}

void ComposeDialog::open(Gtk::Window& parent, const core::Profile& p,
                         const core::Broker& b, const core::Case& k,
                         const core::Statute* law, const std::string& today) {
    m_profile = p;
    m_broker  = b;
    m_case    = k;
    m_has_law = (law != nullptr);
    m_law     = law ? *law : core::Statute{};
    m_today   = today;

    // Every disclosure starts off on every open. Carrying a tick over from the
    // last case would let a choice made about one broker quietly apply to the
    // next one, which is the shape of an option nobody chose.
    m_setting = true;
    m_aliases.set_active(false);
    m_places.set_active(false);
    m_phones.set_active(false);
    m_emails.set_active(false);
    m_setting = false;

    // A box with nothing behind it is a box that lies about what it would do.
    m_aliases.set_sensitive(!m_profile.also_known_as.empty());
    m_places.set_sensitive(!m_profile.places.empty());
    m_phones.set_sensitive(!m_profile.phones.empty());
    m_emails.set_sensitive(m_profile.emails.size() >
                           (m_profile.contact_email.empty() ? 0u : 1u));

    set_transient_for(parent);
    recompose();
    present();
    // NOT a Reported label: those take focus and show a caret. The body is
    // where a person actually works.
    m_body.grab_focus();
}

void ComposeDialog::recompose() {
    core::ComposeOptions opt;
    opt.include_aliases = m_aliases.get_active();
    opt.include_places  = m_places.get_active();
    opt.include_phones  = m_phones.get_active();
    opt.include_emails  = m_emails.get_active();

    m_request = core::compose_request(m_profile, m_broker, m_case,
                                      m_has_law ? &m_law : nullptr,
                                      m_today, opt);

    const std::string who = m_broker.name.empty() ? m_broker.id : m_broker.name;
    std::string line;
    switch (m_request.channel) {
        case core::Method::Email:
            line = "By email to " + m_request.to + ".  Subject: " + m_request.subject;
            break;
        case core::Method::Web:
            line = "Through " + who + "'s own form: " + m_request.to;
            break;
        default:
            line = "There is no route to " + who + " in the roster.";
            break;
    }
    m_channel_line.set_text(line);

    // What it stands on, stated in the window as well as in the letter, so a
    // user can see the difference between a demand and a request before they
    // send one believing it is the other.
    if (!m_request.statute_id.empty() && m_request.respond_days > 0)
        m_standing.set_text("Filed under " + m_law.name + ". They owe an answer "
                            "within " + std::to_string(m_request.respond_days) +
                            " days of the date you send it.");
    else if (!m_request.statute_id.empty())
        m_standing.set_text("Filed under " + m_law.name + ", which states no "
                            "deadline delr knows of.");
    else
        m_standing.set_text("This asks under their own posted policy. There is "
                            "no law behind it and none is claimed.");

    std::string cautions;
    for (core::Caution c : m_request.cautions) {
        if (!cautions.empty()) cautions += "\n";
        cautions += std::string(core::caution_blocking(c) ? "• " : "· ") +
                    core::caution_text(c);
    }
    m_cautions.set_text(cautions);
    m_cautions.set_visible(!cautions.empty());

    m_body.get_buffer()->set_text(m_request.body);

    // Both buttons follow the same gate. Copying a request that cannot go
    // anywhere is harmless; recording that you SENT one that had nowhere to go
    // would put a false act in the file that exists to be evidence.
    m_sent.set_sensitive(m_request.sendable);
    m_copy.set_sensitive(!m_request.body.empty());

    if (auto lg = log::get(log::Area::Cases))
        lg->info("compose: case:{} {}", m_case.id, core::compose_log_ref(m_request));
}

std::string ComposeDialog::body_text() const {
    auto buf = m_body.get_buffer();
    return buf ? std::string(buf->get_text()) : std::string{};
}

void ComposeDialog::on_copy() {
    // What is in the box, edits included. There is no second, secret version.
    const std::string text = body_text();
    if (text.empty()) return;
    if (auto cb = get_clipboard()) cb->set_text(text);
    // Said out loud: a clipboard write is invisible and a user who is not sure
    // it happened will press the button again, or worse, retype the letter.
    m_channel_line.set_text("Copied. Paste it into your mail, check it once "
                            "more, and send it yourself.");
}

void ComposeDialog::on_filed() {
    if (!m_request.sendable) return;
    m_filed.emit(m_case.id, m_request.channel);
    set_visible(false);
}

void ComposeDialog::on_hidden() {
    // The most PII-dense string this program assembles does not survive the
    // close. See the header.
    if (auto buf = m_body.get_buffer()) buf->set_text("");
    m_channel_line.set_text("");
    m_cautions.set_text("");
    m_standing.set_text("");
    m_request = core::Request{};
    m_profile = core::Profile{};
    m_broker  = core::Broker{};
    m_case    = core::Case{};
    m_law     = core::Statute{};
    m_has_law = false;
    m_today.clear();
}

}  // namespace delr
