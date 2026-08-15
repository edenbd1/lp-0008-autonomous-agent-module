# LP-0008 owner console — the Basecamp `ui` plugin half of the submission.
#
# Deliberately a plain Qt build rather than a `logos_module()` one. `ui` plugins
# are not Logos Core modules: Basecamp loads them with QPluginLoader out of its
# plugins directory and casts them to `IComponent`, and the module-builder's
# helper builds the `core` shape (a provider object, generated dispatch, a
# `logos_host` process). Running it here would produce the wrong artefact.
#
#   cmake -S app -B build-ui -DCMAKE_PREFIX_PATH=$HOME/logos/Qt/6.9.2/macos \
#         -DLOGOS_CPP_SDK_ROOT=$HOME/logos/src/logos-cpp-sdk
#   cmake --build build-ui
#
# Qt is a CEILING, not a floor: Basecamp 0.2.2 bundles Qt 6.9.2 and refuses a
# plugin built against a higher minor. See docs/basecamp.md.
cmake_minimum_required(VERSION 3.20)
project(lp0008_agent_ui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# The SDK is headers-only for this target. Nothing from it is compiled in and
# nothing from it is linked: `LogosAPI`, `LogosAPIClient` and `TokenManager`
# are already in the process, exported by the `liblogos_core` Basecamp links,
# and that is where they must come from.
#
# Compiling the SDK's translation units in instead is the expensive mistake:
# `TokenManager::instance()` is a singleton, and a second copy inside this
# plugin wins the symbol for this plugin's own calls, starts empty, and every
# call comes back `ModuleProxy: rejecting unauthorized call … auth token not
# recognized` — which reads as a permissions problem and is not. Basecamp's own
# `main_ui.dylib` carries the same symbols as UNDEFINED; `nm -u` on it is the
# check.
set(LOGOS_CPP_SDK_ROOT "$ENV{LOGOS_CPP_SDK_ROOT}" CACHE PATH
    "Checkout of logos-co/logos-cpp-sdk (headers only for this target)")
if(NOT LOGOS_CPP_SDK_ROOT OR NOT EXISTS "${LOGOS_CPP_SDK_ROOT}/cpp/logos_api.h")
    message(FATAL_ERROR
        "logos-cpp-sdk not found. Pass -DLOGOS_CPP_SDK_ROOT=/path/to/logos-cpp-sdk "
        "or set the environment variable. docs/basecamp.md pins it at c87f343.")
endif()

# moc resolves Q_PLUGIN_METADATA(FILE "…") relative to the header that declares
# the plugin, so stage a copy of the manifest beside it.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/metadata.json
               ${CMAKE_CURRENT_SOURCE_DIR}/src/metadata.json COPYONLY)

qt_add_plugin(agent_ui CLASS_NAME AgentUiPlugin)

target_sources(agent_ui PRIVATE
    src/plugin.h
    src/plugin.cpp
    src/agent_console.h
    src/agent_console.cpp
)

target_include_directories(agent_ui PRIVATE
    ${LOGOS_CPP_SDK_ROOT}/cpp
    ${LOGOS_CPP_SDK_ROOT}/core
)

target_link_libraries(agent_ui PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

# nlohmann/json.hpp comes in through the SDK's logos_json.h. Header-only, and
# only for declarations — no nlohmann symbol ends up in this plugin.
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
          HINTS /opt/homebrew/include /usr/local/include /usr/include)
if(NLOHMANN_JSON_INCLUDE_DIR)
    target_include_directories(agent_ui PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()

# Let the host resolve the SDK symbols at load time. Without this the link
# fails on every LogosAPI reference; with it, they bind to Basecamp's copies.
if(APPLE)
    target_link_options(agent_ui PRIVATE -Wl,-undefined,dynamic_lookup)
else()
    # ELF leaves undefined symbols in a shared object by default.
    target_link_options(agent_ui PRIVATE -Wl,--allow-shlib-undefined)
endif()

# Basecamp resolves `main` from the manifest inside the plugin directory, and
# the packaging script writes `agent_ui` there. The default `lib` prefix would
# make the manifest name a file that is not present — which fails the load
# before Qt is reached, and logs nothing.
set_target_properties(agent_ui PROPERTIES OUTPUT_NAME "agent_ui" PREFIX "")

# Drop the manifests beside the binary so the build directory can be installed
# by copy, exactly as the packaged variant is.
add_custom_command(TARGET agent_ui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/metadata.json
        $<TARGET_FILE_DIR:agent_ui>/metadata.json)
add_custom_command(TARGET agent_ui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/metadata.json
        $<TARGET_FILE_DIR:agent_ui>/module.json)
