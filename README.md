# delr

A local-first tool for getting your personal data out of data brokers, and —
more to the point — for checking that it *stays* out.

Built on the disciplines in the working rules and CANON (kept in a separate
docs repo, deliberately not vendored here) and the Cairn seed: a GTK-free
core with the seam enforced by the build graph, a split-class Shell, a
mandatory-name widget substrate, and a headless selftest that has to pass
before anything ships.

## Why this exists

Commercial removal services sell clerical persistence, not technology. Strip
one down and it's three tables: **brokers** (who holds data and how they let
you out), **cases** (what you've asked, when, and where it stands), and your
**profile** (the identifiers a request is made against). Everything else is a
view over those.

The part nobody does is verification. A removal service reports its own
success; California's DROP platform reports what the broker self-declares.
Neither goes back to look at the live page in ninety days, and re-listing is
routine. That gap is the reason for this app.

## The division of labour

**Discovery is yours.** You find your own listing, in your own browser, with
your own fingers. Broker sites sit behind CAPTCHAs and bot defences, every
scraper needs its own parser, and every parser breaks on their schedule — and
querying a broker for yourself can be a signal to them rather than an
observation of them.

**Verification is the machine's.** Once a listing URL is known, checking
whether it's still live is cheap, robust, and needs doing every 45 days
forever. That's the job a computer should have.

**Your profile is a search key, not a submission.** delr needs to know what a
listing about you would print -- names you've gone by, places you've lived,
numbers, handles -- because a broker's page is only evidence about *you* if your
own details are on it. Those terms are kept on your machine, mode 0600, and
typing them sends nothing anywhere. Old details matter most: a broker's record is
a decade of accretion, and the address from three moves ago is often the one that
finds you. delr asks for a birth *year* and not a date of birth, because a year
disambiguates you from the other person with your name and that is the whole job.

## Build

```
./setup-dev.sh     # Debian/Ubuntu dev packages (one time)
./build.sh         # configure + build
./run.sh           # build, then start the app
./build/delr --selftest    # the core's checks, on demand
./build/delr --netcheck                                # preflight the saved policy
./build/delr --netcheck wg0 socks5h://127.0.0.1:1080   # or an ad-hoc one
./build/delr --import-registry data/registry/cppa_registry2025.csv   # rebuild the roster
```

`--netcheck` runs the whole preflight -- against the saved policy when given no
arguments, or a named interface when given one -- and reports what it found
**without printing a single address** -- not the exit, not the tunnel's
own, not the resolver. It is the thing to paste into a bug report when a check
will not run.

`--import-registry` rebuilds `data/brokers.json` from California's data broker
registry export. **You do not have to trust the roster shipped here** -- download
the state's CSV yourself and run the command; the transformation is in the tree,
it is checked, and it merges rather than overwrites, so anything you have added
or annotated survives.

One binary. The core's checks ride in it behind `--selftest` rather than a
second executable; they run when you ask for them, not as part of the build.

gtkmm is **optional at configure time**: without it you still get `delr_core`,
so the library builds anywhere, but the app and its checks need the full stack.

## Status

Stub. What exists and is exercised:

| Piece | Where | State |
|---|---|---|
| Broker roster + JSON pump + validation | `include/core/Broker.hpp`, `src/core/Broker.cpp` | exercised |
| Registry importer: CSV -> roster, merged not overwritten | `include/core/RosterImport.hpp`, `src/core/RosterImport.cpp` | exercised; **544 brokers, 553 listing domains** |
| Caseload: status/outcome/provenance, dates, scheduling, exposure roll-up | `include/core/Case.hpp`, `src/core/Case.cpp` | exercised |
| Promotion: when a listing is believed gone, and when it has come back | `include/core/Case.hpp`, `src/core/Case.cpp` | exercised |
| Intake: URL parse, broker match by host, id minting, duplicate + relist detection | `include/core/Intake.hpp`, `src/core/Intake.cpp` | exercised |
| Egress policy: bind, preflight identity, DNS mode, one named verdict | `include/core/Egress.hpp`, `src/core/Egress.cpp` | exercised |
| A DNS mode an ordinary VPN account can meet, with the leak test as its guarantee | `include/core/Egress.hpp`, `src/core/Probe.cpp` | exercised |
| Page rules: what a fetched page has to say for a listing to count as gone | `include/core/PageRules.hpp`, `src/core/PageRules.cpp` | exercised |
| Profile: the terms a listing about you would print, and the needles derived from them | `include/core/Profile.hpp`, `src/core/Profile.cpp` | exercised |
| The observer's judgment: readings -> observation | `include/core/Probe.hpp`, `src/core/Probe.cpp` | exercised |
| The verification fetch, over libcurl, gated by the policy | `include/net/Fetch.hpp`, `src/net/Fetch.cpp` | exercised; fetches real pages |
| The syscall shim: getifaddrs, bind, routes, echo, canary | `include/net/Observer.hpp`, `src/net/Observer.cpp` | exercised; runs a real preflight |
| Egress policy on disk, 0600 because it holds your own address | `src/core/Egress.cpp` | exercised; round-trips |
| Recording your no-tunnel baseline -- your own address AND who answers your lookups | `src/net/Observer.cpp` | exercised (its refusals); one real request |
| Page rules loaded and validated against the roster | `src/Shell_handlers.cpp` | exercised |
| The check, wired to a button: preflight, fetch, read the page, record it | `src/Shell_work.cpp` | compiles; **unverified visually** |
| The maintenance queue -- listings fetched and not readable | `src/Shell_handlers.cpp` | compiles; **unverified visually** |
| Follows the desktop light/dark preference | `include/Appearance.hpp`, `src/Appearance.cpp` | working |
| Core checks (`delr --selftest`) | `src/selftest.cpp` | 772 pass / 0 fail, run on demand |
| One real preflight, printing no addresses (`delr --netcheck [wg0]`) | `src/netcheck.cpp` | works, against the saved policy or an ad-hoc one |
| Tunnel settings: interface, lookups, proxy, trusted exits, baseline, preflight | `include/EgressDialog.hpp`, `src/EgressDialog.cpp` | compiles; **unverified visually** |
| App shell, sidebar + stack, roster page | `src/Shell*.cpp` | compiles; **unverified visually** |
| Cases page: status line, exposure roll-up, maintenance line, case list | `src/Shell_zones.cpp`, `src/Shell_handlers.cpp` | compiles; **unverified visually** |
| Add a case: paste a URL, pick the broker, tick what it exposes | `include/AddCaseDialog.hpp`, `src/AddCaseDialog.cpp` | compiles; **unverified visually** |

The GUI has been compiled but never *seen* — there's no display in the
environment it was built in. Compiling is one channel of evidence; eyes are the
other, and only the case where both agree counts as working.

## Next

Opt-out filing by email, which the registry makes the primary
channel · a journal that says loudly when nothing has run · scheduled runs over
the whole due queue, rate limited between brokers.

**The roster is 543 registrants; it is not 543 checkable listings.** California
compels the list, so the roster is now complete and current on who holds data and
where to write to them. Most of those companies are bulk data brokers with no
page a person appears on -- there is nothing to fetch and nothing to verify. The
verification half of this program reaches the people-search subset, and reaching
even those needs page rules a human has to write by reading live pages. That work
is still the gate, and it is the only part of this no session can do.

**A check needs your own details before it can confirm a presence.** A rule with
`needs_needle` asks that the listing's own identifiers appear on the page, which
is what turns "this looks like a profile page" into "this is YOUR profile page".
Until profile storage lands, those brokers answer `NoNeedles` -- indeterminate,
never a removal, and shown in the maintenance line rather than counted as clean.

**Name lookups need a SOCKS5 proxy.** The stock Debian/Ubuntu libcurl is built
with the threaded resolver rather than c-ares, so `CURLOPT_DNS_SERVERS` returns
`CURLE_NOT_BUILT_IN` and a pinned resolver silently would not take effect. A
pinned resolver that did not pin is the host resolver, which is refused by
design -- so this app refuses it too, loudly, rather than fetching anyway. That
leaves `socks5h://`, where the proxy resolves and the name never leaves this
machine at all. Most commercial VPNs publish a SOCKS5 endpoint; an `ssh -D` also
works. `delr --netcheck` reports whether your libcurl can pin, and the settings window
says so plainly rather than letting you choose something that would not take
effect.

The check goes out through a tunnel or it does not go out. `core/Egress` decides
that as a pure function of a configured policy and an observation somebody else
made, so the whole leak-or-not decision is exercised headless, before any socket
exists to argue with it. Binding to the tunnel is the killswitch — a dead tunnel
*fails* instead of falling back to the default route — and the preflight answers
the question binding cannot: is this the tunnel I meant? A refusal is
`Indeterminate` + `NoTunnel`, which never rounds to "not found", and never counts
against the listing.

Believing a record is gone is its own act, kept apart from recording what a
fetch saw: two consecutive **clean absences** (a page that loaded and did not
have you in it — never a 404, which is a retired slug and not a removal), or one
plus a broker's claim, which is two independent sources rather than one source
twice. Our own fetch then overwrites the claim it agrees with. A `Removed` case
that fetches `Listed` again is the event the whole app exists to catch, and it
opens a successor case rather than editing the old one.

Two invariants the model already enforces, because they cannot be retrofitted
into a report: **indeterminate never rounds to not-found**, and **who says a
record is gone is a field**, not a footnote. A broker's claim, a state
platform's relay of that claim, and our own fetch of the live page are three
different weights of evidence.

## Non-negotiables

- **No telemetry, ever.** The roster channel is download-only. Nothing about
  what you searched, found, or removed leaves your machine. An app that phoned
  home with these results would be a data broker with excellent targeting.
- **No PII in logs.** Log the broker id, the field name, the outcome — never
  the value being matched.
- **Your own data only.** This is not a people-search tool.

## Licence

MIT. See `LICENSE`.
