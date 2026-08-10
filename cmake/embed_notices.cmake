# SPDX-License-Identifier: GPL-3.0-or-later
#
# Turn THIRD-PARTY-NOTICES.md into a C++ source file holding its bytes.
#
# WHY THE NOTICES ARE EMBEDDED RATHER THAN READ FROM DISK.
#
# The about panel exists to discharge a licence obligation -- LGPL-2.1 section 6
# requires the shipped libraries' copyright notices to be displayed once the
# program displays its own. An obligation that is only met when a file happens to
# be beside the executable, and when the working directory happens to be right,
# is not met. Embedding makes the text a property of the binary: there is no
# path to get wrong, no install layout to depend on, and nothing different about
# the NVIDIA Shield at M8.
#
# AS A HEX BYTE ARRAY, NOT A STRING LITERAL, and that is not fussiness.
#
# The file is Markdown written by hand. It contains backticks, quotes, brackets
# and parentheses freely, so a raw string literal needs a delimiter no substring
# collides with -- provable for today's bytes and not for tomorrow's. It also
# contains non-ASCII (a u-umlaut in libebur128's copyright line), and CLAUDE.md
# records that this project's encoding trap has already bitten twice. A generated
# file of pure-ASCII hex escapes cannot be mangled by any of that, and cannot be
# broken by a character somebody types into the notices next year.
#
# Run as a script: cmake -DINPUT=... -DOUTPUT=... -DTEMPLATE=... -P embed_notices.cmake

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED TEMPLATE)
    message(FATAL_ERROR "embed_notices: INPUT, OUTPUT and TEMPLATE are all required")
endif()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)

if(hex_length EQUAL 0)
    # An empty notices file would compile and would silently discharge nothing.
    message(FATAL_ERROR "embed_notices: ${INPUT} is empty")
endif()

math(EXPR byte_count "${hex_length} / 2")

set(body "")
set(offset 0)
set(column 0)
while(offset LESS hex_length)
    string(SUBSTRING "${hex}" ${offset} 2 byte)
    string(APPEND body "0x${byte},")
    math(EXPR column "${column} + 1")
    if(column EQUAL 16)
        string(APPEND body "\n    ")
        set(column 0)
    endif()
    math(EXPR offset "${offset} + 2")
endwhile()

set(NOTICES_BYTES "${body}")
set(NOTICES_COUNT "${byte_count}")
set(NOTICES_SOURCE "${INPUT}")

configure_file("${TEMPLATE}" "${OUTPUT}" @ONLY)
