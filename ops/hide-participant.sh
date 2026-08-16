#!/usr/bin/env bash
# hide-participant.sh <handle> [--show]
#
# Toggle a participant's visibility on the PUBLIC leaderboard, live, with no
# redeploy and no effect on any box. A hidden participant keeps full platform
# access (login, Run, Submit, their own results) — they simply do not appear on
# the board, and are not selected for the Phase III golden rejudge.
#
#   ops/hide-participant.sh alice          # hide alice
#   ops/hide-participant.sh alice --show   # un-hide alice
#
# Backed by participants.hidden, which the `leaderboard` view filters on
# (`AND p.hidden IS NOT TRUE`). Because leaderboard_from_db() and rejudge() both
# read that view, hiding takes effect on the very next board fetch.

set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/aws/lib.sh"

HANDLE="${1:?usage: hide-participant.sh <handle> [--show]}"
VAL=true; VERB=HIDDEN
if [[ "${2:-}" == "--show" ]]; then VAL=false; VERB=VISIBLE; fi

load_outputs
esc="$(printf '%s' "$HANDLE" | sed "s/'/''/g")"

node web "docker exec flashmatch-postgres-1 psql -U mebench -d mebench -c \
  \"UPDATE participants SET hidden = $VAL WHERE handle = '$esc' AND removed_at IS NULL;\""

echo "participant '$HANDLE' is now $VERB on the leaderboard."
