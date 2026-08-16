#!/usr/bin/env bash
# provision-web-m8.sh — wire the on-demand box-provisioner (M8) onto a freshly
# deployed web node, reproducibly.
#
# cloud-init brings up the base web stack (repo clone + docker compose), but the
# M8 control plane — the AWS launch config, the sealed seeds, the fleet SSH key,
# and the box-provisioner systemd service — was historically hand-typed onto the
# web node and undocumented. This script IS that step, so "destroy web and
# redeploy" is a repeatable operation rather than tribal knowledge.
#
#   ops/aws/provision-web-m8.sh --ami ami-XXXX
#
# Everything the daemon launches boxes with is derived from Terraform outputs;
# the Postgres password comes from the shared secrets file; the sealed event
# seeds are generated ON the web node from /dev/urandom and never leave it.
#
# Optional env:
#   ADMIN_EMAIL, ADMIN_PASSWORD  — set operator credentials in the compose .env.
#                                  Omit to leave existing credentials untouched
#                                  (this script never rotates a password on its
#                                  own).
#   REGEN_SEEDS=1                — force-regenerate /opt/flashmatch/platform/
#                                  .seeds even if it already exists.

set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

AMI=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --ami) AMI="$2"; shift 2 ;;
    *) die "unknown argument: $1" ;;
  esac
done
[[ "$AMI" == ami-* ]] || die "pass the agent AMI: --ami ami-XXXX"

load_outputs
load_secrets   # exports POSTGRES_PASSWORD

REGION="$AWS_REGION_TF"
SUBNET="$(_out subnet_id)"
SG="$(_out agent_security_group_id)"
PROFILE="$(_out instance_profile)"
KEYNAME="$(_out key_name)"
ITYPE="$(_out agent_instance_type)"
CORES="$(_out agent_core_count)"
S3="$S3_BUCKET_TF"
for v in REGION SUBNET SG PROFILE KEYNAME ITYPE CORES S3 WEB_PRIVATE_IP; do
  [[ -n "${!v}" ]] || die "missing Terraform output for $v — run terraform apply first"
done

step "installing the AWS CLI on the web node (if absent)"
node web 'command -v aws >/dev/null 2>&1 || (
  curl -fsSL https://awscli.amazonaws.com/awscli-exe-linux-x86_64.zip -o /tmp/awscliv2.zip &&
  cd /tmp && unzip -q -o awscliv2.zip && sudo ./aws/install --update &&
  rm -rf /tmp/aws /tmp/awscliv2.zip )'
node web 'aws --version'

step "placing the fleet SSH key so the daemon can reach agent boxes"
# The daemon SSHes to each box's PRIVATE ip as ubuntu with this key; it is the
# same private key that matches the imported public key.
node_scp web "${SSH_IDENTITY/#\~/$HOME}" /tmp/fleet_key
node web 'mkdir -p /home/ubuntu/.ssh && mv /tmp/fleet_key /home/ubuntu/.ssh/fleet_key && chmod 600 /home/ubuntu/.ssh/fleet_key'

step "sealing event seeds (urandom, on the web node, never printed)"
# SEED1/SEED2 are injected into every agent box; SEED3 is golden-box-only and is
# read from here only when a rejudge starts. Generated once and kept 0600 — the
# whole point is that no one, including this script's output, ever sees them.
FORCE="${REGEN_SEEDS:-0}"
node web "bash -s" <<SEEDGEN
set -e
SEEDS=/opt/flashmatch/platform/.seeds
if [ ! -f "\$SEEDS" ] || [ "$FORCE" = 1 ]; then
  s() { printf '%u' "0x\$(head -c4 /dev/urandom | od -An -tx1 | tr -d ' ')"; }
  sudo mkdir -p /opt/flashmatch/platform
  printf 'MEBENCH_SEED1=%s\nMEBENCH_SEED2=%s\nMEBENCH_SEED3=%s\n' "\$(s)" "\$(s)" "\$(s)" | sudo tee "\$SEEDS" >/dev/null
  sudo chmod 600 "\$SEEDS"
  echo "sealed a fresh .seeds"
else
  echo ".seeds already present — kept (pass REGEN_SEEDS=1 to rotate)"
fi
SEEDGEN

step "writing /opt/flashmatch/provisioner.env (launch config for the daemon)"
node web "sudo tee /opt/flashmatch/provisioner.env >/dev/null <<ENVEOF
REGION=$REGION
AMI=$AMI
ITYPE=$ITYPE
CORES=$CORES
SUBNET=$SUBNET
SG=$SG
PROFILE=$PROFILE
KEYNAME=$KEYNAME
WEB_PRIVATE_IP=$WEB_PRIVATE_IP
S3_BUCKET=$S3
PGPASS=$POSTGRES_PASSWORD
ENVEOF
sudo chmod 600 /opt/flashmatch/provisioner.env"

if [[ -n "${ADMIN_PASSWORD:-}" ]]; then
  step "setting operator credentials in the compose .env"
  # docker-compose interpolates \$, so a literal \$ in the password must be
  # doubled in the .env (BUGS.md B: the ADMIN_PASSWORD \$\$ gotcha).
  esc_pw="$(printf '%s' "$ADMIN_PASSWORD" | sed 's/\$/\$\$/g')"
  admin_email="${ADMIN_EMAIL:-developer@iicpc.com}"
  node web "ENVF=/opt/flashmatch/platform/.env
    sudo touch \$ENVF
    sudo sed -i '/^ADMIN_EMAIL=/d;/^ADMIN_PASSWORD=/d' \$ENVF
    printf 'ADMIN_EMAIL=%s\nADMIN_PASSWORD=%s\n' '$admin_email' '$esc_pw' | sudo tee -a \$ENVF >/dev/null
    cd /opt/flashmatch/platform && sudo docker compose -f compose.yaml up -d api"
else
  log "ADMIN_PASSWORD not provided — leaving operator credentials untouched (no rotation)."
fi

step "installing and starting the box-provisioner service"
node web 'sudo cp /opt/flashmatch/ops/box-provisioner.service /etc/systemd/system/ &&
  sudo systemctl daemon-reload &&
  sudo systemctl enable box-provisioner &&
  sudo systemctl restart box-provisioner &&
  sleep 2 && systemctl is-active box-provisioner'

ok "web node wired for M8. Daemon launches boxes from $AMI; seeds sealed on the node."
