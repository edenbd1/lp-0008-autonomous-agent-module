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

# Logos Delivery, linked INTO the plugin.
#
# This is the switch that decides whether a *loaded* module can reach the
# network at all. `SkillPorts` is a struct of `std::function`s, so a host that
# loads this over Qt Remote Objects cannot hand one over — but the module can
# build its own out of a node it opened, and that needs the library in the
# binary. Off by default because the library is a 40 MB Nim build that has to
# come from somewhere; point LOGOS_DELIVERY_ROOT at a checkout that has been
# through `make liblogosdelivery` and the plugin links it, defines
# LP0008_WITH_DELIVERY, and `meta.configure("delivery","on")` starts a node.
#
#   cmake -S module -B build -DLOGOS_DELIVERY_ROOT=$PWD/_external/logos-delivery
#
# Without it, `src/delivery_runtime.cpp` still compiles and still ships — every
# entry point answers "no" and `meta.status` reports the transport as `absent`
# rather than `off`, which is the difference between a node nobody started and a
# binary that could not start one.
#
# THE BUILD INPUT IS THE HEADER, NOT THE LIBRARY, AND THAT MATTERS FOR LINUX
#
# The library is opened with `dlopen` at the moment a node is first asked for
# (see the note below), so nothing links it and the only thing the compiler
# needs is `library/liblogosdelivery.h`. That is normally a distinction without
# a difference — a checkout that has been through `make liblogosdelivery` has
# both — and on one platform it is the whole difference:
# `logos-messaging/logos-delivery` publishes **no Linux library**, and no
# release of it ever has (`module/third-party/liblogosdelivery/README.md` has
# the audit). A linux-amd64 build therefore has the header and cannot have the
# binary, and it must not be the same build as one that was pointed at nothing.
#
#   -DLOGOS_DELIVERY_ROOT=…                              headers + library
#   -DLOGOS_DELIVERY_ROOT=… -DLOGOS_DELIVERY_LIBRARY_OPTIONAL=ON
#                                                        headers only
#   (neither)                                            no delivery code at all
#
# Those are three states and the module reports three answers, which is the
# reason for the option rather than a `-DCMAKE_CXX_FLAGS=-DLP0008_WITH_DELIVERY`
# on the command line. `meta.status` says `absent` for the third — a binary that
# could not start a node — and for the second it says it tried to open
# `liblogosdelivery.so`, names every path it tried, and every one of the 25
# skills is still registered. A build log that said "WITHOUT Logos Delivery"
# over a binary carrying the code path would be the same class of defect this
# file's other comments exist to prevent.
#
# It also keeps `scripts/check-package-fresh.py` honest across variants. That
# check requires every source literal of >= 8 bytes to be inside every shipped
# binary; the delivery code path holds about ninety of them, and a linux variant
# compiled without `LP0008_WITH_DELIVERY` could never satisfy it. Either the
# check would have to learn about `#ifdef`s, or the variants have to be built
# from the same set of translation units. The second is cheaper and stronger.
set(LOGOS_DELIVERY_ROOT "" CACHE PATH "logos-delivery checkout with library/liblogosdelivery.h and, normally, build/liblogosdelivery.{dylib,so}")
option(LOGOS_DELIVERY_LIBRARY_OPTIONAL
       "Compile the delivery code path from the header alone, for a platform upstream ships no library for" OFF)
set(_agent_delivery_lib "")
set(_agent_delivery_headers "")
if(LOGOS_DELIVERY_ROOT)
    if(NOT EXISTS "${LOGOS_DELIVERY_ROOT}/library/liblogosdelivery.h")
        message(FATAL_ERROR
            "LOGOS_DELIVERY_ROOT=${LOGOS_DELIVERY_ROOT} has no "
            "library/liblogosdelivery.h. That header is the build input; "
            "without it nothing in src/delivery_runtime.cpp can compile.")
    endif()
    set(_agent_delivery_headers "${LOGOS_DELIVERY_ROOT}/library")
    foreach(_ext dylib so)
        if(EXISTS "${LOGOS_DELIVERY_ROOT}/build/liblogosdelivery.${_ext}")
            set(_agent_delivery_lib "${LOGOS_DELIVERY_ROOT}/build/liblogosdelivery.${_ext}")
            break()
        endif()
    endforeach()
    if(NOT _agent_delivery_lib AND NOT LOGOS_DELIVERY_LIBRARY_OPTIONAL)
        message(FATAL_ERROR
            "LOGOS_DELIVERY_ROOT=${LOGOS_DELIVERY_ROOT} has no "
            "build/liblogosdelivery.{dylib,so}. Build it first (`make "
            "liblogosdelivery`); refusing to configure a delivery-enabled module "
            "that would silently fall back to the unlinked build.\n"
            "If this is a platform upstream publishes no library for — Linux is "
            "one — say so: -DLOGOS_DELIVERY_LIBRARY_OPTIONAL=ON. The plugin then "
            "loads, registers every skill, and meta.status reports the library it "
            "could not open together with every path it tried.")
    endif()
endif()

logos_module(
    NAME agent
    SOURCES
        src/agent_module_interface.h
        src/agent_module_plugin.h
        src/agent_module_plugin.cpp
        src/delivery_runtime.h
        src/delivery_runtime.cpp
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
        src/owner_skills.h
        src/owner_skills.cpp
        src/spend_marker.h
        src/spend_marker.cpp
        src/task_persistence.h
        src/task_persistence.cpp
)

# THE FIVE SDK SYMBOLS THE PINNED BUILDER LEAVES UNDEFINED
#
# `logos-module-builder` at 5396513 compiles eight of the SDK's translation
# units into every plugin and not `logos_types.cpp`, so a plugin built with it
# carries `operator<<(QDataStream&, const LogosResult&)` and its `>>` as
# UNDEFINED — along with three `LogosTransportFactory::create*` overloads that
# `logos_api_provider.cpp` references.
#
# On Mach-O that is invisible: the target links `-undefined dynamic_lookup`,
# dyld binds lazily, and nothing ever faulted. On ELF it is not invisible, and
# the failure is worth writing down because it happens after everything looks
# right — the module loads, joins the runtime's loaded set, hands out a client,
# and then the first call across the transport ends in
#
#   .logos_host.elf: symbol lookup error: agent_plugin.so:
#     undefined symbol: _ZlsR11QDataStreamRK11LogosResult
#
# and `configure()` times out exactly as a Qt mismatch does. `logos_host` links
# spdlog and Qt and NOT `liblogos_core`, on either platform, so there is nothing
# in that process to bind against. Basecamp's own `capability_module_plugin.so`
# has zero undefined Logos symbols; a module plugin is supposed to be
# self-contained, and this one was not.
#
# `logos_types.cpp` is fifteen lines and pulls in nothing. The three
# `LogosTransportFactory` overloads are a different matter — the factory drags
# in four transport implementations, Boost and OpenSSL — and they stay
# undefined, deliberately: they are referenced by a constructor path this
# deployment does not take, and `app/package-ui.sh` already treats undefined
# Logos symbols as correct when the host defines them. If a build ever faults on
# one, the fix is the SDK's own source list, not a stub.
# `logos_find_dependencies()` sets LOGOS_CPP_SDK_ROOT with PARENT_SCOPE, which
# does not reach this scope, so resolve it the same way the helper does rather
# than adding "/cpp/logos_types.cpp" to nothing.
if(NOT LOGOS_CPP_SDK_ROOT)
    set(LOGOS_CPP_SDK_ROOT "$ENV{LOGOS_CPP_SDK_ROOT}")
endif()
if(NOT EXISTS "${LOGOS_CPP_SDK_ROOT}/cpp/logos_types.cpp")
    message(FATAL_ERROR
        "logos_types.cpp not found under LOGOS_CPP_SDK_ROOT="
        "${LOGOS_CPP_SDK_ROOT}. Set LOGOS_CPP_SDK_ROOT (docs/basecamp.md pins "
        "the checkout at c87f343); without it the plugin loads and then dies on "
        "`undefined symbol: _ZlsR11QDataStreamRK11LogosResult`.")
endif()
target_sources(agent_module_plugin PRIVATE
    "${LOGOS_CPP_SDK_ROOT}/cpp/logos_types.cpp")

if(_agent_delivery_headers)
    # HEADERS ONLY. The library is opened with `dlopen` at the moment a node is
    # first asked for (`deliverySymbols()` in src/delivery_runtime.cpp), not
    # linked, and this is a deliberate choice with a measured reason.
    #
    # Linked, the plugin carries `@rpath/liblogosdelivery.dylib`, and a module
    # directory missing that one file makes the WHOLE PLUGIN fail to load:
    #
    #   Cannot load library .../agent_plugin.dylib: dlopen(...): Library not
    #   loaded: @rpath/liblogosdelivery.dylib ... (no such file)
    #
    # A harness prints that. Basecamp does not: a module that fails to load
    # shows no error at all — the tile is inert and the reason goes to stderr.
    # So the most likely installation mistake there is (unpack the package, miss
    # one file while flattening the variant) produces exactly the failure this
    # repository refuses to ship: something that installed, looks enabled, and
    # does nothing, with nothing anywhere saying why.
    #
    # Opened at run time instead, the plugin always loads, all 22 skills
    # register, and `meta.status` answers with the file it wanted and every path
    # it tried. The library still ships beside the plugin and is still found
    # there first — `dladdr` gives the plugin's own directory, which is the one
    # `@loader_path` would have named — so nothing about the install changes.
    target_compile_definitions(agent_module_plugin PRIVATE LP0008_WITH_DELIVERY)
    target_include_directories(agent_module_plugin PRIVATE "${_agent_delivery_headers}")

    if(_agent_delivery_lib)
        # Staged beside the plugin so `package-basecamp.sh` picks it up, and by
        # name rather than by absolute path: a path into a build tree resolves on
        # the build machine and nowhere else, which is the same defect that
        # script already refuses for Qt.
        get_filename_component(_agent_delivery_name "${_agent_delivery_lib}" NAME)
        add_custom_command(TARGET agent_module_plugin POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_agent_delivery_lib}"
                    "${CMAKE_BINARY_DIR}/modules/${_agent_delivery_name}"
            COMMENT "Staging ${_agent_delivery_name} beside the plugin")
        if(APPLE)
            add_custom_command(TARGET agent_module_plugin POST_BUILD
                COMMAND install_name_tool -id "@rpath/${_agent_delivery_name}"
                        "${CMAKE_BINARY_DIR}/modules/${_agent_delivery_name}"
                COMMENT "Naming the staged ${_agent_delivery_name} by rpath")
        endif()
        message(STATUS "Agent module can open Logos Delivery: ${_agent_delivery_lib}")
    else()
        message(STATUS
            "Agent module carries the Logos Delivery code path and no library "
            "(LOGOS_DELIVERY_LIBRARY_OPTIONAL=ON). Every skill registers; "
            "meta.status will report the library it could not open and every "
            "path it tried.")
    endif()
else()
    message(STATUS "Agent module builds WITHOUT Logos Delivery "
                   "(set -DLOGOS_DELIVERY_ROOT=... to enable it)")
endif()

# `owner_channel.cpp` used to carry a note here saying it was compiled, linked,
# and constructed by nothing inside the plugin — "host-owned", because "an
# `OwnerChannel` needs a delivery transport, which is a struct of
# `std::function`s, and those cannot cross a plugin boundary. A host that
# *links* this module constructs it; a host that *loads* it as a plugin
# cannot."
#
# The first half of that is still true and the conclusion was wrong, which is
# worth leaving on the page because the same mistake was made three times in
# this repository under three different names. A host cannot PASS a closure over
# Qt Remote Objects. It does not follow that a loaded module cannot HAVE one:
# `src/delivery_runtime.cpp` opens a Logos Delivery node from the module's own
# configuration and builds `DeliveryPort` and `OwnerChannelPort` out of it,
# entirely on the module's side of the boundary. `AgentModuleImpl` now
# constructs an `OwnerChannel` on that node
# (`publishApprovalOverDelivery`), so the class is constructed inside the plugin
# and not only by a host that links it — and the runtime-event path
# (`ownerApprovalRequested` out, `approveSpend` back) remains as the fallback
# for a deployment with no node configured.
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
