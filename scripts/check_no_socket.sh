#!/usr/bin/env bash
# Fails if an `strace -f -e trace=socket` log shows any socket() call (replay must not touch the
# network, design §7).
#
#   scripts/check_no_socket.sh <strace.log>
set -euo pipefail

if [ $# -ne 1 ] || [ ! -f "$1" ]; then
    echo "usage: $0 <strace.log>" >&2
    exit 2
fi
if grep -q 'socket(' "$1"; then
    echo 'unexpected network call during replay' >&2
    exit 1
fi
echo "no socket() calls"
