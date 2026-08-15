# LP-0008 agent module for Logos Core.
#
# The builder is normally driven from Nix. It does not have to be: the helper is
# located from LOGOS_MODULE_BUILDER_ROOT first, so a local checkout is enough.
#
#   LOGOS_MODULE_BUILDER_ROOT=/path/to/logos-module-builder \
#   LOGOS_MODULE_ROOT=/path/to/logos-module \
#   LOGOS_CPP_SDK_ROOT=/path/to/logos-cpp-sdk \
#   cmake -S module -B build && cmake --build build
cmake_minimum_required(VERSION 3.14)
project(AgentModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR
        "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT to a checkout "
        "of https://github.com/logos-co/logos-module-builder")
endif()

logos_module(
    NAME agent
    SOURCES
        src/agent_module_interface.h
        src/agent_module_plugin.h
        src/agent_module_plugin.cpp
        src/messaging_skills.h
        src/messaging_skills.cpp
        src/storage_skills.h
        src/storage_skills.cpp
        src/inference.h
        src/inference.cpp
        src/wallet_skills.h
        src/wallet_skills.cpp
        src/program_skills.h
        src/program_skills.cpp
        src/agent_skills.h
        src/agent_skills.cpp
        src/owner_channel.h
        src/owner_channel.cpp
        src/task_persistence.h
        src/task_persistence.cpp
)

# Two of the translation units above are compiled and linked but are not
# constructed by anything inside the plugin: `owner_channel.cpp` and
# `task_persistence.cpp`. That is deliberate and it is not the same as leaving
# them out.
#
# Both are host-owned: an `OwnerChannel` needs a delivery transport and a
# `TaskPersistence` needs a `TaskStorePort`, and both are `std::function`s,
# which cannot cross a plugin boundary — the same reason the twenty-one skills
# register with nothing wired (see `registerBuiltinSkills` in
# src/agent_module_plugin.h). A host that *links* this module constructs them;
# a host that *loads* it as a plugin cannot.
#
# Linking them anyway is what makes that distinction possible at all. Left out
# of SOURCES, the code is absent from the shipped `agent.lgx` outright, so a
# host that links the module cannot call it either, and the suites CI runs
# against these files describe a property no artefact has. `task_persistence.cpp`
# was in exactly that state: 121 assertions green in CI, and not one byte of the
# code they cover in the downloadable package. Compiled in CI is not shipped.
