#!/usr/bin/env bash
# usage: c.sh "commit message"  -- format all C++ (tracked + new), build debug, commit
set -euo pipefail
CF=/c/Users/samar/AppData/Local/Programs/Python/Python314/Scripts/clang-format.exe
files=$(git ls-files --cached --others --exclude-standard '*.cpp' '*.hpp' '*.hpp.in' ':!:third_party/**')
$CF -i $files
$CF --dry-run --Werror $files
if cmake --build build/debug 2>&1 | grep -E " error|warning:|FAILED"; then echo "BUILD PROBLEM"; exit 1; fi
git add -A
git commit -q -m "$1"
git log --oneline -1
