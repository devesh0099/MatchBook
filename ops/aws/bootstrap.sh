#!/usr/bin/env bash
# bootstrap.sh — run once per account, before deploy.sh.
#
# Creates the artifact bucket, the instance role every node carries, and the
# scoped deployer role. These are the only resources in this project that need
# IAM write, which is why they are separate from the rest: the daily
# create/destroy cycle then runs without administrative permissions.
#
#   ops/aws/bootstrap.sh [--yes]
#
# Idempotent. Running it again on an existing deployment is a no-op that
# re-prints the values ../terraform.tfvars needs.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

ASSUME_YES=0
for a in "$@"; do
  case "$a" in
    --yes|-y) ASSUME_YES=1 ;;
    -h|--help) sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's|^# \?||'; exit 0 ;;
    *) die "unknown argument: $a" ;;
  esac
done

need_cmd terraform; need_cmd aws; need_cmd jq

step "initialising infra/bootstrap"
tf "$BOOTSTRAP_DIR" init -input=false
tf "$BOOTSTRAP_DIR" validate

step "planning"
# A non-empty plan on an existing deployment means somebody changed IAM or the
# bucket underneath us; show it rather than applying it silently.
if tf "$BOOTSTRAP_DIR" plan -input=false -detailed-exitcode -out=/tmp/bootstrap.tfplan >/dev/null 2>&1; then
  ok "nothing to do — bootstrap resources already match the configuration"
else
  rc=$?
  if (( rc == 1 )); then
    tf "$BOOTSTRAP_DIR" plan -input=false   # re-run visibly to surface the error
    die "bootstrap plan failed"
  fi
  tf "$BOOTSTRAP_DIR" show /tmp/bootstrap.tfplan
  echo >&2
  confirm "Apply this bootstrap plan? It creates IAM roles and an S3 bucket." \
    || die "aborted; nothing was created"
  step "applying"
  tf "$BOOTSTRAP_DIR" apply -input=false /tmp/bootstrap.tfplan
fi
rm -f /tmp/bootstrap.tfplan

# --------------------------------------------------------- wire up infra/

BUCKET="$(tf "$BOOTSTRAP_DIR" output -raw s3_bucket)"
PROFILE_NAME="$(tf "$BOOTSTRAP_DIR" output -raw instance_profile)"

step "artifact bucket"
ok "$BUCKET"
log "the running platform's ENTIRE AWS surface is read/write on this bucket"

TFVARS="$INFRA_DIR/terraform.tfvars"
if [[ ! -f "$TFVARS" ]]; then
  step "creating infra/terraform.tfvars"
  cp "$INFRA_DIR/terraform.tfvars.example" "$TFVARS"
  # Substitute what we now know, so the human only fills in the network bits.
  sed -i \
    -e "s|^instance_profile .*|instance_profile  = \"$PROFILE_NAME\"|" \
    -e "s|^s3_bucket .*|s3_bucket         = \"$BUCKET\"|" \
    "$TFVARS"
  ok "wrote $TFVARS with the bucket and instance profile filled in"
  warn "still needed: vpc_id, subnet_id, ami_id — then run ops/aws/preflight.sh"
else
  step "infra/terraform.tfvars already exists — leaving it alone"
  log "confirm it carries these values:"
  tf "$BOOTSTRAP_DIR" output -raw tfvars_snippet
fi

echo >&2
printf '%sbootstrap complete.%s Next: fill in infra/terraform.tfvars, then ops/aws/preflight.sh\n\n' \
  "$C_GREEN$C_BOLD" "$C_RESET" >&2
