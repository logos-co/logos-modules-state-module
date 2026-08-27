cmake_minimum_required(VERSION 3.14)
project(ModulesStateTests LANGUAGES CXX)

include(LogosTest)

# NAME must end in _test or _tests. mkLogosModuleTests finds suites with
#   find . -maxdepth 1 -executable \( -name "*_tests" -o -name "*_test" \)
# so a binary named anything else is BUILT, MATCHED BY NOTHING, never executed —
# and the check still goes GREEN. This suite was `modules_state_invariants` and
# passed three mutation tests it should have caught before that was spotted.

# Registry logic only: no daemon, no Qt, no plugin load.
logos_test(
    NAME modules_state_invariants_tests
    MODULE_SOURCES
        ../src/modules_state_impl.cpp
    TEST_SOURCES
        main.cpp
        registry_invariants.cpp
)
