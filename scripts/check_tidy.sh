#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# The Ubuntu 22.04 run-clang-tidy wrapper cannot forward warnings-as-errors; call the
# Clang tool directly, using its compile database and application-only paths.
git -c safe.directory="$PWD" ls-files -z 'src/*.cpp' | \
  xargs -0 -r -n 1 -P "${BLC_TIDY_JOBS:-2}" clang-tidy-15 -p "${BLC_TIDY_BUILD:-build/tidy}" \
    -header-filter='(^|/)src/(blc/.*|main.cpp)$' -warnings-as-errors='*'
