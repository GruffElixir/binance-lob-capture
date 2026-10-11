#!/usr/bin/env bash
# Runs the reviewer's credential check (credential-like words joined into one regular expression)
# over the tracked files. Only docs/, README.md and .gitignore may mention those words; anywhere
# else a match fails. The pattern is assembled at run time so this script never matches itself.
set -euo pipefail

pattern="api""_key|sec""ret|pass""word"
if git grep -n -I -i -E "$pattern" -- . ':!docs/**' ':!README.md' ':!.gitignore'; then
    echo "credential-like text found outside docs/, README.md and .gitignore" >&2
    exit 1
fi
echo "credential check: clean"
