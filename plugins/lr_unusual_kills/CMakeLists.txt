# lr_unusual_kills — Plugify C++ port of the Pisex LR module "[LR] Unusual Kills"
# (the original LR_UnusualKills module, Metamod plugin lr_unusual_kills).
#
#   lr_unusual_kills_core       engine-free core (unusual_kills.ini model, flag evaluation,
#                               counters, SQL; also used by tests/unusual_tests.cpp)
#   plugify-lr_unusual_kills    the plugin shared library (entry "bin/lr_unusual_kills")

set(LR_UNUSUAL_KILLS_VERSION "1.0.1")

# --- core ---------------------------------------------------------------------
add_library(lr_unusual_kills_core STATIC src/unusual_core.cpp)
target_include_directories(lr_unusual_kills_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# pisexcompat brings the KeyValues parser (and, PUBLIC-linked, the s2sdk/hudmenu globals);
# plgdb is the database layer.
target_link_libraries(lr_unusual_kills_core PUBLIC plgdb pisexcompat)
target_compile_options(lr_unusual_kills_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_unusual_kills_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- generated levels_ranks bindings -------------------------------------------
# The __levels_ranks_* globals have to be compiled into every consumer plugin; plugins/levels_ranks
# builds them as the object library `lr_bindings` (it sorts before this directory), this is the
# fallback for a build of this directory alone.
if(NOT TARGET lr_bindings AND EXISTS
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    add_library(lr_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    target_include_directories(lr_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(lr_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

# --- plugin -------------------------------------------------------------------
add_library(plugify-lr_unusual_kills SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:lr_bindings>
)
target_include_directories(plugify-lr_unusual_kills PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated lr/plg/s2sdk headers and lr_api.hpp must not fail -Werror on
# our sources.
target_include_directories(plugify-lr_unusual_kills SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include"
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-lr_unusual_kills PRIVATE
        lr_unusual_kills_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-lr_unusual_kills PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-lr_unusual_kills PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="lr_unusual_kills"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-lr_unusual_kills PROPERTIES OUTPUT_NAME lr_unusual_kills)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-lr_unusual_kills PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/lr_unusual_kills.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/lr_unusual_kills.pplugin"
)

# --- tests ---------------------------------------------------------------------
# Engine-free tests (config against the shipped file, flag evaluation, SQL against SQLite and
# the test MySQL of ./test.sh); build/tests so ../test.sh runs them.
add_executable(lr_unusual_kills_tests tests/unusual_tests.cpp)
target_link_libraries(lr_unusual_kills_tests PRIVATE lr_unusual_kills_core -static-libstdc++ -static-libgcc)
target_compile_options(lr_unusual_kills_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_unusual_kills_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        LR_UNUSUAL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set_target_properties(lr_unusual_kills_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
