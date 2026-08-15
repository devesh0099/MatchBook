#!/usr/bin/env bash
# Issue login credentials for every handle in a roster file, one per line
# ('#' comments allowed), and print the slips to cut up and hand out at
# check-in. Runs against the OPERATOR listener — reach it over the SSH tunnel:
#
#   ssh -L 8081:127.0.0.1:8081 web-node
#   ops/issue-credentials.sh roster.txt | tee slips.txt
#
# Re-running for an existing handle RESETS their password and signs out their
# sessions — that is also the "student forgot their password" procedure.
set -euo pipefail

ROSTER=${1:?usage: issue-credentials.sh ROSTER_FILE [ADMIN_URL]}
ADMIN=${2:-http://127.0.0.1:8081}

while IFS= read -r handle; do
  handle=$(echo "$handle" | tr -d '[:space:]')
  [ -z "$handle" ] && continue
  case "$handle" in '#'*) continue ;; esac
  curl -sf -X POST "$ADMIN/admin/participants" \
       -H 'content-type: application/json' \
       -d "{\"handle\":\"$handle\"}" |
    python3 -c "
import json,sys
d=json.load(sys.stdin)
print('-----------------------------------------')
print('  FlashMatch login')
print('  handle:   %s' % d['handle'])
print('  password: %s' % d['password'])
print('-----------------------------------------')"
done < "$ROSTER"
