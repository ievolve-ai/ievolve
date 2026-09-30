#!/usr/bin/env bash
# Runs circle packing using YAML as the only runtime configuration source.
#
#   ./run_example.sh
#   ./run_example.sh --config path/to/config.yaml
#
# Set max_iterations: 0 in YAML to evaluate only the seed without model calls.
# IEVOLVE_BUILD_DIR selects the build directory (default: ./build).

set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${IEVOLVE_BUILD_DIR:-$repo/build}"
args=("$@")
if [[ $# -eq 0 ]]; then
  args=(--config "$repo/examples/circle_packing/config.yaml")
fi

# Build only when the binary is missing; existing builds remain explicit.
binary="$build_dir/ievolve/bin/ievolve"
[[ -x "$binary" ]] || binary="$build_dir/bin/ievolve"
if [[ ! -x "$binary" ]]; then
  echo "==> No binary under $build_dir; configuring and building (first run only)"
  cmake -S "$repo" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$build_dir" -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
  binary="$build_dir/ievolve/bin/ievolve"
  [[ -x "$binary" ]] || binary="$build_dir/bin/ievolve"
fi

exec "$binary" "${args[@]}"
