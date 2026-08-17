#pragma once
#include "Registry.hpp"
#include <gtkmm/widget.h>
#include <string>
#include <string_view>
#include <utility>

// Named<W> -- the mandatory-name widget wrapper (CANON: discipline through
// inheritance). A GTK name is optional and easy to skip; Named<W> makes it
// mandatory in the only constructor and registers by construction, so an
// unnamed widget simply isn't expressible in app code.
namespace delr::widgets {

// Opt-out tag for transient per-row content: named for the Inspector, NOT
// entered in the registry (CANON: the unregistered_t primitive).
struct unregistered_t { explicit unregistered_t() = default; };
inline constexpr unregistered_t unregistered{};

template <class W>
class Named : public W {
public:
    template <class... Args>
    explicit Named(std::string_view name, Args&&... args)
        : W(std::forward<Args>(args)...) {
        this->set_name(std::string(name));
        registry::add(name, this);
        m_registered = true;
    }

    template <class... Args>
    explicit Named(unregistered_t, std::string_view name, Args&&... args)
        : W(std::forward<Args>(args)...) {
        this->set_name(std::string(name));
    }

    ~Named() override { if (m_registered) registry::remove(this); }

private:
    bool m_registered = false;
};

}  // namespace delr::widgets
