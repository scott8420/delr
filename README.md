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

## Build

```
./setup-dev.sh     # Debian/Ubuntu dev packages (one time)
./build.sh         # configure + build
./run.sh           # build, then start the app
./build/delr --selftest    # the core's checks, on demand
```

One binary. The core's checks ride in it behind `--selftest` rather than a
second executable; they run when you ask for them, not as part of the build.

gtkmm is **optional at configure time**: without it you still get `delr_core`,
so the library builds anywhere, but the app and its checks need the full stack.

## Status

Stub. What exists and is exercised:

| Piece | Where | State |
|---|---|---|
| Broker roster + JSON pump + validation | `include/core/Broker.hpp`, `src/core/Broker.cpp` | exercised |
| Caseload: status/outcome/provenance, dates, scheduling, exposure roll-up | `include/core/Case.hpp`, `src/core/Case.cpp` | exercised |
| Intake: URL parse, broker match by host, id minting, duplicate + relist detection | `include/core/Intake.hpp`, `src/core/Intake.cpp` | exercised |
| Follows the desktop light/dark preference | `include/Appearance.hpp`, `src/Appearance.cpp` | working |
| Core checks (`delr --selftest`) | `src/selftest.cpp` | 228 pass / 0 fail, run on demand |
| App shell, sidebar + stack, roster page | `src/Shell*.cpp` | compiles; **unverified visually** |
| Cases page: status line, exposure roll-up, case list | `src/Shell_zones.cpp`, `src/Shell_handlers.cpp` | compiles; **unverified visually** |
| Add a case: paste a URL, pick the broker, tick what it exposes | `include/AddCaseDialog.hpp`, `src/AddCaseDialog.cpp` | compiles; **unverified visually** |

The GUI has been compiled but never *seen* — there's no display in the
environment it was built in. Compiling is one channel of evidence; eyes are the
other, and only the case where both agree counts as working.

## Next

Egress policy (`core/Egress`: bind first, preflight, fail
closed) · the verification fetch itself · roster fetched from a hosted repo with
a baked-in fallback · profile storage, encrypted at rest.

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
