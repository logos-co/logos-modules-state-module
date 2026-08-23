// The three invariants that are cheap to state and expensive to lose.
//
// Each is load-bearing for a claim the header makes, and each was previously
// checked only by driving a live logoscore daemon by hand — which is not a
// thing CI does, so in practice they were unguarded.
//
// The ingest gate is opened here with LOGOS_MODULES_STATE_TEST_INGEST=1, which
// is the documented test-only door. A run WITHOUT it is the proof that the gate
// is closed by default; that belongs in its own case, below.

#include <logos_test.h>
#include "../src/modules_state_impl.h"

#include <cstdlib>
#include <string>
#include <vector>

// The event emitter is DECLARED on the impl and DEFINED by the generated
// scaffold, which a unit test does not link. Stubbing it here is not a
// workaround — it is what makes the emissions assertable at all, so the
// membership edges can be checked rather than assumed.
std::vector<std::string> g_emitted;

void ModulesStateImpl::module_state_changed(const std::string& module,
                                            const std::optional<std::string>&,
                                            const std::optional<int64_t>&,
                                            const std::string& old_state,
                                            const std::string& new_state,
                                            const std::optional<std::string>&,
                                            uint64_t)
{
    g_emitted.push_back(module + ":" + old_state + "->" + new_state);
}

namespace {

// The gate reads the environment on every call, so scoping it per test keeps
// one case from opening the door for the next.
struct OpenIngest {
    OpenIngest()  { setenv("LOGOS_MODULES_STATE_TEST_INGEST", "1", 1); }
    ~OpenIngest() { unsetenv("LOGOS_MODULES_STATE_TEST_INGEST"); }
};

constexpr const char* kTok = "test-ingest";

bool note(ModulesStateImpl& m, const std::string& mod,
          const std::string& from, const std::string& to, uint64_t seq)
{
    return m.note_transition(kTok, mod, std::nullopt, std::nullopt,
                             from, to, std::nullopt, seq);
}

bool listingHas(const ModuleListing& l, const std::string& mod)
{
    for (const ModuleRecord& r : l.modules)
        if (r.module == mod) return true;
    return false;
}

} // namespace

// INVARIANT 1 — no surface may ever hand back a record whose state is "absent".
//
// `absent` is an EVENT-ONLY transition target. If a record could carry it,
// there would be two spellings for "not there" — a record saying absent, and
// the empty optional — and every consumer would have to handle both or quietly
// handle one.
LOGOS_TEST(absent_never_appears_on_a_record)
{
    OpenIngest gate;
    ModulesStateImpl m;

    LOGOS_ASSERT(note(m, "chat_module", "absent", "unloaded", 1));
    LOGOS_ASSERT(note(m, "chat_module", "unloaded", "loaded", 2));
    LOGOS_ASSERT(m.module_record("chat_module").has_value());

    // The membership edge OUT. The event fires; the record must go.
    LOGOS_ASSERT(note(m, "chat_module", "loaded", "absent", 3));

    LOGOS_ASSERT(!m.module_record("chat_module").has_value());
    const ModuleListing after = m.list_modules();
    LOGOS_ASSERT(!listingHas(after, "chat_module"));
    for (const ModuleRecord& r : after.modules)
        LOGOS_ASSERT(r.state != "absent");

    // ...and the edge still RODE as an event. Dropping the record silently
    // would satisfy every assertion above while destroying the only thing a
    // consumer can act on.
    bool sawIn = false, sawOut = false;
    for (const std::string& e : g_emitted) {
        if (e == "chat_module:absent->unloaded") sawIn = true;
        if (e == "chat_module:loaded->absent")   sawOut = true;
    }
    LOGOS_ASSERT(sawIn);
    LOGOS_ASSERT(sawOut);
}

// INVARIANT 2 — a departed module stays departed under a stale delta.
//
// Erasing the record also erases the seq the replay rule compares against, so
// without a tombstone an out-of-order push for a pruned module would resurrect
// it. Deliveries are NOT ordered, so this is a real sequence, not a contrived
// one.
LOGOS_TEST(a_stale_delta_does_not_resurrect_a_departed_module)
{
    OpenIngest gate;
    ModulesStateImpl m;

    LOGOS_ASSERT(note(m, "waku_module", "absent",   "unloaded", 10));
    LOGOS_ASSERT(note(m, "waku_module", "unloaded", "loaded",   12));
    LOGOS_ASSERT(note(m, "waku_module", "loaded",   "absent",   14));
    LOGOS_ASSERT(!m.module_record("waku_module").has_value());

    // Older than the departure: must be refused, and must not recreate it.
    LOGOS_ASSERT(!note(m, "waku_module", "unloaded", "loaded", 13));
    LOGOS_ASSERT(!m.module_record("waku_module").has_value());

    // Strictly newer is a genuine re-discovery and IS applied.
    LOGOS_ASSERT(note(m, "waku_module", "absent", "unloaded", 15));
    LOGOS_ASSERT(m.module_record("waku_module").has_value());
}

// INVARIANT 3 — a snapshot record with no state is skipped, like an absent one.
//
// `state` is tstr on the wire with no enum behind it, so a record that simply
// omitted the field would otherwise be admitted with state "" — a seventh,
// undeclared state, reachable from outside and indistinguishable from a real
// one to every consumer.
LOGOS_TEST(a_snapshot_record_with_no_state_is_not_admitted)
{
    OpenIngest gate;
    ModulesStateImpl m;

    ModuleRecord stateless;
    stateless.module = "ghost_module";
    stateless.seq    = 5;
    // .state deliberately left empty

    ModuleRecord real;
    real.module = "irc_module";
    real.state  = "loaded";
    real.seq    = 5;

    ModuleListing listing;
    listing.modules = { stateless, real };
    listing.seq     = 5;

    LOGOS_ASSERT(m.apply_snapshot(kTok, listing));
    LOGOS_ASSERT(!m.module_record("ghost_module").has_value());
    LOGOS_ASSERT(m.module_record("irc_module").has_value());
}

// The gate is closed unless a door is opened, and this case deliberately opens
// none. It is the reason the others may open one without weakening the claim.
LOGOS_TEST(ingest_is_refused_when_no_door_is_open)
{
    unsetenv("LOGOS_MODULES_STATE_TEST_INGEST");
    unsetenv("LOGOS_MODULES_STATE_INGEST_TOKEN");
    ModulesStateImpl m;

    LOGOS_ASSERT(!note(m, "chat_module", "absent", "unloaded", 1));
    LOGOS_ASSERT(!m.module_record("chat_module").has_value());
    LOGOS_ASSERT(m.rejected_ingest_count() >= 1);
}
