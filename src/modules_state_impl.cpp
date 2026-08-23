#include "modules_state_impl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// ═════════════════════════════════════════════════════════════════════════════
// THREAD SAFETY — read this before touching anything below.
//
// Where calls come from
//   Inbound method calls arrive through the generated Qt glue. With
//   `"concurrency": "single"` in metadata.json the glue dispatches one handler
//   at a time, so in THIS build the handlers are already serialised and the
//   mutex below is never contended.
//
// Why the mutex exists anyway — two independent reasons, either sufficient
//   1. `concurrency` is one metadata key. Flipping it to "multi" must not
//      silently introduce a data race in the one module whose entire job is to
//      be the trustworthy answer about system state. The lock makes that flip
//      a performance decision instead of a correctness decision.
//   2. The feed that is coming (liblogos ModuleManager) originates state
//      changes on the container's BACKGROUND asio thread — a module crash is
//      detected there, not on the main thread. Core is responsible for
//      marshalling that onto its Qt queue before it leaves the host, but this
//      module must not stake its own invariants on somebody else's marshalling
//      being correct.
//
// THE ONE RULE THAT MATTERS
//   Never hold m_mutex across an event emission.
//
//   module_state_changed() is a generated body that marshals into JSON and
//   crosses the C ABI into the host, which fans out to every subscriber. A
//   subscriber can call straight back in — is_ready() is the obvious one, and
//   it is exactly what a consumer reacting to an event would do. std::mutex is
//   not recursive, so emitting under the lock is a self-deadlock waiting for a
//   consumer to be written naturally.
//
//   Every mutator therefore follows the same shape, and there are no
//   exceptions to it:
//
//       compute under the lock -> collect pending events into a local vector
//       -> release the lock -> emit
//
//   That is the same compute-under-lock/dispatch-after discipline the liblogos
//   registry observer will use on the other side of the wire.
// ═════════════════════════════════════════════════════════════════════════════

namespace {

// ── The state vocabulary ─────────────────────────────────────────────────────
// Free functions rather than an enum: the wire type is `tstr` (LIDL has no
// enum, no union, no string-literal constraint), and an enum here would invite
// treating an unrecognised state as an error — the single thing the contract
// forbids.
//
// They live in this TU and not in the impl header on purpose: the generator
// parses the header as text to derive the contract, so anything in it that is
// not part of the contract is at best noise and at worst a dropped declaration.
//
// kAbsent is the EVENT-ONLY one, and the invariant it carries is checkable by
// grep. It is READ where a module leaves the view (note_transition's
// membership edge, and apply_snapshot's skip of a snapshot record that claims
// it), and WRITTEN only into a PendingEvent — as the previousState of a module
// a snapshot has just discovered, and as the new_state of one the snapshot
// pruned. It is never assigned to a stored record's `state`, in any path. That
// absence is the whole of list_modules' never-absent invariant.
constexpr const char* kAbsent = "absent";
constexpr const char* kLoaded = "loaded";
constexpr const char* kReady  = "ready";

// "Up and usable, as far as the host is concerned."
//
// `loaded` is in this set BECAUSE `ready` is not reachable yet — nothing emits
// it. If the set were {ready} alone, is_ready() would answer false forever and
// the method would be useless on the day it ships.
//
// When a real loaded->ready transition exists, `loaded` leaves this set. That
// is not a silent break: the only visible change is that is_ready() goes false
// during the loaded-but-not-yet-ready window, which is precisely the window a
// caller of is_ready() was always asking about. Consumers get more correct, not
// broken.
//
// Any state string this build does not recognise answers false — the same
// forward-compatibility fallback the contract imposes on consumers. A module
// that demands others tolerate unknown states has to do it itself.
bool stateIsReady(const std::string& state)
{
    return state == kLoaded || state == kReady;
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

// A fully zeroed record: the starting point for a module first learned from a
// delta, whose static metadata (path/type/version/deps) only a snapshot can
// fill in.
//
// It is NOT a value any surface returns, and it deliberately leaves `state`
// EMPTY. The one caller assigns a real state before storing it, and an empty
// state is not in the vocabulary — so if a future path ever stores this seed
// untouched, the result is visibly wrong rather than plausibly "absent". That
// is the whole reason this stopped being called absentRecord(): it used to be
// module_record()'s miss answer, and the miss is std::nullopt now.
//
// Written out explicitly rather than leaning on std::map::operator[]'s
// value-initialisation. That would in fact zero the members (ModuleRecord has
// no user-provided constructor), but the guarantee is a language subtlety, and
// the fields cannot carry default member initialisers: the impl header is
// parsed as text to derive the contract, and a `uint64_t seq = 0;` field line
// is a spelling the field scanner is not promised to read.
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
// The problem
//   note_transition() and apply_snapshot() write the facts every other module
//   is about to trust. If any module can call them, any module can forge a
//   lifecycle event — announce that a rival crashed, or that a module it wants
//   others to call is `ready`. The read surface is deliberately open; the
//   ingest surface must not be.
//
// Why the token is an ARGUMENT and not a policy entry
//   liblogos' AccessPolicy restricts by TARGET MODULE, with no per-method
//   granularity. Restricting modules_state to caller "core" would lock out
//   every reader, which is the entire point of the module. So the authority for
//   the two ingest methods has to ride in the call itself.
//
// Why this module cannot just check who called it
//   LogosModuleContext exposes this module's own identity (moduleName(),
//   instanceId(), modulePath()) and nothing about the CALLER. There is no
//   caller-identity accessor to check against, so a shared secret is what is
//   actually available today. If a caller-identity accessor lands later, this
//   gate should be rewritten to use it and the token retired — a structural
//   check beats a secret.
//
// The gate, and it is FAIL-CLOSED
//   LOGOS_MODULES_STATE_INGEST_TOKEN set and non-empty
//       -> ingest requires an exact match. This is the production shape: the
//          host generates a per-run nonce, puts it in the module subprocess's
//          environment when it spawns it, and sends it with every push. The
//          module subprocess's environment is not readable by other modules'
//          subprocesses, so the secret does not leak sideways.
//   unset
//       -> ALL ingest is refused. A build with no configured authority reports
//          only what it was given by someone who proved authority, which is
//          nothing. That is the correct answer, not an inconvenience.
//   unset AND LOGOS_MODULES_STATE_TEST_INGEST=1
//       -> the TEST-ONLY escape. Any token is accepted, and the module says so
//          loudly on stderr once at first use and again on every accepted call,
//          so an accidental production run is visible in the logs rather than
//          silent.
//
// The test escape is deliberately an ENVIRONMENT variable and not a method.
// A method — `enable_test_ingest()` — would itself be callable by any module,
// which would make the gate decorative. Environment is set by whoever launches
// the process, which is the host or the developer, and never by a peer module.
//
// STAGE 1 STATUS: nothing sets LOGOS_MODULES_STATE_INGEST_TOKEN yet, because
// liblogos does not push yet. Every Stage-1 verification run therefore uses
// LOGOS_MODULES_STATE_TEST_INGEST=1, and a run WITHOUT it is the proof that the
// gate is closed by default.
bool testIngestEnabled()
{
    const char* v = std::getenv("LOGOS_MODULES_STATE_TEST_INGEST");
    return v != nullptr && std::strcmp(v, "1") == 0;
}

const char* configuredIngestToken()
{
    const char* v = std::getenv("LOGOS_MODULES_STATE_INGEST_TOKEN");
    if (v == nullptr || v[0] == '\0')
        return nullptr;
    return v;
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

    // MEMBERSHIP IS THIS MAP. Every module the host's view contains, and
    // nothing else. No entry here ever has state "absent" — that state is an
    // event-only transition target, so leaving the view means leaving the map.
    std::map<std::string, StoredRecord> records;

    // Modules that WERE in `records` and are not any more, with the seq at
    // which each left. Tombstones, and they exist for exactly one reason: the
    // REPLAY RULE has to stay total.
    //
    // Before `absent` was narrowed, a departed module stayed in `records` as an
    // absent record and its `seq` was what a late delta was compared against.
    // Now it is erased — so without this table a delta that lost a race would
    // find no stored seq, pass the rule, and resurrect a module the host has
    // already pruned. This is the same tombstone, minus the fake record: one
    // uint64 instead of a whole ModuleRecord, and unreachable from any read
    // method, so it cannot be mistaken for membership.
    //
    // It grows with the number of modules ever seen, not with events. That is
    // bounded by what is installed on the machine.
    std::map<std::string, uint64_t> departedSeq;

    // Highest seq applied from any source. Reported as ModuleListing::seq.
    uint64_t highWaterSeq = 0;

    // See list_modules() for why this starts TRUE.
    bool partial = true;

    // Refusals by the authority gate specifically — NOT stale-seq drops and NOT
    // malformed arguments. Kept narrow so a test asserting "the gate is closed"
    // asserts exactly that.
    uint64_t rejectedIngest = 0;

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

// Shared by both ingest methods. Returns true when the caller proved authority;
// increments the refusal counter and complains on stderr when it did not.
static bool ingestAuthorised(const std::string& authToken)
{
    const char* expected = configuredIngestToken();

    if (expected != nullptr) {
        // Length-independent comparison is not worth it here: the token is a
        // per-run nonce and a peer module gets one guess per RPC, not a timing
        // oracle. Simplicity beats a false sense of hardening.
        if (authToken == expected)
            return true;
        std::lock_guard<std::mutex> lock(reg().mutex);
        ++reg().rejectedIngest;
        std::fprintf(stderr,
                     "[modules_state] REFUSED ingest: bad authToken\n");
        return false;
    }

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
                "[modules_state] LOGOS_MODULES_STATE_TEST_INGEST=1 and no\n"
                "[modules_state] LOGOS_MODULES_STATE_INGEST_TOKEN is set, so ANY\n"
                "[modules_state] caller can write lifecycle facts. This is for\n"
                "[modules_state] testing only. Never set this in production.\n");
        }
        std::fprintf(stderr, "[modules_state] TEST INGEST: accepting unauthenticated write\n");
        return true;
    }

    std::lock_guard<std::mutex> lock(reg().mutex);
    ++reg().rejectedIngest;
    std::fprintf(stderr,
        "[modules_state] REFUSED ingest: no LOGOS_MODULES_STATE_INGEST_TOKEN is\n"
        "[modules_state] configured, so this build accepts no writes at all.\n");
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
    // std::map iterates in key order, so the listing is sorted by module name.
    // Deterministic output is not cosmetic: it is what lets a test diff two
    // listings instead of set-comparing them.
    //
    // NOT FILTERED, on purpose. `records` is membership, so every entry belongs
    // in the listing and none of them can be "absent" — the two mutators are
    // the only writers and neither can store that state. A defensive filter
    // here would hide the bug it was written to catch, and would re-introduce
    // the second spelling of "not there" that narrowing `absent` removed.
    for (const auto& kv : reg().records)
        out.modules.push_back(kv.second);

    // `partial` starts TRUE and only a snapshot can clear it.
    //
    // Before a snapshot has arrived, everything here was learned from
    // individual deltas — which by construction only mention modules that
    // CHANGED since this module came up. A module that has been quietly loaded
    // the whole time is missing from that view. Reporting partial:false then
    // would be a confidently short list, which is the exact failure this flag
    // exists to prevent. After a snapshot, `partial` is whatever the host said:
    // true when the host's own scan skipped a module it could not read.
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
    // The miss. A tombstone is not a hit: a module that was pruned is exactly
    // as absent as one never discovered, and this method's job is to answer
    // membership, not history.
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

uint64_t ModulesStateImpl::rejected_ingest_count()
{
    std::lock_guard<std::mutex> lock(reg().mutex);
    return reg().rejectedIngest;
}

// ─────────────────────────────────────────────────────────────────────────────
// Ingest surface
// ─────────────────────────────────────────────────────────────────────────────

bool ModulesStateImpl::note_transition(const std::string& authToken,
                                       const std::string& module,
                                       const std::optional<std::string>& instance,
                                       const std::optional<int64_t>& pid,
                                       const std::string& old_state,
                                       const std::string& new_state,
                                       const std::optional<std::string>& reason,
                                       uint64_t seq)
{
    if (!ingestAuthorised(authToken))
        return false;

    // Malformed arguments are refused but NOT counted as an authority refusal:
    // rejected_ingest_count() has to mean "the gate turned someone away" and
    // nothing else, or it stops being usable as a test assertion.
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

        // THE REPLAY RULE. Applied if and only if seq is strictly newer than
        // what is stored for THIS module.
        //
        // Delivery is not ordered. The first push to an un-tokened target
        // coalesces behind a token handshake, so a later push can complete
        // first. Per-module seq makes that harmless with no in-flight buffer,
        // no timing assumption and no lock held across an RPC in either
        // direction: a stale delta is simply dropped.
        uint64_t stored = 0;
        if (storedSeqLocked(module, stored) && seq <= stored)
            return false;

        // THE MEMBERSHIP EDGE. `absent` is event-only, so a transition INTO it
        // does not store an absent record — it removes the module from the
        // listing and leaves a seq tombstone behind. The event still fires
        // below, carrying the pair the caller computed, and the high-water seq
        // still advances, so a consumer re-reading list_modules can still tell
        // that something moved.
        //
        // This is the whole mechanism behind list_modules' never-absent
        // invariant: there is no spelling of this method that puts an absent
        // record into `records`.
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
            // Two classes of field, treated differently on purpose:
            //
            //   instance / pid  are OVERWRITTEN, including to empty. They
            //     describe the incarnation this transition is ABOUT — an
            //     unload's pid is the pid that just went away — so a delta is
            //     authoritative for them and a delta that says "no pid" means
            //     there is no pid.
            //
            //   path / type / version / dependencies / dependents / loadedAt
            //     are SNAPSHOT-ONLY. A transition carries the lifecycle change,
            //     not the module's static metadata, so whatever a previous
            //     snapshot put there is preserved rather than blanked. A record
            //     first learned from a delta simply has them empty until a
            //     snapshot fills them.
        }

        if (seq > reg().highWaterSeq)
            reg().highWaterSeq = seq;

        // The pair forwarded is the one the CALLER computed, not
        // (ourStoredState -> new_state). Core computes old_state atomically
        // with its own write, so its pair is the authoritative description of
        // what happened; our stored value can only be older.
        pending = PendingEvent{module, instance, pid, old_state, new_state, reason, seq};
    }

    // Lock released. Safe to re-enter.
    if (pending)
        module_state_changed(pending->module, pending->instance, pending->pid,
                             pending->oldState, pending->newState, pending->reason,
                             pending->seq);
    return true;
}

bool ModulesStateImpl::apply_snapshot(const std::string& authToken,
                                      const ModuleListing& listing)
{
    if (!ingestAuthorised(authToken))
        return false;

    std::vector<PendingEvent> pending;
    {
        std::lock_guard<std::mutex> lock(reg().mutex);

        std::set<std::string> present;

        for (const ModuleRecord& incoming : listing.modules) {
            if (incoming.module.empty())
                continue;

            // A snapshot record claiming "absent" is contract-malformed: the
            // state is event-only, and a snapshot spells non-membership by
            // OMISSION. Skipping it makes it mean exactly what omitting it
            // would have meant — the prune loop below then drops whatever we
            // hold for that name. One rule for leaving the listing, not two.
            if (incoming.state == kAbsent)
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

            // An unknown module is one the host has just DISCOVERED, whether it
            // is unknown because we never heard of it or because it departed
            // and came back. Either way the edge it just crossed is
            // absent -> incoming.state, which is the membership edge.
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
