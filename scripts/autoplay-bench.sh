#!/usr/bin/env bash
# Build and run the Auto Play bench on the host (no Android SDK needed).
# Any arguments are passed to the bench, e.g.
#   scripts/autoplay-bench.sh --runs 100 --levels --campaigns
#   scripts/autoplay-bench.sh --runs 50 --campaigns --trace   # dump every lost ball
# See app/src/main/cpp/test/autoplay_bench.cpp for the options.
set -euo pipefail
cd "$(dirname "$0")/.."
cpp=app/src/main/cpp
out=build/autoplay-bench
mkdir -p "$out"
cxx="${CXX:-c++}"
"$cxx" -std=c++17 -O2 -Wall -Wextra -I"$cpp" -I"$cpp/test/host" \
    "$cpp/test/autoplay_bench.cpp" "$cpp/game.cpp" "$cpp/test/audio_stub.cpp" \
    -o "$out/autoplay_bench"
exec "$out/autoplay_bench" "$@"
