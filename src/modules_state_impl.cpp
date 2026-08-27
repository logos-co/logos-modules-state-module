#include "modules_state_impl.h"

#include <logos_caller.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// THREAD SAFETY — read before touching anything below.
//
// Handlers are serialised today by `"concurrency": "single"`, so the mutex is
// never contended in this build. It exists anyway for two reasons, either
// sufficient: flipping that one metadata key must not silently introduce a data
// race in the module whose job is to be the trustworthy answer about system
// state; and the feed originates on liblogos' background asio thread, so this
// module must not stake its invariants on somebody else's marshalling.
//
// THE ONE RULE: never hold m_mutex across an event emission. Emitting crosses
// the C ABI into the host and fans out to subscribers, any of which can call
// straight back in — is_ready() is the obvious one. std::mutex is not
// recursive, so emitting under the lock is a self-deadlock waiting for a
// consumer to be written naturally.
//
// Every mutator therefore follows one shape, with no exceptions:
//   compute under the lock -> collect pending events -> release -> emit.

namespace {

// Free constants rather than an enum: the wire type is `tstr`, and an enum here
// would invite treating an unrecognised state as an error — the one thing the
// contract forbids. They live in this TU because the generator parses the
// header as text, so anything there that is not contract is noise at best.
//
// kAbsent is the EVENT-ONLY one, and its invariant is checkable by grep: it is
// READ where a module leaves the view, and WRITTEN only into a PendingEvent.
// It is never assigned to a stored record's `state` on any path.
constexpr const char* kAbsent = "absent";
constexpr const char* kLoaded = "loaded";
constexpr const char* kReady  = "ready";

// "Up and usable, as far as the host is concerned."
//
// `loaded` is NOT in this set: liblogos emits loaded->ready once the module
// publishes its object, so is_ready() is false during that window — which is
// precisely the window a caller is asking about.
//
// An unrecognised state answers false: the same forward-compatibility fallback
// the contract imposes on consumers. A module that demands others tolerate
// unknown states has to do it itself.
bool stateIsReady(const std::string& state)
{
    return state == kReady;
}

// The stored form of a ModuleRecord. Identical to the wire record; kept as its
// own alias so the storage type and the wire type can diverge later without a
// rename sweep.
using StoredRecord = ModuleRecord;

// An event computed under the lock and emitted after it is released.
struct PendingEvent {
    std::string module;
    std::optional<std::string> instance;
    std::optional<int64_t> pid;
    std::string oldState;
    std::string newState;
    std::optional<std::string> reason;
    uint64_t seq;
};

// The starting point for a module first learned from a delta, whose static
// metadata only a snapshot can fill in.
//
// It deliberately leaves `state` EMPTY: the one caller assigns a real state
// before storing, and an empty state is not in the vocabulary, so a future path
// that stored this seed untouched would be visibly wrong rather than plausibly
// "absent".
//
// Written out rather than leaning on std::map::operator[]'s value-init: that
// would in fact zero the members, but the guarantee is a language subtlety, and
// the fields cannot carry default member initialisers because the header is
// parsed as text and `uint64_t seq = 0;` is not a spelling the field scanner is
// promised to read.
ModuleRecord blankRecord(const std::string& name)
{
    ModuleRecord rec;
    rec.module = name;
    rec.loadedAt = 0;
    rec.seq = 0;
    return rec;
}

// ── INGEST AUTHORITY ─────────────────────────────────────────────────────────
//
// note_transition and apply_snapshot write the facts every other module is
// about to trust. Unguarded, any module could forge a lifecycle event —
// announce that a rival crashed, or that a module it wants others to call is
// `ready`. The read surface is open; this one must not be.
//
// THE GATE IS STRUCTURAL: the caller must be the HOST. A push from core arrives
// as {"kind":"host"}; a call from any module arrives as {"kind":"module",...}.
// So authority is what the caller IS, not what it knows, and there is no secret
// to distribute, rotate or leak. This replaced an authToken compared against a
// per-run nonce, which existed only because the accessor did not.
//
// WHAT `host` MEANS: rule 5 of the caller contract gives the host arm no name,
// because "core" and "capability_module" hold the same token value under two
// keys and a name there "would be a coin flip presented as a fact". So this
// admits core OR capability_module — both host-side runtime components rather
// than peer modules, which is the distinction that matters.
//
// FAIL CLOSED ON `unknown`. currentCaller() answers Unknown for anything that
// is not an inbound dispatch, and for any identity this build cannot parse.
// The case that bites is a MISPINNED BUILD: a stale logos-module-builder
// produces a plugin with no caller machinery at all, so every push is refused
// while everything still compiles, links and loads. This module ships alongside
// a host that carries the machinery, and that pairing is the mitigation — there
// is no counter on this surface to ask. The refusal goes to stderr, naming what
// the caller actually was.
//
// THE TEST DOOR: LOGOS_MODULES_STATE_TEST_INGEST=1 accepts any caller, loudly.
// A unit test calls the impl directly, so there is no dispatch and no caller,
// and without it these invariants could only be driven through a live daemon —
// which CI does not do. It is an environment variable and not a method on
// purpose: a method would itself be callable by any module.
bool testIngestEnabled()
{
    const char* v = std::getenv("LOGOS_MODULES_STATE_TEST_INGEST");
    return v != nullptr && std::strcmp(v, "1") == 0;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The registry itself.
//
// Held in a pimpl so the impl header stays a pure statement of the contract:
// the generator reads that header as text, and every private member added
// there is one more thing it has to be taught to ignore.
// ─────────────────────────────────────────────────────────────────────────────
struct ModulesStateRegistry {
    std::mutex mutex;

    // MEMBERSHIP IS THIS MAP. No entry ever has state "absent": that is an
    // event-only target, so leaving the view means leaving the map.
    std::map<std::string, StoredRecord> records;

    // Tombstones: modules that left `records`, and the seq at which each left.
    // They exist so the REPLAY RULE stays total — without them a delta that
    // lost a race would find no stored seq, pass the rule, and resurrect a
    // module the host already pruned. Unreachable from any read method, so it
    // cannot be mistaken for membership. Grows with modules ever seen, not with
    // events, so it is bounded by what is installed.
    std::map<std::string, uint64_t> departedSeq;

    // Highest seq applied from any source. Reported as ModuleListing::seq.
    uint64_t highWaterSeq = 0;

    // See list_modules() for why this starts TRUE.
    bool partial = true;

    bool warnedAboutTestIngest = false;
};

static ModulesStateRegistry& registryOf(void*& slot)
{
    if (slot == nullptr)
        slot = new ModulesStateRegistry();
    return *static_cast<ModulesStateRegistry*>(slot);
}

// The single registry instance for this module. A module is one process and one
// impl object, so a file-static is the whole story; it is written this way
// rather than as a member to keep the impl header free of private state.
static void* g_registrySlot = nullptr;

static ModulesStateRegistry& reg()
{
    return registryOf(g_registrySlot);
}

// The seq the REPLAY RULE compares an incoming delta against: what is stored
// for this module, whether it is still a member or only a tombstone. Returns
// false when this registry has never heard of the module at all, which is the
// one case where any seq is newer.
//
// Callers hold reg().mutex.
static bool storedSeqLocked(const std::string& module, uint64_t& out)
{
    auto it = reg().records.find(module);
    if (it != reg().records.end()) {
        out = it->second.seq;
        return true;
    }
    auto t = reg().departedSeq.find(module);
    if (t != reg().departedSeq.end()) {
        out = t->second;
        return true;
    }
    return false;
}

ModulesStateImpl::ModulesStateImpl() = default;
ModulesStateImpl::~ModulesStateImpl() = default;

// Shared by both ingest methods. Returns true when the caller IS the host;
// increments the refusal counter and complains on stderr when it is not.
//
// Order matters: the structural check comes first, so a machine that has the
// test door open for some other module's suite still takes the real path here
// whenever the real path can answer.
static bool ingestAuthorised()
{
    const logos::LogosCaller caller = logos::currentCaller();

    if (caller.isHost())
        return true;

    if (testIngestEnabled()) {
        bool warn = false;
        {
            std::lock_guard<std::mutex> lock(reg().mutex);
            warn = !reg().warnedAboutTestIngest;
            reg().warnedAboutTestIngest = true;
        }
        if (warn) {
            std::fprintf(stderr,
                "[modules_state] *** TEST INGEST ENABLED ***\n"
                "[modules_state] LOGOS_MODULES_STATE_TEST_INGEST=1, so ANY caller\n"
                "[modules_state] can write lifecycle facts regardless of identity.\n"
                "[modules_state] This is for testing only. Never set it in production.\n");
        }
        std::fprintf(stderr, "[modules_state] TEST INGEST: accepting write from a non-host caller\n");
        return true;
    }

    // Refused. The message names what the caller ACTUALLY was, because the two
    // ways to land here need different fixes and are otherwise
    // indistinguishable from outside:
    //
    //   kind=module  a peer module tried to write lifecycle facts. Working as
    //                intended; this is the case the gate exists for.
    //   kind=unknown either a non-dispatch context, or A MISPINNED BUILD whose
    //                plugin carries no caller machinery at all. The second is
    //                silent everywhere else — it compiles, links and loads —
    //                so naming it here is the only warning anyone gets.
    if (caller.isUnknown()) {
        std::fprintf(stderr,
            "[modules_state] REFUSED ingest: caller identity is UNKNOWN.\n"
            "[modules_state] Either this was not an inbound call, or this build\n"
            "[modules_state] carries no caller machinery -- check that\n"
            "[modules_state] logos-module-builder is not pinned stale.\n");
    } else {
        std::fprintf(stderr,
            "[modules_state] REFUSED ingest: caller is '%s', not the host.\n",
            caller.name.empty() ? "<unnamed non-host>" : caller.name.c_str());
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Read surface
// ─────────────────────────────────────────────────────────────────────────────

ModuleListing ModulesStateImpl::list_modules()
{
    ModuleListing out;
    std::lock_guard<std::mutex> lock(reg().mutex);

    out.modules.reserve(reg().records.size());
    // std::map iterates in key order, so the listing is sorted — deterministic
    // output lets a test diff two listings instead of set-comparing them.
    //
    // NOT FILTERED, on purpose: `records` is membership and neither mutator can
    // store "absent", so a defensive filter would hide the bug it was written
    // to catch.
    for (const auto& kv : reg().records)
        out.modules.push_back(kv.second);

    // `partial` starts TRUE and only a snapshot can clear it. Deltas by
    // construction only mention modules that CHANGED, so a module quietly
    // loaded the whole time is missing from that view — reporting false would
    // be a confidently short list, the exact failure this flag prevents. After
    // a snapshot it is whatever the host said.
    out.partial = reg().partial;
    out.seq = reg().highWaterSeq;
    return out;
}

std::optional<ModuleRecord> ModulesStateImpl::module_record(const std::string& module)
{
    std::lock_guard<std::mutex> lock(reg().mutex);
    auto it = reg().records.find(module);
    if (it != reg().records.end())
        return it->second;
    // A tombstone is not a hit: a pruned module is exactly as absent as one
    // never discovered. This answers membership, not history.
    return std::nullopt;
}

bool ModulesStateImpl::is_ready(const std::string& module)
{
    std::lock_guard<std::mutex> lock(reg().mutex);
    auto it = reg().records.find(module);
    if (it == reg().records.end())
        return false;
    return stateIsReady(it->second.state);
}


// ─────────────────────────────────────────────────────────────────────────────
// Ingest surface
// ─────────────────────────────────────────────────────────────────────────────

bool ModulesStateImpl::note_transition(const std::string& module,
                                      const std::optional<std::string>& instance,
                                      const std::optional<int64_t>& pid,
                                      const std::string& old_state,
                                      const std::string& new_state,
                                      const std::optional<std::string>& reason,
                                      uint64_t seq)
{
    if (!ingestAuthorised())
        return false;

    // Malformed arguments are refused, and are a different thing from the
    // authority refusal above: the caller proved it was the host and then sent
    // something unusable.
    if (module.empty() || old_state.empty() || new_state.empty())
        return false;

    // A transition to the state it is already in is not a transition. Core
    // guarantees old != new on its side; refusing it here keeps that guarantee
    // true of the event stream regardless of who is calling.
    if (old_state == new_state)
        return false;

    std::optional<PendingEvent> pending;
    {
        std::lock_guard<std::mutex> lock(reg().mutex);

        // THE REPLAY RULE: applied iff seq is strictly newer than what is
        // stored for THIS module. Delivery is not ordered, so a later push can
        // land first; per-module seq makes that harmless with no buffering, no
        // timing assumption and no lock held across an RPC.
        uint64_t stored = 0;
        if (storedSeqLocked(module, stored) && seq <= stored)
            return false;

        // THE MEMBERSHIP EDGE. A transition INTO `absent` stores no record: it
        // removes the module and leaves a seq tombstone. The event still fires
        // and the high-water seq still advances. This is the whole mechanism
        // behind list_modules' never-absent invariant.
        if (new_state == kAbsent) {
            // Leaving the view. Erase the record and leave a seq tombstone, so
            // a delta that lost a race cannot resurrect a module the host has
            // already pruned.
            reg().records.erase(module);
            reg().departedSeq[module] = seq;
        } else {
            auto it = reg().records.find(module);
            if (it == reg().records.end())
                reg().records.emplace(module, blankRecord(module));
            // Coming back is membership again: drop the tombstone so it cannot
            // outlive the record and shadow a later erase/re-add cycle.
            reg().departedSeq.erase(module);
            StoredRecord& rec = reg().records[module];
            rec.module = module;
            rec.instance = instance;
            rec.pid = pid;
            rec.state = new_state;
            rec.reason = reason;
            rec.seq = seq;
            // instance/pid are OVERWRITTEN, including to empty: they describe
            // the incarnation this transition is ABOUT, so a delta saying "no
            // pid" means there is no pid. path/type/version/deps/loadedAt are
            // SNAPSHOT-ONLY and preserved rather than blanked — a delta carries
            // the lifecycle change, not the static metadata.
        }

        if (seq > reg().highWaterSeq)
            reg().highWaterSeq = seq;

        // The pair forwarded is the CALLER's, not (ourStoredState -> new).
        // Core computes old_state atomically with its own write, so its pair is
        // authoritative; our stored value can only be older.
        pending = PendingEvent{module, instance, pid, old_state, new_state, reason, seq};
    }

    // Lock released. Safe to re-enter.
    if (pending)
        module_state_changed(pending->module, pending->instance, pending->pid,
                             pending->oldState, pending->newState, pending->reason,
                             pending->seq);
    return true;
}

bool ModulesStateImpl::apply_snapshot(const ModuleListing& listing)
{
    if (!ingestAuthorised())
        return false;

    std::vector<PendingEvent> pending;
    {
        std::lock_guard<std::mutex> lock(reg().mutex);

        std::set<std::string> present;

        for (const ModuleRecord& incoming : listing.modules) {
            if (incoming.module.empty())
                continue;

            // "absent" is contract-malformed here — a snapshot spells
            // non-membership by OMISSION — so skipping makes it mean exactly
            // what omitting it would have. An EMPTY state is skipped by the
            // same rule: `state` is tstr with no enum, so an omitted field
            // would otherwise be admitted as a seventh, undeclared state.
            if (incoming.state == kAbsent || incoming.state.empty())
                continue;

            present.insert(incoming.module);

            auto it = reg().records.find(incoming.module);
            const bool known = (it != reg().records.end());

            // Same replay rule as note_transition, applied per record, and
            // consulting the tombstones for the same reason: a module that was
            // pruned at a HIGHER seq than this snapshot's view of it must not
            // be brought back by a snapshot that predates its departure.
            uint64_t stored = 0;
            if (storedSeqLocked(incoming.module, stored) && incoming.seq <= stored)
                continue;

            // Unknown means just DISCOVERED — never heard of, or departed and
            // come back. Either way the edge crossed is absent -> state.
            const std::string previousState = known ? it->second.state : std::string(kAbsent);

            reg().records[incoming.module] = incoming;
            reg().departedSeq.erase(incoming.module);

            if (incoming.seq > reg().highWaterSeq)
                reg().highWaterSeq = incoming.seq;

            // Only a genuine change is an event. A snapshot that agrees with
            // what we already knew updates the record's metadata silently.
            if (previousState != incoming.state)
                pending.push_back(PendingEvent{incoming.module, incoming.instance,
                                               incoming.pid, previousState,
                                               incoming.state, incoming.reason,
                                               incoming.seq});
        }

        // Anything we hold that the snapshot does not mention is gone from the
        // host's view -> "absent". Guarded by seq so a module we learned about
        // from a delta that is NEWER than this snapshot is not dropped by a
        // snapshot that predates it.
        // There is no `state == kAbsent` case to skip here any more: `records`
        // is membership, so nothing in it can be absent.
        std::vector<std::string> toDrop;
        for (auto& kv : reg().records) {
            if (present.count(kv.first) != 0)
                continue;
            if (kv.second.seq >= listing.seq)
                continue;
            // Stamped with the listing's seq. Core's counter is global and
            // monotonic, so any later real delta about this module necessarily
            // carries a higher seq and still wins the replay rule.
            pending.push_back(PendingEvent{kv.first, kv.second.instance, kv.second.pid,
                                           kv.second.state, std::string(kAbsent),
                                           std::optional<std::string>("not present in host snapshot"),
                                           listing.seq});
            toDrop.push_back(kv.first);
        }
        for (const std::string& name : toDrop) {
            reg().records.erase(name);
            // Same tombstone the note_transition membership edge leaves, for
            // the same reason and at the seq the event above was stamped with.
            reg().departedSeq[name] = listing.seq;
        }

        if (listing.seq > reg().highWaterSeq)
            reg().highWaterSeq = listing.seq;

        // The host has now spoken about the whole set, so `partial` stops being
        // "we have only seen deltas" and becomes whatever the host reported:
        // true only when the host's own scan skipped something.
        reg().partial = listing.partial;
    }

    // Lock released. Safe to re-enter.
    for (const PendingEvent& e : pending)
        module_state_changed(e.module, e.instance, e.pid, e.oldState, e.newState,
                             e.reason, e.seq);
    return true;
}
