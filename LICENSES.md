# Third-party notices

delr itself is MIT (see `LICENSE`). This file records what it depends on and
what each dependency asks for, so the answer is on disk rather than in
somebody's memory when the first packaged build happens.

The distinction that governs all of it: **linking against a system library
distributes nothing.** Building on Debian/Ubuntu against the distro's
`libgtkmm-4.0-0`, `libspdlog1`, `libcurl4` carries no notice obligation at all —
the distro already ships those licences. The obligations below attach when a
build **bundles** a library: an AppImage, a Flatpak, a Windows or macOS
package, or any static link.

---

## Vendored — shipped in this repo today

### nlohmann/json — MIT

`third_party/nlohmann/json.hpp`. Header-only, included in the source tree, so
it is distributed with delr **now**, not only in a packaged build. MIT, same as
delr; the copyright notice at the top of the header is the notice, and it must
stay there. Kept `PRIVATE` on `delr_core` so no consumer inherits the include.

---

## Linked — system libraries

### libcurl — the curl licence (MIT/X11 derivative)

The transport, and the only producer `core/Egress`'s policy has:
`CURLOPT_INTERFACE` binds to the tunnel's address, `socks5h://` resolves names
at the far end.

Not copyleft, not dual-licensed, no commercial tier for the library. Two asks:

1. **Keep the copyright and permission notice** in copies of the software —
   i.e. include the curl licence text in any build that ships libcurl.
2. **Do not use the copyright holders' names to promote delr** without written
   permission. This forbids implying endorsement; it does not require
   attribution in marketing.

Nothing reaches delr's own source, and it is compatible with any licence delr
might take, GPL included. Commercial curl support contracts exist (wolfSSL) and
are entirely optional — there is no dual-licence trap.

**The TLS backend is where the licence surface actually varies**, not curl.
`setup-dev.sh` installs `libcurl4-openssl-dev` deliberately: OpenSSL 3.x is
Apache-2.0. The GnuTLS build is LGPL — fine to link dynamically, and the pair
to re-read before ever static-linking or bundling.

Worth stating for a privacy tool: **curl contacts nobody on its own.** No
telemetry, no auto-update check, no analytics. Every request delr makes is one
delr asked for, through the egress policy that decided it could leave.

### gtkmm-4.0 / GTK 4 — LGPL-2.1-or-later

Dynamically linked, which is what the LGPL is written for and imposes nothing
on delr's own MIT source. A bundled build must ship the LGPL text and leave the
library replaceable — dynamic linking satisfies that. `delr_core` does not link
it at all, by design: the seam is enforced by the build graph.

### spdlog — MIT

Dynamically linked against the system package. Bundling requires the notice.

### fmt — MIT

Arrives underneath spdlog. Same terms.

---

## Not a dependency

Nothing here talks to a network service on delr's behalf, and there is no
analytics, crash-reporting, or update-check library in the tree. That is a
design constraint, not an accident, and this section exists so that adding one
has to be a visible edit to this file.
