#include "core/Compose.hpp"

#include <algorithm>

namespace delr::core {
namespace {

// A block of lines under a heading, or nothing at all when the list is empty.
// Nothing at all rather than an empty heading: "Places associated with this
// record:" followed by white space reads as a request that lost something in
// transit, and the recipient is a stranger with no way to ask.
void append_list(std::string& body, const char* heading,
                 const std::vector<std::string>& items) {
    if (items.empty()) return;
    body += "\n";
    body += heading;
    body += "\n";
    for (const auto& s : items) body += "    - " + s + "\n";
}

// The other addresses, with the reply address removed -- listing the address
// the letter already asks them to reply to would be padding a disclosure with
// a fact stated twice.
std::vector<std::string> other_emails(const Profile& p) {
    std::vector<std::string> out;
    for (const auto& e : p.emails)
        if (e != p.contact_email) out.push_back(e);
    return out;
}

void add(std::vector<Caution>& v, Caution c) {
    if (std::find(v.begin(), v.end(), c) == v.end()) v.push_back(c);
}

}  // namespace

const char* caution_name(Caution c) {
    switch (c) {
        case Caution::NoName:               return "no-name";
        case Caution::NoChannel:            return "no-channel";
        case Caution::ChannelNotComposable: return "channel-not-composable";
        case Caution::NoContactEmail:       return "no-contact-email";
        case Caution::NoResidency:          return "no-residency";
        case Caution::NoStatute:            return "no-statute";
        case Caution::NoCitation:           return "no-citation";
        case Caution::WebFormOnly:          return "web-form-only";
        case Caution::RequiresId:           return "requires-id";
        case Caution::FcraRegulated:        return "fcra-regulated";
        case Caution::DropCovered:          return "drop-covered";
        case Caution::DropNotYours:         return "drop-not-yours";
        case Caution::DisclosesExtra:       return "discloses-extra";
        case Caution::UnverifiedListing:    return "unverified-listing";
    }
    return "unknown";
}

const char* caution_text(Caution c) {
    switch (c) {
        case Caution::NoName:
            return "Your profile has no name on it, so this request cannot say "
                   "whose record it is about. Fill in the You page first.";
        case Caution::NoChannel:
            return "The roster holds no opt-out address for this broker — no "
                   "email, no form. (It carries no postal addresses at all.) "
                   "There is nowhere for this request to go until one is added.";
        case Caution::ChannelNotComposable:
            return "This broker takes opt-outs by telephone. A letter is not a "
                   "phone call, and delr will not pretend otherwise — the text "
                   "below is a script at best.";
        case Caution::NoContactEmail:
            return "Your profile has no contact address, so there is nowhere "
                   "for them to reply. A request nobody can answer is a request "
                   "you cannot follow up.";
        case Caution::NoResidency:
            return "Your profile does not say where you live, so delr cannot "
                   "tell which law you are entitled to invoke. This goes out as "
                   "a courtesy request.";
        case Caution::NoStatute:
            return "delr has no deletion law on file for your jurisdiction, so "
                   "this asks rather than demands. That is not nothing — most "
                   "brokers honour a plain request — but there is no deadline "
                   "behind it and none is claimed.";
        case Caution::NoCitation:
            return "The request names the act but cites no section number, "
                   "because nobody has verified one for this jurisdiction yet. "
                   "Naming it is honest; inventing a section would not be.";
        case Caution::WebFormOnly:
            return "This broker takes requests through a web form. The text "
                   "below is what to paste into it — expect the form to ask for "
                   "some of the same facts in separate fields.";
        case Caution::RequiresId:
            return "This broker demands a photograph of a government ID before "
                   "acting. Uploading one to a data broker is a real cost and "
                   "the decision is yours; delr will not help you do it.";
        case Caution::FcraRegulated:
            return "This broker is regulated under the FCRA and may LAWFULLY "
                   "refuse to delete. A refusal here is a correct outcome, not "
                   "a failed request.";
        case Caution::DropCovered:
            return "This broker is registered with California's deletion "
                   "platform, which covers you. Filing there reaches every "
                   "registered broker at once and is usually the better move; "
                   "this request is the direct route, not a replacement.";
        case Caution::DropNotYours:
            return "This broker is registered with California's deletion "
                   "platform, which does not cover you. The direct request "
                   "below is the route you have.";
        case Caution::DisclosesExtra:
            return "This request includes details beyond what the listing "
                   "already shows. That may be what makes the record findable "
                   "— it is also information you are handing to a data broker. "
                   "Read it before you send it.";
        case Caution::UnverifiedListing:
            return "delr has never fetched this page cleanly, so it cannot "
                   "confirm the record is there or that it is yours. This may "
                   "tell a broker your name and your address to no purpose.";
    }
    return "";
}

bool caution_blocking(Caution c) {
    return c == Caution::NoName || c == Caution::NoChannel ||
           c == Caution::ChannelNotComposable;
}

bool options_disclose_extra(const ComposeOptions& o) {
    return o.include_aliases || o.include_places || o.include_phones ||
           o.include_emails;
}

// Declared first, then what is actually on hand. A broker declaring `email`
// with an empty `opt_out_email` has no email channel whatever the column says.
Method compose_channel(const Broker& b) {
    if (b.method == Method::Email && !b.opt_out_email.empty()) return Method::Email;
    if (b.method == Method::Web   && !b.opt_out_url.empty())   return Method::Web;

    // Both are declarations this program cannot act on, and both are reported
    // rather than papered over: there is no postal address anywhere in the
    // roster's schema, and a telephone call is not a document.
    if (b.method == Method::Postal) return Method::Postal;
    if (b.method == Method::Phone)  return Method::Phone;

    // `Drop`, `Unknown`, or a declaration with nothing behind it. Use whatever
    // the row does hold -- an address that exists beats a method that was
    // declared.
    if (!b.opt_out_email.empty()) return Method::Email;
    if (!b.opt_out_url.empty())   return Method::Web;
    if (b.method == Method::Drop) return Method::Drop;
    return Method::Unknown;
}

bool request_has(const Request& r, Caution c) {
    return std::find(r.cautions.begin(), r.cautions.end(), c) != r.cautions.end();
}

std::string compose_log_ref(const Request& r) {
    return std::string("request:") + method_name(r.channel) + "/" +
           std::to_string(r.cautions.size()) + " cautions" +
           (r.sendable ? "" : ", not sendable");
}

Request compose_request(const Profile& p, const Broker& b, const Case& k,
                        const Statute* law, const std::string& today,
                        const ComposeOptions& opt) {
    Request r;
    r.channel = compose_channel(b);

    switch (r.channel) {
        case Method::Email: r.to = b.opt_out_email; break;
        case Method::Web:   r.to = b.opt_out_url;   break;
        default:            r.to.clear();           break;
    }

    // ── Cautions, gathered before a word is written ─────────────────────────
    // Composed anyway when they are blocking: the text is still worth showing
    // a user who wants to see what delr WOULD have sent, and `sendable` is the
    // field that stops it going out. A window that produced an empty box and
    // no explanation would be the app refusing without saying what it refused.
    if (p.full_name.empty())     add(r.cautions, Caution::NoName);
    if (p.contact_email.empty()) add(r.cautions, Caution::NoContactEmail);

    if (r.channel == Method::Web)                       add(r.cautions, Caution::WebFormOnly);
    if (r.channel == Method::Phone)                     add(r.cautions, Caution::ChannelNotComposable);
    if (r.channel == Method::Postal ||
        r.channel == Method::Drop  ||
        r.channel == Method::Unknown)                   add(r.cautions, Caution::NoChannel);

    if (b.requires_id)     add(r.cautions, Caution::RequiresId);
    if (b.fcra_regulated)  add(r.cautions, Caution::FcraRegulated);

    // The state platform, and which side of it the user is on. Two cautions
    // and not one: "use the platform instead" and "the platform will not help
    // you" are opposite instructions.
    if (b.method == Method::Drop) {
        if (p.residency == "US-CA") add(r.cautions, Caution::DropCovered);
        else                        add(r.cautions, Caution::DropNotYours);
    }

    if (p.residency.empty()) add(r.cautions, Caution::NoResidency);
    if (!law)                add(r.cautions, Caution::NoStatute);
    else if (law->citation.empty()) add(r.cautions, Caution::NoCitation);

    if (options_disclose_extra(opt)) add(r.cautions, Caution::DisclosesExtra);

    // The listing itself. `Outcome::Listed` is the only value that means we
    // fetched the page and the record was on it; everything else is a case we
    // have not seen with our own eyes, including one that has never been
    // checked at all.
    if (k.outcome != Outcome::Listed) add(r.cautions, Caution::UnverifiedListing);

    if (law) {
        r.statute_id   = law->id;
        r.respond_days = law->respond_days;
    }

    // Blocking first, then declaration order. Explicit rather than a plain
    // sort on the enum value, so reordering the enum some later session
    // cannot quietly bury "there is nowhere to send this".
    std::stable_sort(r.cautions.begin(), r.cautions.end(),
                     [](Caution a, Caution c) {
                         const int ka = caution_blocking(a) ? 0 : 1;
                         const int kc = caution_blocking(c) ? 0 : 1;
                         if (ka != kc) return ka < kc;
                         return static_cast<int>(a) < static_cast<int>(c);
                     });

    r.sendable = std::none_of(r.cautions.begin(), r.cautions.end(),
                              caution_blocking);

    // ── The text ────────────────────────────────────────────────────────────
    const std::string who = p.full_name.empty() ? "(no name on file)" : p.full_name;

    r.subject = "Request to delete personal information — " + who;

    std::string body;
    body += "To whom it concerns,\n\n";
    body += "I am asking " + (b.name.empty() ? std::string("you") : b.name) +
            " to delete the personal information you hold about me";
    if (!k.url.empty())
        body += ", including the record published at:\n\n    " + k.url + "\n";
    else
        body += ".\n";

    body += "\nThe name on that record is " + who + ".\n";

    // ── The optional disclosures ────────────────────────────────────────────
    // Off unless the user turned them on. See the header: each of these is a
    // fact the broker did not print and now has.
    if (opt.include_aliases)
        append_list(body, "This record may also be filed under:", p.also_known_as);
    if (opt.include_places)
        append_list(body, "Places associated with this record:", p.places);
    if (opt.include_phones)
        append_list(body, "Telephone numbers associated with this record:", p.phones);
    if (opt.include_emails)
        append_list(body, "Other email addresses associated with this record:",
                    other_emails(p));

    body += "\n";

    // ── What this request stands on ─────────────────────────────────────────
    // The region code rather than the state's name, and deliberately: a
    // fifty-row lookup table whose only job is to make one sentence read more
    // smoothly is a table that rots, and "I am a resident of TN" is a sentence
    // every compliance desk in the country reads twenty times a day.
    if (law) {
        if (law->requires_residency_statement && !p.residency.empty())
            body += "I am a resident of " + jurisdiction_region(p.residency) + ". ";
        body += "I make this request under the " + law->name;
        if (!law->citation.empty()) body += " (" + law->citation + ")";
        body += ".\n";
    } else {
        // No law, and the letter says what it is instead of dressing up.
        body += "I make this request under your published privacy policy and "
                "opt-out procedure.\n";
    }

    body += "\nPlease delete the underlying personal information rather than "
            "only suppressing the record from search results, and confirm in "
            "writing once it has been done.";
    if (r.respond_days > 0) {
        const std::string act = (law && !law->short_name.empty()) ? law->short_name
                                                                  : "the act above";
        body += " " + act + " requires a response within " +
                std::to_string(r.respond_days) + " days.";
    }
    body += "\n";

    if (!p.contact_email.empty())
        body += "\nPlease reply to " + p.contact_email + ".\n";

    body += "\n" + who + "\n";
    if (date_valid(today)) body += today + "\n";

    r.body = std::move(body);
    return r;
}

}  // namespace delr::core
