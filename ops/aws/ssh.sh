#!/usr/bin/env bash
# ssh.sh — connect to a node without looking up an IP.
#
#   ops/aws/ssh.sh web
#   ops/aws/ssh.sh bench 'systemctl status mebench-bench'
#
# With no command it drops you into a shell in /opt/flashmatch.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

ROLE="${1:-}"
[[ "$ROLE" =~ ^(web|pool|bench)$ ]] || {
  echo "usage: $0 <web|pool|bench> [command...]" >&2; exit 2; }
shift

load_outputs
ip="$(_ip_for_role "$ROLE")"
[[ -n "$ip" ]] || die "the $ROLE node has no public IP (see associate_public_ip)"

mapfile -t opts < <(ssh_opts "$ROLE")
if (( $# )); then
  exec ssh "${opts[@]}" "ubuntu@$ip" "$@"
else
  exec ssh -t "${opts[@]}" "ubuntu@$ip" 'cd /opt/flashmatch 2>/dev/null; exec bash -l'
fi
