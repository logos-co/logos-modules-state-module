#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// modules_state — the read-only registry of module lifecycle state.
//
// WHAT THIS IS
//   Today there are no module-lifecycle events at all: load, unload and crash
//   are spdlog lines inside logos-liblogos' ModuleManager, so every consumer
//   polls. This module is the place that state becomes a first-class, queryable,
//   subscribable fact.
//
// WHAT THIS IS NOT
//   It does not drive load/unload. Nothing here starts, stops or restarts a
//   module — that stays with liblogos' C API. This module only *reports*.
//
// THE TWO SURFACES
//   read   — list_modules / module_record / is_ready, plus the
//            module_state_changed event. Open to every module.
//   ingest — note_transition / apply_snapshot. Gated by `authToken`, and
//            intended for exactly one caller: liblogos core. See INGEST
//            AUTHORITY in modules_state_impl.cpp.
//
// STAGE 1 (this repo, right now)
//   Nothing feeds it. The ingest surface is the only way state enters, which
//   makes it both the future core seam AND the Stage-1 test injection point.
//
// AUTHORING RULES (universal / Qt-free) — the generator parses this header as
// text, so these are hard constraints, not style:
//   * no Qt types; std + declared structs only.
//   * no trailing `// comment` on a declaration line — the parser only accepts
//     a line ending in `;`, and silently DROPS anything else. Comments go above.
//   * event params that are non-scalar must be `const T&`.
//   * do not declare name()/version() — auto-injected as `derived`.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <logos_module_context.h>

// ─────────────────────────────────────────────────────────────────────────────
// THE STATE VOCABULARY  (wire type: tstr — LIDL has no enum)
//
// Six RECORD states — the ones a record returned by list_modules or
// module_record may carry:
//
//   "unloaded"  known and installed, not running
//   "loading"   the host has selected a loader and is bringing it up
//   "loaded"    the plugin is up and its provider has published
//   "ready"     loaded AND has completed its own readiness work
//   "stopping"  an orderly teardown is in progress
//   "error"     it exited without being asked to
//
// and one EVENT-ONLY state, which no record ever carries:
//
//   "absent"    the host does not know this module
//
// ── WHY "absent" SURVIVES — DO NOT DELETE IT AS REDUNDANT ────────────────────
//
// It is the only way to name the two MEMBERSHIP EDGES of the graph, and those
// edges are this module's headline claim:
//
//   absent -> unloaded   the host has just DISCOVERED a module. In liblogos
//                        that is the upsert loop of
//                        discoverInstalledModules (module_registry.cpp:80-94).
//   unloaded -> absent   the host has just PRUNED one, because its files went
//                        away (module_registry.cpp:96-108).
//
// A transition needs a state on BOTH sides. Strike `absent` and those two
// edges have no old_state and no new_state to ride on, so they cannot be
// events at all — and a consumer is back to what logos-basecamp does today:
// PackageCoordinator.cpp:117-157 infers module lifecycle from PACKAGE-INSTALL
// events plus a 100ms QTimer settle. Guessing membership from package events is
// exactly what this module exists to stop doing.
//
// ── AND WHY IT IS EVENT-ONLY ─────────────────────────────────────────────────
//
// `absent` may appear as old_state or new_state in module_state_changed. It may
// NEVER be the `state` of a record. A module that is absent is simply NOT IN
// list_modules, and module_record answers an empty optional.
//
// It used to carry a second load: it was also module_record's MISS ANSWER, a
// workaround for a generator that refused `-> ?T`. That refusal is gone, so the
// miss is an empty optional and the second load with it. Two spellings for "not
// there" is the drift this narrowing removes — a consumer that filtered
// `state == "absent"` out of a listing and a consumer that checked has_value()
// were two code paths for one fact, and only one of them stayed correct.
//
// The invariant holds BY CONSTRUCTION, not by convention: a transition into
// `absent` removes the record (see note_transition in the .cpp), so there is no
// spelling of the ingest surface that can put an absent record into the store.
//
// ── DIVERGENCE FROM THE DRAFT CORE SPECS (logos-lips#317) ────────────────────
//
// spec-module-runtime.md §3.4 spells the runtime's vocabulary
// `unloaded | loaded | ready | stopping | error` — five states, no `absent`,
// and it is normative there as a CDDL enum (logos.runtime_control.state).
//
// After the narrowing above, `absent` diverges as exactly one EVENT-ONLY
// TRANSITION TARGET rather than as a sixth record state. A consumer that reads
// records already sees only spec vocabulary; only an event subscriber ever sees
// `absent`, and only on the two membership edges above. The drafts have no way
// to say "discovered" or "pruned" — registry membership is not in their state
// machine at all — so this is a deliberate addition, not drift.
//
// One other divergence, named here so it is not mistaken for the same thing:
// `loading` is a RECORD state we have and the drafts do not. Their `loaded`
// covers our loading+loaded ("Runtime has acquired or attached the selected
// realization and is performing the applicable initialization and readiness
// checks"). Folding the two is a live option; it is a separate question from
// `absent` and is not settled here.
//
// ── REACHABILITY, TODAY ──────────────────────────────────────────────────────
//
// Against liblogos as it stands: absent, unloaded, loading, loaded, stopping
// and error all have real emission points. "ready" does not — it becomes real
// when a module can report its own readiness. It ships anyway so consumers
// written now are not rewritten then; that is the same reason the read surface
// says is_ready() and not isLoaded().
//
// NORMATIVE FORWARD-COMPATIBILITY RULE — this is the whole point of shipping
// the full vocabulary, so it is a rule and not advice:
//   A consumer MUST treat an unrecognised state string as forward-compatible
//   and fall back to "not loaded". It MUST NOT treat it as an error and MUST
//   NOT crash. Without this, the day "ready" starts being emitted is the day
//   every existing consumer breaks — the exact failure the full vocabulary
//   exists to prevent.
//
// The canonical strings are exposed as functions rather than an enum because
// the wire type is tstr; see modules_state_impl.cpp.
// ─────────────────────────────────────────────────────────────────────────────

// One module's lifecycle facts, as reported by the host.
//
// `state` is one of the six RECORD states. It is never "absent": that one is an
// event-only transition target, and a record for a module that is not there is
// not a record — it is the empty optional module_record answers with.
//
// `instance` and `pid` answer different questions and both are cheap:
//   instance — the host's persistence identity. STABLE across load/unload
//              cycles (ResolveMode::ReuseOrCreate), so it cannot tell you a
//              module died and came back.
//   pid      — the process incarnation. A changed pid between two reads IS
//              that answer.
struct ModuleRecord {
    std::string module;
    std::optional<std::string> instance;
    std::optional<int64_t> pid;
    std::string state;
    std::optional<std::string> reason;
    std::string path;
    std::string type;
    std::string version;
    std::vector<std::string> dependencies;
    std::vector<std::string> dependents;
    int64_t loadedAt;
    uint64_t seq;
};

// The answer to list_modules().
//
// `partial` is an honest short answer, not a health flag: it is true when the
// host's last scan SKIPPED at least one module (unreadable metadata, or a
// failed trusted-name check). A silently short list is worse than a flagged
// one.
//
// `seq` is the listing-level high-water mark. Paired with each record's own
// `seq` it lets a consumer re-read and tell whether anything moved underneath.
struct ModuleListing {
    std::vector<ModuleRecord> modules;
    bool partial;
    uint64_t seq;
};

class ModulesStateImpl : public LogosModuleContext {
public:
    ModulesStateImpl();
    ~ModulesStateImpl();

    // ── READ SURFACE ─────────────────────────────────────────────────────────

    // Every module the host knows about, with its current state.
    //
    // MEMBERSHIP IS THE LISTING. No record here ever carries state "absent" —
    // a module that is absent is not a member, so it is not in `modules` at
    // all. A consumer must not filter for it, and a consumer that finds it has
    // found a bug in this module, not a module that went away.
    ModuleListing list_modules();

    // One module's record — or nothing at all.
    //
    // EMPTY means "the host's view does not contain this module": never
    // discovered, or discovered and since pruned. That is not an error, and it
    // is not a record either. `absent` is an EVENT-ONLY state (see THE STATE
    // VOCABULARY above), so there is exactly ONE spelling for "not there" and
    // this is it.
    //
    // Why an optional and not a sentinel record: std::optional has a state no
    // ModuleRecord value occupies, so nullopt is distinct from
    // ModuleRecord{} — the distinction a bare record could never make. A caller
    // branches on has_value(), not on a sentinel field it has to be told to
    // look at. (It used to be told: the miss came back as `state:"absent"`,
    // `seq:0`. That was a workaround for a generator that refused `-> ?T`, the
    // refusal is gone, and the workaround went with it.)
    //
    // Over the wire the empty answer is JSON null, and on every surface this
    // module is reached through, null does NOT mean the call failed:
    //   * the generated Qt consumer decides success from the C ABI return code
    //     (rc == LP_OK) and reports failure on a SEPARATE channel,
    //     logos::CallError — never from the value. See logos-qt-sdk,
    //     qt-generator/lidl_gen_qt_consumer.cpp:524-558.
    //   * logoscore's `logosctl module call` answers
    //     {"status":"ok","result":null}. On a null return it asks the module
    //     for its published method list and says METHOD_NOT_FOUND only when the
    //     method genuinely is not there — logos-logoscore-cli,
    //     src/core_service/call_envelope.cpp:81-103.
    std::optional<ModuleRecord> module_record(const std::string& module);

    // True when this module is up and usable *from the host's point of view*.
    //
    // READ THE LIMIT: this answers "the host has it loaded and it has
    // published". It does NOT answer "a call from ME to it will succeed" —
    // that additionally needs the per-caller token handshake, which is
    // per-caller and therefore not a fact this module can hold. A caller that
    // needs "can I call it" wants whenObjectAvailable() on its own client, not
    // this. Using is_ready() for that trades a working poll for a predicate
    // that goes true a few hundred milliseconds early.
    bool is_ready(const std::string& module);

    // ── INGEST SURFACE (authToken-gated; core only) ───────────────────────────

    // Record one state transition. Returns true when it was applied.
    //
    // Returns FALSE for three different reasons, deliberately not
    // distinguished on the wire (a probe must not be able to tell a bad token
    // from a stale seq):
    //   * the token was refused,
    //   * `seq` was not newer than what is already stored for this module,
    //   * old_state/new_state were empty.
    //
    // REPLAY RULE: applied if and only if `seq` is strictly greater than the
    // seq already stored for that module. Deliveries are not ordered — the
    // first call to an un-tokened target coalesces behind a handshake, so a
    // later push can land first — and per-module seq is what makes that
    // harmless without any buffering or timing assumption. The rule is TOTAL:
    // it covers a module that has since gone absent too, which is why the .cpp
    // keeps a seq tombstone for one that left the listing.
    //
    // THE TOMBSTONE PUTS A REQUIREMENT ON CORE, and it is not obvious from
    // this side: a record pruned by apply_snapshot is tombstoned at the
    // LISTING's seq, so core must stamp snapshot record seqs from the SAME
    // global counter it stamps transitions from. Stamp them from anything
    // else — a per-snapshot counter, or zero — and the tombstone is either
    // unreachably high (a real later delta is dropped forever) or trivially
    // low (a stale delta resurrects a module that is gone). One counter, both
    // paths.
    //
    // A transition whose `new_state` is "absent" is a MEMBERSHIP EDGE. The
    // event fires exactly as any other, but no absent record is stored: the
    // module leaves the listing instead. That is what makes list_modules'
    // never-absent invariant true by construction.
    bool note_transition(const std::string& authToken,
                         const std::string& module,
                         const std::optional<std::string>& instance,
                         const std::optional<int64_t>& pid,
                         const std::string& old_state,
                         const std::string& new_state,
                         const std::optional<std::string>& reason,
                         uint64_t seq);

    // Replace the whole picture with a host-supplied snapshot.
    //
    // This is what makes a late-loading modules_state correct: core pushes a
    // snapshot when this module comes up, so everything that loaded BEFORE it
    // (capability_module always does) is still reported.
    //
    // Merge is per-record and uses the same seq rule as note_transition, so a
    // delta that overtook the snapshot survives it. Records missing from the
    // listing whose stored seq is older than the listing's seq are dropped,
    // each with its own state -> "absent" event.
    //
    // A snapshot record whose `state` is "absent" is contract-malformed —
    // `absent` is event-only, and a snapshot spells non-membership by OMISSION.
    // It is skipped, which makes it mean exactly what omitting it would have
    // meant. One rule, not two.
    bool apply_snapshot(const std::string& authToken, const ModuleListing& listing);

    // Number of ingest calls refused. A counter, not an event: it exists so a
    // test can prove the gate is closed, and so an operator can see a module
    // trying to forge lifecycle facts.
    uint64_t rejected_ingest_count();

logos_events:
    // Emitted on every APPLIED transition — including the ones apply_snapshot
    // applies.
    //
    // A transition PAIR, not a "kind" string: strictly more informative, and a
    // consumer that only cares that something went away just reads new_state.
    // old_state is never equal to new_state; a no-op is not an event.
    void module_state_changed(const std::string& module,
                              const std::optional<std::string>& instance,
                              const std::optional<int64_t>& pid,
                              const std::string& old_state,
                              const std::string& new_state,
                              const std::optional<std::string>& reason,
                              uint64_t seq);
};
