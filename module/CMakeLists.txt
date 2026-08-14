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
)
