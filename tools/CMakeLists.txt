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

# ON ANDROID IT IS A SHARED LIBRARY, NOT AN EXECUTABLE, and that is the platform's
# shape rather than a workaround. An Android application is a Java Activity; the
# native side is a .so the Activity dlopens, and SDL's own Activity then looks up
# the C symbol `SDL_main` in it. There is no process for an ELF executable to be.
#
# `holocron` stays the OUTPUT_NAME either way, so the artifact is libholocron.so
# and HolocronActivity's getLibraries() names "holocron".
#
# The executable still builds for Android and is still useful -- `holocron-analyze`
# runs over adb shell with no Activity at all, which is how the analysis spine was
# confirmed on the Shield. It is only the PLAYER that needs a window and therefore
# an Activity.
if(ANDROID)
    add_library(holocron SHARED
        player/main.cpp
        # SDL_main plus the JavaVM handoff. Its own translation unit so that
        # main.cpp does not have to include SDL. See android_entry.cpp.
        player/android_entry.cpp
    )
    set_target_properties(holocron PROPERTIES OUTPUT_NAME holocron)

    # android_entry.cpp is the one place in the player that includes SDL, and it
    # needs the headers directly rather than through holocron::render.
    target_include_directories(holocron SYSTEM PRIVATE
        $<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_link_libraries(holocron PRIVATE SDL3::SDL3 log)
else()
    add_executable(holocron
        player/main.cpp
    )
endif()

target_link_libraries(holocron
    PRIVATE
        holocron::contract
        # Named explicitly rather than relied on transitively: main.cpp calls
        # resolve_data_path directly, and android_entry.cpp calls
        # set_data_directory and set_java_vm.
        holocron::platform
        holocron::analysis
        holocron::decoder
        holocron::sdl_sink
        holocron::wasapi_sink
        holocron::render
        holocron::session
        holocron::config
        holocron::plex
        holocron::notices
        holocron::warnings
        # The decode thread is a std::thread; gcc/libstdc++ needs an explicit
        # pthread link for that and MSVC needs nothing.
        Threads::Threads
)

# The colophon prints the version on its first page, so the definition has to
# reach this target too. It was PRIVATE to holocron_plex, which announces it over
# GDM -- and the panel making the number visible on a projector is exactly the
# kind of drift #38 is about.
target_compile_definitions(holocron PRIVATE HOLOCRON_VERSION="${PROJECT_VERSION}")

set_target_properties(holocron PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    # Android puts a .so here instead, and it goes straight into the layout the
    # APK wants: lib/<abi>/. scripts/android-apk.sh zips this directory in.
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/${ANDROID_ABI}"
)
