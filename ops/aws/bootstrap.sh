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

# --------------------------------------------------- keep the modules aligned
#
# bootstrap and ../ are separate root modules with separate variables, and
# nothing in Terraform makes them agree. Three values MUST match, and each fails
# in a way that does not mention the mismatch:
#
#   aws_region   the bucket is created here, and the deployer policy's
#                aws:RequestedRegion condition is templated from it. Bootstrap
#                in us-east-1 while ../ deploys to ap-south-1 and the first call
#                under that role fails UnauthorizedOperation — plus every
#                submission then crosses regions to reach S3.
#   aws_profile  a profile name that does not exist on this machine is
#                "failed to get shared config profile", before anything runs.
#   name_prefix  the policy conditions destructive actions on
#                Project = <name_prefix>; a mismatch is AccessDenied later.
#
# So resolve them from ../terraform.tfvars when it exists — which resolves
# defaults properly rather than grepping — and fall back to the AWS CLI's own
# configuration on a first run, before that file has been written.

TFVARS="$INFRA_DIR/terraform.tfvars"
BOOTSTRAP_VARS=()

infra_var() {
  [[ -f "$TFVARS" ]] || return 1
  local out
  out="$(printf 'var.%s\n' "$1" | terraform -chdir="$INFRA_DIR" console 2>/dev/null)" || return 1
  out="$(printf '%s' "$out" | tr -d '"')"
  [[ -z "$out" || "$out" == "null" ]] && return 1
  printf '%s' "$out"
}

if [[ -f "$TFVARS" ]]; then
  terraform -chdir="$INFRA_DIR" init -backend=false -input=false >/dev/null 2>&1 || true
fi

REGION="$(infra_var aws_region || true)"
[[ -z "$REGION" ]] && REGION="$(aws configure get region 2>/dev/null || true)"
[[ -n "$REGION" ]] || die "no region: set aws_region in infra/terraform.tfvars, or run 'aws configure set region <region>'"
BOOTSTRAP_VARS+=(-var "aws_region=$REGION")

PROFILE="$(infra_var aws_profile || true)"
[[ -z "$PROFILE" && -n "${AWS_PROFILE:-}" ]] && PROFILE="$AWS_PROFILE"
[[ -n "$PROFILE" ]] && BOOTSTRAP_VARS+=(-var "aws_profile=$PROFILE")

PREFIX="$(infra_var name_prefix || true)"
[[ -n "$PREFIX" ]] && BOOTSTRAP_VARS+=(-var "name_prefix=$PREFIX")

step "settings (taken from infra/terraform.tfvars where it exists)"
ok "region      $REGION"
ok "profile     ${PROFILE:-<default credential chain>}"
ok "name_prefix ${PREFIX:-flashmatch (module default)}"

step "initialising infra/bootstrap"
tf "$BOOTSTRAP_DIR" init -input=false
tf "$BOOTSTRAP_DIR" validate

step "planning"
# A non-empty plan on an existing deployment means somebody changed IAM or the
# bucket underneath us; show it rather than applying it silently.
if tf "$BOOTSTRAP_DIR" plan -input=false "${BOOTSTRAP_VARS[@]}" -detailed-exitcode -out=/tmp/bootstrap.tfplan >/dev/null 2>&1; then
  ok "nothing to do — bootstrap resources already match the configuration"
else
  rc=$?
  if (( rc == 1 )); then
    tf "$BOOTSTRAP_DIR" plan -input=false "${BOOTSTRAP_VARS[@]}"   # re-run visibly to surface the error
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
