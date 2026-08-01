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
