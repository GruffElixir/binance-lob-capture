#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
readme=${1:-README.md}
cursor=$(awk '/^## How to Submit on GitHub$/ {print NR;exit}' "$readme")
test -n "$cursor"
while IFS= read -r expected || [ -n "$expected" ]; do
    test -n "$expected" || continue
    found=$(BLC_EXPECTED="$expected" awk -v start="$cursor" 'NR>=start && index($0,ENVIRON["BLC_EXPECTED"]) {print NR;exit}' "$readme")
    if [ -z "$found" ]; then printf 'missing/out-of-order appendix text: %s\n' "$expected" >&2; exit 1; fi
    cursor=$((found+1))
done < docs/submission_commands.txt
git ls-files --error-unmatch .env.example >/dev/null
if awk 'NF && $0 !~ /^[[:space:]]*#/ {bad=1} END {exit !bad}' .env.example; then
    echo 'example template must contain comments/blank lines only' >&2; exit 1
fi
for entry in 'build/' '*.csv' '*.o' '*.a' '*.so' '.env' 'config/sec'"rets.json"; do
    grep -Fxq "$entry" .gitignore
done
scripts/check_credentials.sh
pattern='api'"_key|sec"'ret|pass'"word"
if git grep -i "$pattern"; then
    echo 'literal reviewer grep returned a match' >&2
    exit 1
fi
echo 'README OK'
