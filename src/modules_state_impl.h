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
//   "absent"    the host does not know this module (never discovered, or
//               pruned because its files went away)
//   "unloaded"  known and installed, not running
//   "loading"   the host has selected a loader and is bringing it up
//   "loaded"    the plugin is up and its provider has published
//   "ready"     loaded AND has completed its own readiness work
//   "stopping"  an orderly teardown is in progress
//   "error"     it exited without being asked to
//
// Reachability against liblogos as it stands today: absent, unloaded, loading,
// loaded, stopping and error are all reachable at real emission points.
// "ready" is not — it becomes real when a module can report its own readiness.
// It ships anyway so consumers written now are not rewritten then; that is the
// same reason the read surface says is_ready() and not isLoaded().
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
    ModuleListing list_modules();

    // One module's record.
    //
    // A module this registry has never heard of is not an error and not an
    // empty optional — it comes back as a record whose `state` is "absent" and
    // whose `seq` is 0. That is the honest answer ("the host's view does not
    // contain this module") and it is expressible in the state vocabulary
    // itself, so callers need no second code path for the miss.
    //
    //   seq == 0  <=>  never observed. Core's sequence counter is pre-
    //                  incremented, so a real record always carries seq >= 1.
    //
    // This deliberately does NOT return `? ModuleRecord`, which is what the
    // draft contract said. An empty `?T` return is JSON null on the wire, and
    // null is ALREADY how this path reports a failed call — logos_json_convert
    // turns it into an invalid QVariant and core_service reports METHOD_FAILED.
    // "found nothing" and "the call blew up" would be the same bytes for every
    // non-Rust caller. The generator refuses `-> ?T` for exactly this reason.
    ModuleRecord module_record(const std::string& module);

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
    // harmless without any buffering or timing assumption.
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
    // delta that overtook the snapshot survives it. Records absent from the
    // listing whose stored seq is older than the listing's seq are dropped.
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
