#!/usr/bin/env bash
# tunnel.sh — forward the operator API to localhost:8081.
#
#   ops/aws/tunnel.sh          # blocks; Ctrl-C to close
#
# Then, in another shell:
#
#   curl -s  localhost:8081/admin/queue | jq
#   curl -s  localhost:8081/admin/discards | jq      # earliest warning of a sick bench node
#   curl -XPOST localhost:8081/admin/freeze
#   curl -XPOST localhost:8081/admin/bench/unhealthy
#   curl -XPOST localhost:8081/admin/bench/healthy
#
# There is no token and no password on any of these routes. Reaching the admin
# API IS ssh access to the web node — that is the entire auth story, and it is
# why nothing should ever proxy /admin.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

PORT="${1:-8081}"

load_outputs
[[ -n "$WEB_IP" ]] || die "the web node has no public IP"

# Fail early and clearly rather than after the tunnel is up: a port already in
# use makes ssh print one line and exit, which reads like an SSH problem.
if timeout 2 bash -c "</dev/tcp/127.0.0.1/$PORT" 2>/dev/null; then
  die "localhost:$PORT is already in use — another tunnel is probably open.
    Close it, or pass a different local port: $0 <port>"
fi

mapfile -t opts < <(ssh_opts web)

cat >&2 <<EOF

  forwarding localhost:$PORT → 127.0.0.1:8081 on the web node ($WEB_IP)

    curl -s localhost:$PORT/admin/queue | jq

  Ctrl-C to close.

EOF

exec ssh -N -L "$PORT:127.0.0.1:8081" "${opts[@]}" "ubuntu@$WEB_IP"
