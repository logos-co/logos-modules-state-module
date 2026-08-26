# modules_state

The read-only registry of module lifecycle state.

Today there are no module-lifecycle events in Logos at all: load, unload and
crash are spdlog lines inside `logos-liblogos`' `ModuleManager`, so every
consumer polls — `logos-basecamp` runs a 2-second timer and infers module state
from package-manager install events. This module is where that state becomes a
first-class, queryable, subscribable fact.

It **reports**. It does not drive load/unload — that stays with liblogos' C API.

## Status: Stage 1

Nothing feeds it yet. The module is complete and standalone; the liblogos
registry observer and the core push that will feed it are Stages 2 and 3.

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
method rejected_ingest_count()     -> uint

# ingest surface — authToken-gated, for liblogos core only
method note_transition(authToken, module, instance: ?tstr, pid: ?int,
                       old_state, new_state, reason: ?tstr, seq: uint) -> bool
method apply_snapshot(authToken: tstr, listing: ModuleListing) -> bool

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
plain subscription is one-shot and is refused before the registry handshake.
The generated typed wrapper (`logos.modules_state.on("module_state_changed", …)`)
already does the right thing; only hand-rolled call sites are exposed.

## Ingest authority

`note_transition` and `apply_snapshot` write the facts every other module is
about to trust, so the ingest surface is gated while the read surface is open.
The token is an **argument** because liblogos' `AccessPolicy` restricts by target
module with no per-method granularity — restricting `modules_state` to caller
`core` would lock out every reader.

| environment | behaviour |
|---|---|
| `LOGOS_MODULES_STATE_INGEST_TOKEN=<nonce>` | ingest requires an exact match. The production shape: the host generates a per-run nonce and puts it in the module subprocess's environment. |
| neither set | **all ingest refused.** Fail closed. |
| `LOGOS_MODULES_STATE_TEST_INGEST=1` (and no token) | **test only.** Any token accepted, with a loud stderr banner on every accepted write. |

The test escape is an environment variable and not a method on purpose: a method
would itself be callable by any module, which would make the gate decorative.

`rejected_ingest_count()` counts refusals **by the authority gate** and nothing
else — not stale-seq drops, not malformed arguments — so a test asserting "the
gate is closed" asserts exactly that.

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
# Observed ONCE, on a cold first run (first load of the plugin, first spawn of
# capability_module from a fresh store path). It did NOT reproduce warm: 24/24
# consecutive subscribes succeeded with delays of 0,1,2,3,4,6s. So the trigger
# is startup cost, NOT a fixed window — which is exactly why a sleep is the
# wrong fix. Any constant is either too short on a cold machine or wasted on a
# warm one. Retry until subscribed.
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
