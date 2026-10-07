#!/bin/sh
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "Usage: build_evaluator.sh WORKSPACE [DEPENDENCY_PREFIX]" >&2
  exit 2
fi
script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository=$(CDPATH= cd -- "$script_directory/../.." && pwd)
workspace=$1
dependency_prefix=${2:-"$repository/build/_deps/install"}
if [ ! -d "$dependency_prefix" ]; then
  echo "Dependency prefix missing: $dependency_prefix. Build ievolve first or pass its dependency prefix." >&2
  exit 2
fi
cmake -S "$repository" -B "$workspace" -DIEVOLVE_SUPERBUILD=OFF \
  -DBUILD_TESTING=OFF -DIEVOLVE_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$dependency_prefix"
cmake --build "$workspace" --target ievolve_cli_sorting_evaluator -j 2
