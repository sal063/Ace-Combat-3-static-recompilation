#!/bin/sh
# Usage: play_enhanced.sh [1|2] [game options...]
# Boots from disc 1 (or 2) with both discs mounted, using the enhanced renderer.
cd "$(dirname "$0")" || exit 1
prog=${AC3_PROG:-build/ac3_enhanced}
disc=1
case $1 in 1|2) disc=$1; shift ;; esac
exec "$prog" --boot --window --frames 0 \
    --disc "game/disc/Ace Combat 3 - Electrosphere (Japan, Asia) (Disc $disc) (Rev 1).bin" "$@"
