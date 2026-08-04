# SPDX-License-Identifier: GPL-3.0-or-later
#
# holocron-analyze: the offline analysis harness (#3 / O-002).
#
# Deliberately not scaffolding. It is how an analysis change gets checked
# against a real file without a renderer, and it is the only executable in the
# project until the spine lands.

add_executable(holocron-analyze
    analyze/main.cpp
)

target_link_libraries(holocron-analyze
    PRIVATE
        holocron::contract
        holocron::analysis
        holocron::decoder
        holocron::warnings
)

set_target_properties(holocron-analyze PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# holocron: the spine.
#
# The first executable that plays a file and draws it -- decode, analysis,
# TripleBuffer, debug facet, and SdlSink, in one process. Every piece was tested
# on its own before this existed; this is what checks whether they agree.
# ---------------------------------------------------------------------------

add_executable(holocron
    player/main.cpp
)

target_link_libraries(holocron
    PRIVATE
        holocron::contract
        holocron::analysis
        holocron::decoder
        holocron::sdl_sink
        holocron::wasapi_sink
        holocron::render
        holocron::config
        holocron::plex
        holocron::warnings
        # The decode thread is a std::thread; gcc/libstdc++ needs an explicit
        # pthread link for that and MSVC needs nothing.
        Threads::Threads
)

set_target_properties(holocron PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
