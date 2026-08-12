#!/usr/bin/env bash
# logs.sh — read logs off a node without remembering where each one lives.
#
#   ops/aws/logs.sh web api           # a compose service: api, web, caddy, postgres, redis
#   ops/aws/logs.sh web all
#   ops/aws/logs.sh pool worker       # every mebench-pool@N unit, interleaved
#   ops/aws/logs.sh bench worker
#   ops/aws/logs.sh bench tune        # the boot-time tuning unit
#   ops/aws/logs.sh <role> cloud-init # first-boot provisioning
#
#   -f    follow
#   -n N  lines of history (default 100)

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

FOLLOW=0
LINES=100
ARGS=()
while (( $# )); do
  case "$1" in
    -f|--follow) FOLLOW=1; shift ;;
    -n) LINES="$2"; shift 2 ;;
    -h|--help) sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's|^# \?||'; exit 0 ;;
    *) ARGS+=("$1"); shift ;;
  esac
done

ROLE="${ARGS[0]:-}"
WHAT="${ARGS[1]:-}"
[[ "$ROLE" =~ ^(web|pool|bench)$ ]] || { echo "usage: $0 <web|pool|bench> <what> [-f] [-n N]" >&2; exit 2; }
[[ -n "$WHAT" ]] || { echo "usage: $0 $ROLE <api|worker|cloud-init|tune|all>" >&2; exit 2; }

load_outputs

case "$WHAT" in
  cloud-init)
    cmd="sudo tail -n $LINES $( ((FOLLOW)) && echo -f ) /var/log/cloud-init-output.log"
    ;;
  worker)
    if [[ "$ROLE" == "bench" ]]; then
      cmd="sudo journalctl -u mebench-bench -n $LINES $( ((FOLLOW)) && echo -f ) --no-pager"
    else
      # -u accepts a glob, which keeps the boxes interleaved in real time rather
      # than making you pick one and miss the one that failed.
      cmd="sudo journalctl -u 'mebench-pool@*' -n $LINES $( ((FOLLOW)) && echo -f ) --no-pager"
    fi
    ;;
  tune)
    cmd="sudo journalctl -u mebench-tune -n $LINES --no-pager"
    ;;
  all)
    [[ "$ROLE" == "web" ]] \
      || die "'all' means all compose services, which only run on the web node"
    cmd="cd /opt/flashmatch && docker compose -f platform/compose.yaml logs --tail=$LINES $( ((FOLLOW)) && echo -f )"
    ;;
  api|web|caddy|postgres|redis)
    [[ "$ROLE" == "web" ]] || die "'$WHAT' is a compose service and only runs on the web node"
    cmd="cd /opt/flashmatch && docker compose -f platform/compose.yaml logs --tail=$LINES $( ((FOLLOW)) && echo -f ) $WHAT"
    ;;
  *)
    die "don't know how to read '$WHAT'. Try: api, web, caddy, postgres, redis, all, worker, tune, cloud-init"
    ;;
esac

if (( FOLLOW )); then
  node_tty "$ROLE" "$cmd"
else
  node "$ROLE" "$cmd"
fi
