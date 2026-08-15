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

# One of the translation units above is compiled and linked but is not
# constructed by anything inside the plugin: `owner_channel.cpp`. That is
# deliberate and it is not the same as leaving it out.
#
# It is host-owned: an `OwnerChannel` needs a delivery transport, which is a
# struct of `std::function`s, and those cannot cross a plugin boundary — the
# same reason the twenty-one skills register with nothing wired (see
# `registerBuiltinSkills` in src/agent_module_plugin.h). A host that *links*
# this module constructs it; a host that *loads* it as a plugin cannot. What a
# loaded plugin has instead is the owner channel built out of the runtime's own
# surface: the `ownerApprovalRequested` event out and the `approveSpend` method
# back, both wired in src/agent_module_plugin.cpp.
#
# Linking it anyway is what makes that distinction possible at all. Left out of
# SOURCES, the code is absent from the shipped `agent.lgx` outright, so a host
# that links the module cannot call it either, and the suite CI runs against
# that file describes a property no artefact has.
#
# `task_persistence.cpp` used to be in this note and no longer belongs in it:
# 121 assertions green in CI, and not one construction site in the plugin. It is
# now constructed in `AgentModuleImpl::onContextReady` against the host's
# per-instance directory, recovered from in `start()`, and checkpointed after
# every `invoke()` that moved the task store. Compiled in CI is not shipped, and
# shipped-but-never-constructed is not running.
#
# There is a third step in that sequence and it is the one that has actually
# gone wrong twice: shipped is not necessarily built from what is committed.
# Everything above describes the relationship between this file and the binary
# it produces, and says nothing about whether `agent.lgx` — the file a reviewer
# downloads — was produced from the sources named above or from the sources as
# they stood ten commits ago. `module/package-basecamp.sh` writes
# `module/agent.lgx.sources` at packaging time and
# `scripts/check-package-fresh.py` recomputes it on every CI run, which is what
# closes it.
