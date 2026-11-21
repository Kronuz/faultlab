#!/bin/bash
# Rewrites the MemoryIO fixture from journal's live MemoryIO: regenerate_memory_io.sh [OUT]
# (default test/fixtures/memory_io_states.txt). Fetches journal, so it needs the network.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$root/test/fixtures/memory_io_states.txt}
build=${FAULTLAB_REGENERATE_BUILD:-$root/.scratch/regenerate-build}
cmake -S "$root" -B "$build" -DFAULTLAB_REGENERATE_MEMORY_IO=ON >/dev/null
cmake --build "$build" --target faultlab_memory_io_gen -j4 >/dev/null
"$build/faultlab_memory_io_gen" "$out"
