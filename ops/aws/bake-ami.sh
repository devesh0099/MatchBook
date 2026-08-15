#!/usr/bin/env bash
# bake-ami.sh — snapshot a provisioned agent box into the fleet AMI.
#
#   ops/aws/bake-ami.sh [agent1]
#
# This is what makes "replacement in minutes" true (PLAN-measurement-redesign
# §7/M4): the slow provisioning path — toolchain, isolate, engine and worker
# builds, baked streams, kernel isolation — is paid ONCE on the source box,
# and every fleet launch afterwards boots the finished article. After baking:
#
#   1. put the printed id in infra/terraform.tfvars:  agent_ami_id = "ami-..."
#   2. terraform apply launches (and replaces) agents from it
#
# The image REBOOTS the source box during capture (filesystem-consistent
# snapshot); its agent comes back by itself via systemd. Bake after a code
# change, not during the event.
#
# What is deliberately NOT in the image: worker.env. The participant binding
# is written per box by deploy.sh's connect phase, which is what lets one
# image serve every participant. SEED3 artifacts are never on any agent box.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

ROLE="${1:-agent1}"
[[ "$ROLE" =~ ^agent[0-9]+$ ]] || die "usage: bake-ami.sh [agentN]"
N="${ROLE#agent}"

load_outputs
REGION="$AWS_REGION_TF"

INSTANCE_ID="$(aws ec2 describe-instances --region "$REGION" \
  --filters "Name=tag:Name,Values=flashmatch-agent-$N" "Name=instance-state-name,Values=running" \
  --query 'Reservations[0].Instances[0].InstanceId' --output text)"
[[ "$INSTANCE_ID" == i-* ]] || die "no running instance tagged flashmatch-agent-$N"

STAMP="$(date -u +%Y%m%d-%H%M)"
NAME="flashmatch-agent-$STAMP"

step "creating image $NAME from $ROLE ($INSTANCE_ID) — the box reboots during capture"
AMI_ID="$(aws ec2 create-image --region "$REGION" --instance-id "$INSTANCE_ID" \
  --name "$NAME" --description "flashmatch agent box, provisioned by ops/agent-node-setup.sh" \
  --tag-specifications "ResourceType=image,Tags=[{Key=Project,Value=flashmatch}]" \
                       "ResourceType=snapshot,Tags=[{Key=Project,Value=flashmatch}]" \
  --query 'ImageId' --output text)"
[[ "$AMI_ID" == ami-* ]] || die "create-image failed"
log "image $AMI_ID requested; waiting for it to become available (~5-10 min)"

aws ec2 wait image-available --region "$REGION" --image-ids "$AMI_ID" \
  || die "image $AMI_ID never became available"

ok "baked $AMI_ID"
cat >&2 <<EOF

Next:
  1. infra/terraform.tfvars:   agent_ami_id = "$AMI_ID"
  2. ops/aws/deploy.sh         (new boxes launch provisioned; a dead box is a
                                fresh launch + connect, minutes end to end)

Note: applying with agent_ami_id set REPLACES existing agents on their next
apply if their AMI differs — plan first, and do it between events, not during.
EOF
