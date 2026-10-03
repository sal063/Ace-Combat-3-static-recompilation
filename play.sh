#!/bin/sh
# Usage: play.sh [1|2] [game options...]
# The original look: the software renderer, pixel for pixel.
AC3_PROG=build/ac3 exec "$(dirname "$0")/play_enhanced.sh" "$@"
