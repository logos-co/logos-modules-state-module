# modules_state

The read-only registry of module lifecycle state.

Today there are no module-lifecycle events in Logos at all: load, unload and
crash are spdlog lines inside `logos-liblogos`' `ModuleManager`, so every
consumer polls — `logos-basecamp` runs a 2-second timer and infers module state
from package-manager install events. This module is where that state becomes a
first-class, queryable, subscribable fact.

It **reports**. It does not drive load/unload — that stays with liblogos' C API.

## Status

The module is complete and standalone. The liblogos **registry observer** that
turns load/unload/crash/discovery into sequenced transitions is merged
(logos-liblogos#189); the **core push** that carries them across this wire is in
progress. Until that lands, nothing feeds this module in a normal run.

## Contract

```
type ModuleRecord {
  module: tstr, instance: ? tstr, pid: ? int, state: tstr, reason: ? tstr,
  path: tstr, type: tstr, version: tstr,
  dependencies: [tstr], dependents: [tstr], loadedAt: int, seq: uint
}
type ModuleListing { modules: [ModuleRecord], partial: bool, seq: uint }

# read surface — open to every module
method list_modules()              -> ModuleListing
method module_record(module: tstr) -> ?ModuleRecord
method is_ready(module: tstr)      -> bool

# ingest surface — admitted only when currentCaller() is the HOST
method note_transition(module, instance: ?tstr, pid: ?int,
                       old_state, new_state, reason: ?tstr, seq: uint) -> bool
method apply_snapshot(listing: ModuleListing) -> bool

event module_state_changed(module, instance: ?tstr, pid: ?int,
                           old_state, new_state, reason: ?tstr, seq: uint)
```

## The state vocabulary

Six **record** states — what a record from `list_modules` or `module_record`
may carry:

`unloaded`, `loading`, `loaded`, `ready`, `stopping`, `error`.

and one **event-only** state, which no record ever carries:

`absent`.

`absent` exists to name the two *membership edges*: `absent -> unloaded` when
the host discovers a module, `unloaded -> absent` when it prunes one. A
transition needs a state on both sides, so without it those two edges cannot be
events at all — and a consumer is back to inferring membership from
package-install events plus a settle timer, which is what
`logos-basecamp`'s `PackageCoordinator` does today and what this module exists
to replace.

It is not a record state. A module that is absent is simply **not in the
listing**, and `module_record` answers the empty optional. One spelling for "not
there", not two.

Six of the seven are reachable against liblogos as it stands. `ready` is not —
it becomes real when a module can report its own readiness. It ships anyway so
consumers written now are not rewritten then. That is also why the read surface
says `is_ready` and not `isLoaded`.

### Divergence from the draft core specs (logos-lips#317)

`spec-module-runtime.md` §3.4 spells the runtime vocabulary
`unloaded | loaded | ready | stopping | error` — no `absent`. Ours diverges by
exactly one **event-only transition target**, not by a sixth record state: a
consumer that reads records already sees only spec vocabulary. Separately,
`loading` is a record state we have and the drafts fold into `loaded`; that one
is still open.

**Normative forward-compatibility rule.** The wire type is `tstr` — LIDL has no
enum, no union, no string-literal constraint. A consumer **must** treat an
unrecognised state as forward-compatible and fall back to "not loaded". It must
not treat it as an error. Without this rule, the day `ready` starts being
emitted is the day every existing consumer breaks — the exact failure the full
vocabulary exists to prevent.

## Two things that are easy to get wrong

**`is_ready` answers "the host has it loaded and it has published".** It does
not answer "a call from *me* to it will succeed" — that additionally needs the
per-caller token handshake, which is per-caller and therefore not a fact this
module can hold. A caller that wants "can I call it" wants
`whenObjectAvailable()` on its own client.

**Subscribe with `onEventWhenAvailable`, not `requestObject` + `onEvent`.** The
plain subscription is one-shot, and it is refused whenever this module has not
*published* yet — which is not the same as "not loaded". `load-module` returns
when the plugin is in; publishing happens after, and `requestObject` refuses in
between:

```
LogosAPIConsumer::requestObject  ->  if (!m_transport->isConnected()) { qWarning(...); return nullptr; }
                                     logos-protocol, cpp/logos_api_consumer.cpp:610
```

`isConnected()` probes for a listener on the module's socket, so this is a
LIVENESS PROBE, not a handshake. It is deterministic, not a cold-start fluke:
subscribing to a module that is not up yet fails 3/3 warm, every time. The
generated typed wrapper
(`logos.modules_state.on("module_state_changed", …)`) uses
`onEventWhenAvailable` and arms a retry, so it is immune; only hand-rolled call
sites are exposed.

Two traps if you write one yourself:

* `onEventWhenAvailable` **refuses an empty event name**
  (`logos_api_consumer.cpp:558`), while `LogosObject::onEvent` reads empty as
  *wildcard*. Swapping one for the other silently kills the subscribe-to-all
  form. Route a wildcard through `whenObjectAvailable()` instead.
* A refusal and a **typo'd module name** produce the byte-identical error, so a
  failed subscribe does not tell you which one you have.

## Ingest authority

`note_transition` and `apply_snapshot` write the facts every other module is
about to trust, so the ingest surface is gated while the read surface is open.

**The gate is structural: the caller must be the host.**

```cpp
logos::currentCaller().isHost()      // logos-cpp-sdk, cpp/logos_caller.h
```

A push from core arrives as `{"kind":"host"}`; a call from any module arrives as
`{"kind":"module","name":…}`. So authority is what the caller **is**, not what it
knows — no secret to distribute, rotate or leak.

`host` carries **no name**, by rule 5 of the caller contract: `core` and
`capability_module` hold the same token value under two keys, so a name there
"would be a coin flip presented as a fact". The gate therefore admits core *or*
capability_module — both host-side components of the runtime rather than peer
modules, which is the distinction that matters here.

| caller | behaviour |
|---|---|
| `kind=host` | accepted |
| `kind=module` | **refused**, counted, and named on stderr |
| `kind=unknown` | **refused**, counted. Fail closed. |
| `LOGOS_MODULES_STATE_TEST_INGEST=1` | **test only.** Any caller accepted, with a loud stderr banner. |

The test door exists because a unit test calls the impl directly — no dispatch,
so no caller, so `Unknown` — and without it these invariants could only be
exercised by driving a live daemon, which is not a thing CI does. It is an
environment variable and not a method on purpose: a method would itself be
callable by any module, which would make the gate decorative.

### This replaced a shared secret, and why

The ingest surface used to take an `authToken` argument compared against a
per-run nonce in `LOGOS_MODULES_STATE_INGEST_TOKEN`. That design existed only
because the accessor did not — `LogosModuleContext` exposes this module's own
identity and nothing about the caller — and it would have required an `env`
field on `ModuleDescriptor` plumbed through `logos-container` and
`logos-container-subprocess` to deliver the nonce. The accessor landed, it
reaches this module (measured: `kind=host`), and a structural check beats a
secret, so the token and all of that plumbing are retired.

### The one pairing this depends on

This module requires a host that carries the caller machinery, and it ships
alongside one. That pairing is load-bearing rather than incidental.

**A stale `logos-module-builder` pin degrades caller identity to `unknown`
silently.** Measured: at `bc72ce39` the built plugin contained no caller
machinery at all (`nm` finds no `CallerScope`, no `currentInboundCallerJson`)
and every call answered `kind=unknown`; at master `464a75d` the same probe
answers `kind=host`. It compiles, links and loads either way, and nothing warns.

A fail-closed gate on `unknown` then refuses **every** push — inert rather than
secure. There is no counter on this surface to ask about it: the refusal is
written to **stderr**, naming what the caller actually was, with `unknown`
getting its own message pointing at the pin. That log line is the symptom.

One caveat on the measurement: it is macOS. `logos_caller.h` notes that on ELF
at default visibility a function-local static in an inline function emits as
`STB_GNU_UNIQUE` and the linker collapses every image's copy into one, and that
`logos-module-builder` sets no visibility anywhere — so Linux deserves its own
measurement before this is relied on there.

## The replay rule

A transition is applied if and only if its `seq` is strictly greater than the
seq already stored for that module. Delivery is not ordered — the first push to
an un-tokened target coalesces behind a token handshake, so a later push can
complete first. Per-module `seq` makes that harmless with no in-flight buffer,
no timing assumption, and no lock held across an RPC in either direction.

`apply_snapshot` applies the same rule per record, so a delta that overtook the
snapshot survives it. This is what makes a late-loading `modules_state` correct:
core pushes a snapshot when the module comes up, and everything that loaded
before it is still reported.

## `partial`

`partial` starts **true** and only a snapshot can clear it. Before a snapshot
arrives, everything held here was learned from deltas, which by construction
only mention modules that *changed* — a module quietly loaded the whole time is
missing from that view. Reporting `partial: false` then would be a confidently
short list, which is the exact failure the flag exists to prevent. After a
snapshot, `partial` is whatever the host said: true when the host's own scan
skipped a module it could not read.

## Build and try it

```bash
nix build                          # -> result/lib/modules_state_plugin.{dylib,so}
nix build .#install -o result-install
lm result/lib/modules_state_plugin.dylib

LSDIR=$(mktemp -d)                 # per-invocation: a leaked daemon poisons later runs
export LOGOS_MODULES_STATE_TEST_INGEST=1
logoscore --config-dir "$LSDIR" -D -m "$PWD/result-install/modules" &

# WAIT for the daemon to accept commands. `-D &` returns before it is
# listening, so a load-module issued straight after it races the socket.
until logoscore --config-dir "$LSDIR" status >/dev/null 2>&1; do sleep 0.5; done

logoscore --config-dir "$LSDIR" load-module modules_state

# RETRY the subscribe. Do not replace this with a longer sleep.
#
# `watch` can answer
#     {"code":"WATCH_FAILED","message":"Failed to watch events ..."}
# and it does NOT retry, so the rest of the script then runs and passes while
# silently observing nothing — the failure mode worth guarding, because it is
# invisible.
#
# The cause is that `watch` subscribes via requestObject, which refuses while
# the module has not PUBLISHED yet — a liveness probe, not a timing window
# (see "Two things that are easy to get wrong" above). load-module returns when
# the plugin is in; publish lands afterwards.
#
# Which is why a sleep is the wrong fix. It is not a fixed window: 24/24
# consecutive subscribes succeed warm at delays of 0,1,2,3,4,6s, and the one
# failure was a cold first run where publish took longer. Any constant is
# either too short on a cold machine or wasted on a warm one. Retry instead.
for attempt in 1 2 3 4 5; do
    logoscore --config-dir "$LSDIR" watch modules_state \
        --event module_state_changed > "$LSDIR/events.log" 2>&1 &
    WATCH_PID=$!
    sleep 1
    grep -q WATCH_FAILED "$LSDIR/events.log" || break
    kill "$WATCH_PID" 2>/dev/null; sleep 1
done

logoscore --config-dir "$LSDIR" call modules_state note_transition \
    dev chat_module 'json:null' 'json:null' unloaded loaded 'json:null' 1
logoscore --config-dir "$LSDIR" call modules_state list_modules
logoscore --config-dir "$LSDIR" stop
```

An empty optional in a positional slot is `'json:null'` — arity never changes.

A miss comes back as `{"status":"ok","result":null}` — `null` is the empty
optional, **not** a failed call. Verified end-to-end:
`module_record nope` answers exactly that, while `list_modules` answers
`{"modules":[...],"partial":true,"seq":2}` and a replayed `seq` answers
`"result":false`.

## Thread safety

Handlers are serialised today by `"concurrency": "single"`, but the registry
takes a mutex anyway: flipping that key must not silently turn into a data race
in the one module whose job is to be the trustworthy answer about system state,
and the feed that is coming originates on liblogos' background asio thread.

The one rule: **never hold the lock across an event emission.** Emitting crosses
the C ABI into the host and fans out to subscribers, any of which can call
straight back in — `is_ready()` is the obvious one. Every mutator computes under
the lock, collects pending events, releases, then emits.
