cmake_minimum_required(VERSION 3.14)
project(ModulesStateTests LANGUAGES CXX)

include(LogosTest)

# The NAME must end in _test or _tests. mkLogosModuleTests runs the suite with
#   find . -maxdepth 1 -executable \( -name "*_tests" -o -name "*_test" \)
# and pipes the result into a while-read loop, so a binary named anything else
# is BUILT, MATCHED BY NOTHING, and never executed — and the check still goes
# green, because compiling was all that happened. This suite was written as
# `modules_state_invariants` and passed three mutation tests it should have
# caught before that was spotted.

# The registry logic only — no daemon, no Qt, no plugin load. These are the
# invariants that were previously checked only by driving logoscore by hand,
# which CI does not do, so they were unguarded in practice.
logos_test(
    NAME modules_state_invariants_tests
    MODULE_SOURCES
        ../src/modules_state_impl.cpp
    TEST_SOURCES
        main.cpp
        registry_invariants.cpp
)
