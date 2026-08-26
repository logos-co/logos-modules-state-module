// The invariants that are cheap to state and expensive to lose. Each is
// load-bearing for a claim the header makes, and each was previously checked
// only by driving a live daemon by hand — which CI does not do.

#include <logos_test.h>
#include "../src/modules_state_impl.h"

#include <cstdlib>
#include <string>
#include <vector>

// The emitter is declared on the impl and defined by the generated scaffold,
// which a unit test does not link. Stubbing it is what makes the emissions
// assertable, so the membership edges are checked rather than assumed.
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

// A unit test calls the impl DIRECTLY: no dispatch, so no caller, so
// currentCaller() answers Unknown and the real gate refuses. The test door is
// what makes these checkable in CI; the closed-gate case below opens none.
bool note(ModulesStateImpl& m, const std::string& mod,
          const std::string& from, const std::string& to, uint64_t seq)
{
    return m.note_transition(mod, std::nullopt, std::nullopt,
                             from, to, std::nullopt, seq);
}

bool listingHas(const ModuleListing& l, const std::string& mod)
{
    for (const ModuleRecord& r : l.modules)
        if (r.module == mod) return true;
    return false;
}

} // namespace

// INVARIANT 1 — no surface hands back a record whose state is "absent". If one
// could, there would be two spellings for "not there" and every consumer would
// have to handle both, or quietly handle one.
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

// INVARIANT 2 — a departed module stays departed under a stale delta. Erasing
// the record also erases the seq the replay rule compares against, so without a
// tombstone an out-of-order push would resurrect it. Delivery is not ordered,
// so this is a real sequence, not a contrived one.
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
// `state` is tstr with no enum, so an omitted field would otherwise be admitted
// as a seventh, undeclared state indistinguishable from a real one.
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

    LOGOS_ASSERT(m.apply_snapshot(listing));
    LOGOS_ASSERT(!m.module_record("ghost_module").has_value());
    LOGOS_ASSERT(m.module_record("irc_module").has_value());
}

// The gate is closed unless a door is opened, and this case deliberately opens
// none. With no dispatch the caller is Unknown, which is NOT the host, so the
// structural gate refuses — the same answer a peer module gets.
LOGOS_TEST(ingest_is_refused_when_no_door_is_open)
{
    unsetenv("LOGOS_MODULES_STATE_TEST_INGEST");
    ModulesStateImpl m;

    LOGOS_ASSERT(!note(m, "chat_module", "absent", "unloaded", 1));
    LOGOS_ASSERT(!m.module_record("chat_module").has_value());
    // ...and it is absent from the listing too. NOT modules.empty(): the
    // registry is a process-wide static, so earlier cases in this binary have
    // left records behind.
    LOGOS_ASSERT(!listingHas(m.list_modules(), "chat_module"));
}

// `loaded` is not `ready`. liblogos marks a module loaded when it owns the
// process, and emits loaded->ready only once the module publishes its object.
// is_ready() must answer false in that window — it is the window callers ask
// about — and an unknown state must not be optimistic either.
LOGOS_TEST(ready_is_publish_not_load)
{
    OpenIngest gate;
    ModulesStateImpl m;

    LOGOS_ASSERT(note(m, "eth_rpc_module", "absent", "unloaded", 1));
    LOGOS_ASSERT(!m.is_ready("eth_rpc_module"));

    LOGOS_ASSERT(note(m, "eth_rpc_module", "unloaded", "loading", 2));
    LOGOS_ASSERT(!m.is_ready("eth_rpc_module"));

    // The window: the host owns the process, the object is not up yet.
    LOGOS_ASSERT(note(m, "eth_rpc_module", "loading", "loaded", 3));
    LOGOS_ASSERT(!m.is_ready("eth_rpc_module"));

    LOGOS_ASSERT(note(m, "eth_rpc_module", "loaded", "ready", 4));
    LOGOS_ASSERT(m.is_ready("eth_rpc_module"));

    // Going away closes it again.
    LOGOS_ASSERT(note(m, "eth_rpc_module", "ready", "stopping", 5));
    LOGOS_ASSERT(!m.is_ready("eth_rpc_module"));

    // A module nobody reported is not ready, and neither is an unrecognised
    // state — forward compatibility fails closed.
    LOGOS_ASSERT(!m.is_ready("never_seen_module"));
    LOGOS_ASSERT(note(m, "eth_rpc_module", "stopping", "quiescing", 6));
    LOGOS_ASSERT(!m.is_ready("eth_rpc_module"));
}
