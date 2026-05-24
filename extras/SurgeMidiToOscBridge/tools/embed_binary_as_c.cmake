# embed_binary_as_c.cmake
# Embeds a binary file as a C source/header pair.
# Produces: extern const unsigned char <symbol>[];
#           extern const size_t <symbol>_size;
#
# Invoke with:
#   cmake -DINPUT=<file> -DOUTPUT_H=<out.h> -DOUTPUT_CPP=<out.cpp>
#         -DSYMBOL=<symbol_name> -P embed_binary_as_c.cmake

foreach(_var INPUT OUTPUT_H OUTPUT_CPP SYMBOL)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "Required variable ${_var} not defined")
    endif()
endforeach()

if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "Input file '${INPUT}' not found")
endif()

get_filename_component(_h_dir "${OUTPUT_H}" DIRECTORY)
get_filename_component(_cpp_dir "${OUTPUT_CPP}" DIRECTORY)
file(MAKE_DIRECTORY "${_h_dir}" "${_cpp_dir}")

get_filename_component(HEADER_BASENAME "${OUTPUT_H}" NAME)

# --- Write header -----------------------------------------------------------
file(WRITE "${OUTPUT_H}"
"#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern \"C\" {
#endif

extern const unsigned char ${SYMBOL}[];
extern const size_t ${SYMBOL}_size;

#ifdef __cplusplus
}
#endif
")

# --- Write cpp ---------------------------------------------------------------
file(READ "${INPUT}" _raw HEX)
string(LENGTH "${_raw}" _hex_len)
math(EXPR _num_bytes "${_hex_len} / 2")

# Build comma-separated hex literals, 12 bytes per line.
set(_body "")
set(_i 0)
while(_i LESS _hex_len)
    # Determine how many hex chars remain for this line (up to 24 = 12 bytes).
    math(EXPR _remaining "${_hex_len} - ${_i}")
    if(_remaining GREATER 24)
        set(_chunk_len 24)
    else()
        set(_chunk_len ${_remaining})
    endif()

    string(SUBSTRING "${_raw}" ${_i} ${_chunk_len} _chunk)

    # Convert pairs of hex chars to "0xAB" literals.
    set(_line "")
    set(_j 0)
    while(_j LESS _chunk_len)
        string(SUBSTRING "${_chunk}" ${_j} 2 _byte)
        string(TOUPPER "${_byte}" _byte)
        if(_line STREQUAL "")
            set(_line "0x${_byte}")
        else()
            string(APPEND _line ", 0x${_byte}")
        endif()
        math(EXPR _j "${_j} + 2")
    endwhile()

    math(EXPR _next "${_i} + ${_chunk_len}")
    if(_next LESS _hex_len)
        string(APPEND _line ",")
    endif()

    string(APPEND _body "    ${_line}\n")
    set(_i ${_next})
endwhile()

file(WRITE "${OUTPUT_CPP}"
"#include <stddef.h>

#include \"${HEADER_BASENAME}\"

const unsigned char ${SYMBOL}[] = {
${_body}};

const size_t ${SYMBOL}_size = sizeof(${SYMBOL});
")

file(SIZE "${INPUT}" _file_size)
message(STATUS "Embedded '${INPUT}' (${_file_size} bytes) as '${SYMBOL}'")
message(STATUS "  -> ${OUTPUT_H}")
message(STATUS "  -> ${OUTPUT_CPP}")
