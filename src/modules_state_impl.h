#pragma once

// modules_state — the read-only registry of module lifecycle state.
//
// Load, unload and crash are spdlog lines inside liblogos' ModuleManager, so
// every consumer polls. This is where that state becomes queryable and
// subscribable. It REPORTS ONLY; load/unload stays with liblogos' C API.
//
//   read   — list_modules / module_record / is_ready + module_state_changed.
//            Open to every module.
//   ingest — note_transition / apply_snapshot. Host only; see INGEST AUTHORITY
//            in the .cpp.
//
// AUTHORING RULES (universal/Qt-free). The generator parses this header as
// TEXT, so these are constraints, not style:
//   * no Qt types; std + declared structs only.
//   * no trailing `// comment` on a declaration line — the parser wants a line
//     ending in `;` and silently DROPS anything else. Comments go above.
//   * non-scalar event params must be `const T&`.
//   * do not declare name()/version(); they are auto-injected as `derived`.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <logos_module_context.h>

// THE STATE VOCABULARY (wire type: tstr — LIDL has no enum)
//
// Six RECORD states, which a record may carry:
//   unloaded   known and installed, not running
//   loading    the host has selected a loader and is bringing it up
//   loaded     the host owns the process; the object may not be up yet
//   ready      loaded AND the module has published its object
//   stopping   an orderly teardown is in progress
//   error      it exited without being asked to
//
// One EVENT-ONLY state, which no record ever carries:
//   absent     the host does not know this module
//
// WHY `absent` SURVIVES — do not delete it as redundant. It is the only way to
// name the two MEMBERSHIP EDGES, and those are this module's headline claim:
// `absent -> unloaded` on discovery, `unloaded -> absent` on prune (liblogos
// module_registry.cpp). A transition needs a state on both sides; without it
// those edges cannot be events at all, and a consumer is back to inferring
// membership from package-install events plus a settle timer, which is what
// basecamp's PackageCoordinator does today.
//
// AND WHY IT IS EVENT-ONLY. A module that is absent is simply NOT IN the
// listing, and module_record answers the empty optional — one spelling for
// "not there", not two. The invariant holds BY CONSTRUCTION: a transition into
// `absent` removes the record, so no spelling of the ingest surface can store
// one.
//
// DIVERGENCE FROM THE DRAFT SPECS (logos-lips#317). spec-module-runtime §3.4
// has no `absent`, so we diverge by one event-only transition target, not a
// sixth record state — a consumer reading records sees only spec vocabulary.
// Separately, `loading` is a record state we have and the drafts fold into
// `loaded`; folding is a live option and is not settled here.
//
// `ready` has no emission point yet. It ships anyway so consumers written now
// are not rewritten later — the same reason the read surface says is_ready()
// and not isLoaded().
//
// FORWARD-COMPATIBILITY RULE, normative: a consumer MUST treat an unrecognised
// state as forward-compatible and fall back to "not loaded". It must not error
// or crash. Otherwise the day `ready` starts being emitted is the day every
// existing consumer breaks.

// One module's lifecycle facts, as reported by the host.
//
// `state` is never "absent" — that is event-only, and a record for a module
// that is not there is the empty optional instead.
//
// `instance` and `pid` answer different questions: instance is the host's
// persistence identity and is STABLE across load/unload cycles, so only a
// changed pid tells you a module died and came back.
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
// `partial` is an honest short answer, not a health flag: true when the host's
// last scan SKIPPED a module. A silently short list is worse than a flagged one.
//
// `seq` is the listing-level high-water mark, so a consumer re-reading can tell
// whether anything moved underneath.
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
    // MEMBERSHIP IS THE LISTING: no record here carries "absent". A consumer
    // must not filter for it, and one that finds it has found a bug here.
    ModuleListing list_modules();

    // One module's record — or nothing at all.
    //
    // EMPTY means the host's view does not contain it: never discovered, or
    // discovered and since pruned. Not an error.
    //
    // Over the wire that is JSON null, and null does NOT mean the call failed:
    // the generated Qt consumer decides success from the C ABI return code and
    // reports failure on logos::CallError, never from the value; logoscore
    // answers {"status":"ok","result":null} and says METHOD_NOT_FOUND only
    // after checking the module's published method list.
    std::optional<ModuleRecord> module_record(const std::string& module);

    // True when this module is up and usable FROM THE HOST'S POINT OF VIEW.
    //
    // READ THE LIMIT: it does not answer "a call from ME will succeed" — that
    // additionally needs the per-caller token handshake, which is per-caller
    // and so not a fact this module can hold. For "can I call it", use
    // whenObjectAvailable() on your own client; this predicate goes true a few
    // hundred milliseconds early for that purpose.
    bool is_ready(const std::string& module);

    // ── INGEST SURFACE (host only) ───────────────────────────────────────────

    // Record one state transition. True when applied.
    //
    // FALSE covers three cases, deliberately not distinguished on the wire so a
    // probe cannot tell a refused caller from a stale seq: the caller is not the
    // host; `seq` was not newer; old_state/new_state were empty.
    //
    // REPLAY RULE: applied iff `seq` is strictly greater than what is stored for
    // that module. Delivery is not ordered, so a later push can land first, and
    // per-module seq makes that harmless with no buffering and no timing
    // assumption. The rule is TOTAL — it covers a module that has since gone
    // absent, which is why the .cpp keeps a seq tombstone for one that left.
    //
    // THE TOMBSTONE PUTS A REQUIREMENT ON CORE, and it is not visible from this
    // side: a record pruned by apply_snapshot is tombstoned at the LISTING's
    // seq, so core must stamp snapshot record seqs from the SAME counter it
    // stamps transitions from. Anything else makes the tombstone unreachably
    // high (a real later delta dropped forever) or trivially low (a stale delta
    // resurrects a pruned module).
    //
    // `new_state == "absent"` is a MEMBERSHIP EDGE: the event fires as normal
    // but the module leaves the listing rather than being stored absent.
    bool note_transition(const std::string& module,
                         const std::optional<std::string>& instance,
                         const std::optional<int64_t>& pid,
                         const std::string& old_state,
                         const std::string& new_state,
                         const std::optional<std::string>& reason,
                         uint64_t seq);

    // Replace the whole picture with a host-supplied snapshot.
    //
    // This is what makes a late-loading modules_state correct: it loads after
    // other modules, so deltas alone give it a permanently short list.
    //
    // Merge is per-record under the same seq rule, so a delta that overtook the
    // snapshot survives it. Records missing from the listing whose stored seq is
    // older than the listing's are dropped, each with its own "absent" event.
    //
    // A snapshot record claiming "absent" is contract-malformed — a snapshot
    // spells non-membership by OMISSION — so it is skipped, which makes it mean
    // exactly what omitting it would have meant.
    bool apply_snapshot(const ModuleListing& listing);

logos_events:
    // Emitted on every APPLIED transition, including those apply_snapshot
    // applies.
    //
    // A transition PAIR rather than a "kind" string: strictly more informative,
    // and a consumer that only cares something went away reads new_state.
    // old_state is never equal to new_state; a no-op is not an event.
    void module_state_changed(const std::string& module,
                              const std::optional<std::string>& instance,
                              const std::optional<int64_t>& pid,
                              const std::string& old_state,
                              const std::string& new_state,
                              const std::optional<std::string>& reason,
                              uint64_t seq);
};
