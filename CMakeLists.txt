# modules_state — read-only registry of module lifecycle state.
#
# A universal module: only the impl class is hand-written; the Qt plugin glue
# (interface + plugin classes) and the LIDL contract are generated from
# src/modules_state_impl.h by logos-module-builder.

cmake_minimum_required(VERSION 3.14)
project(ModulesStatePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()

# Derive the module name from metadata.json — single source of truth.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET ${METADATA_JSON} name)

logos_module(
    NAME ${MODULE_NAME}
    SOURCES
        src/modules_state_impl.h
        src/modules_state_impl.cpp
)
