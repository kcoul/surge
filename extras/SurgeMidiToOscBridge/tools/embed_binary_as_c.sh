#!/usr/bin/env bash
# Embeds a binary file as a C source/header pair.
# Produces: extern const unsigned char <symbol>[];
#           extern const size_t <symbol>_size;
#
# Usage: embed_binary_as_c.sh <input> <output.h> <output.cpp> <symbol_name>

set -euo pipefail

if [ "$#" -ne 4 ]; then
    echo "Usage: $(basename "$0") <input_file> <output.h> <output.cpp> <symbol_name>" >&2
    exit 1
fi

INPUT="$1"
OUTPUT_H="$2"
OUTPUT_CPP="$3"
SYMBOL="$4"
HEADER_BASENAME="$(basename "$OUTPUT_H")"

if [ ! -f "$INPUT" ]; then
    echo "Error: input file '$INPUT' not found" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT_H")" "$(dirname "$OUTPUT_CPP")"

cat > "$OUTPUT_H" << EOF
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned char ${SYMBOL}[];
extern const size_t ${SYMBOL}_size;

#ifdef __cplusplus
}
#endif
EOF

python3 << PYEOF > "$OUTPUT_CPP"
import sys

input_path  = "$INPUT"
symbol      = "$SYMBOL"
header_name = "$HEADER_BASENAME"

with open(input_path, 'rb') as f:
    data = f.read()

print('#include <stddef.h>')
print()
print('#include "' + header_name + '"')
print()
print('const unsigned char ' + symbol + '[] = {')

n = len(data)
for i in range(0, n, 12):
    chunk = data[i:i+12]
    hex_bytes = ', '.join('0x{:02X}'.format(b) for b in chunk)
    comma = ',' if i + 12 < n else ''
    print('    ' + hex_bytes + comma)

print('};')
print()
print('const size_t ' + symbol + '_size = sizeof(' + symbol + ');')
PYEOF

echo "Embedded '$(basename "$INPUT")' ($(wc -c < "$INPUT") bytes) as '$SYMBOL'"
echo "  -> $OUTPUT_H"
echo "  -> $OUTPUT_CPP"
