#!/usr/bin/env bash
# preflight.sh — everything that can be checked before spending money.
#
# Each check here exists because getting it wrong is expensive, silent, or both:
# a mismatched key pair produces three healthy instances nobody can log into, and
# a vCPU quota of 5 on a fresh account rejects the third instance after the first
# two have already launched.
#
#   ops/aws/preflight.sh
#
# Exits non-zero if anything would block a deploy. Nothing here creates or
# changes an AWS resource.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

TFVARS="$INFRA_DIR/terraform.tfvars"

# ------------------------------------------------------------------ tooling

step "local tooling"

need_cmd terraform "https://developer.hashicorp.com/terraform/install"
need_cmd aws "https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html"
need_cmd jq
need_cmd ssh
need_cmd openssl

tf_version="$(terraform version -json | jq -r .terraform_version)"
if [[ "$(printf '%s\n1.5.0\n' "$tf_version" | sort -V | head -1)" != "1.5.0" ]]; then
  check_fail "terraform $tf_version is older than the required 1.5"
else
  ok "terraform $tf_version"
fi
ok "aws-cli $(aws --version 2>&1 | sed 's|aws-cli/||;s| .*||')"

# ------------------------------------------------------------------- tfvars

step "configuration"

[[ -f "$TFVARS" ]] || die "infra/terraform.tfvars does not exist.
    cp infra/terraform.tfvars.example infra/terraform.tfvars, then fill it in."

# Resolve through Terraform rather than grepping the file, so that variables
# left at their defaults in variables.tf resolve to the value that will actually
# be used rather than to nothing.
terraform -chdir="$INFRA_DIR" init -backend=false -input=false >/dev/null 2>&1 \
  || die "terraform init failed in infra/ — run it directly to see why"

# Evaluate any expression in the infra module — var.*, local.*, whatever.
#
# An unset optional variable prints as `tostring(null)` rather than `null`,
# which is easy to mistake for a value: it sailed through a plain `grep -v
# '^null$'` and became the literal instance profile name "tostring(null)", and
# would have been exported as AWS_PROFILE too. Both spellings are filtered here.
tfexpr() {
  local out
  out="$(printf '%s\n' "$1" | terraform -chdir="$INFRA_DIR" console 2>/dev/null)" || return 1
  out="$(printf '%s' "$out" | tr -d '"')"
  [[ "$out" == "null" || "$out" == "tostring(null)" ]] && return 1
  printf '%s' "$out"
}

var() { tfexpr "var.$1" || true; }

REGION="$(var aws_region)"
PROFILE="$(var aws_profile)"
VPC_ID="$(var vpc_id)"
SUBNET_ID="$(var subnet_id)"
AMI_ID="$(var ami_id)"
KEY_NAME="$(tfexpr local.key_name || true)"
PUBKEY_PATH="$(var public_key_path)"
PUBKEY_PATH="${PUBKEY_PATH/#\~/$HOME}"
BUCKET="$(var s3_bucket)"
INSTANCE_PROFILE="$(tfexpr local.instance_profile || true)"
WEB_TYPE="$(var web_instance_type)"
POOL_TYPE="$(var pool_instance_type)"
BENCH_TYPE="$(var bench_instance_type)"
BENCH_CORES="$(var bench_core_count)"

for pair in "aws_region:$REGION" "vpc_id:$VPC_ID" "subnet_id:$SUBNET_ID" \
            "ami_id:$AMI_ID" "s3_bucket:$BUCKET"; do
  [[ -n "${pair#*:}" ]] || check_fail "${pair%%:*} is not set in infra/terraform.tfvars"
done
(( FAILURES )) && die "fill in infra/terraform.tfvars before continuing"

ok "region $REGION, profile ${PROFILE:-<default>}"

export AWS_REGION="$REGION" AWS_DEFAULT_REGION="$REGION"
[[ -n "$PROFILE" ]] && export AWS_PROFILE="$PROFILE"

# -------------------------------------------------------------- credentials

step "AWS credentials"

if ! caller="$(aws sts get-caller-identity --output json 2>&1)"; then
  die "AWS credentials are not usable for profile '${PROFILE:-default}':
    ${caller}
    Fix with: aws configure --profile ${PROFILE:-default}   (or aws sso login)"
fi
ACCOUNT_ID="$(jq -r .Account <<<"$caller")"
ok "account $ACCOUNT_ID as $(jq -r .Arn <<<"$caller")"

# ---------------------------------------------------------------- ssh key
# The check DEPLOYMENT.md 1 asks for, done automatically. A private key whose
# public half is not what Terraform imports means three instances that launch,
# pass every health check, and refuse every login.

step "SSH key"

PRIVKEY_PATH="${PUBKEY_PATH%.pub}"

# Type and material only. ssh-keygen -y emits no trailing comment while the .pub
# on disk usually carries one, and that difference is harmless — comparing the
# whole line would report a mismatch on a perfectly good pair.
pubkey_material() { cut -d' ' -f1,2 "$1"; }

if [[ ! -f "$PUBKEY_PATH" ]]; then
  check_fail "public key $PUBKEY_PATH does not exist
      ssh-keygen -t ed25519 -f ${PRIVKEY_PATH} -C flashmatch"
elif [[ ! -f "$PRIVKEY_PATH" ]]; then
  check_fail "private key $PRIVKEY_PATH does not exist — you would not be able to log in"
elif ! derived="$(ssh-keygen -y -f "$PRIVKEY_PATH" </dev/null 2>/dev/null)"; then
  # </dev/null so an encrypted key fails here instead of silently waiting for a
  # passphrase nobody is watching for.
  warn "could not read $PRIVKEY_PATH without a passphrase — cannot verify the halves match"
  warn "check by hand: diff <(ssh-keygen -y -f $PRIVKEY_PATH) $PUBKEY_PATH"
elif [[ "$(cut -d' ' -f1,2 <<<"$derived")" == "$(pubkey_material "$PUBKEY_PATH")" ]]; then
  ok "key pair halves match ($PRIVKEY_PATH)"
else
  check_fail "KEY PAIR MISMATCH: $PRIVKEY_PATH is not the private half of $PUBKEY_PATH.
      Launching now gives you three instances you cannot log into, and the only
      recovery is detaching the root volume and mounting it elsewhere."
fi

# An existing EC2 key pair of the same name with different material is imported
# over, or collides — either way it is better found now.
if existing="$(aws ec2 describe-key-pairs --key-names "$KEY_NAME" \
                --query 'KeyPairs[0].KeyPairId' --output text 2>/dev/null)"; then
  warn "an EC2 key pair named '$KEY_NAME' already exists ($existing)"
  warn "terraform apply will fail with InvalidKeyPair.Duplicate unless it is in state"
fi

# -------------------------------------------------------------- networking

step "VPC and subnet"

if ! subnet="$(aws ec2 describe-subnets --subnet-ids "$SUBNET_ID" --output json 2>/dev/null)"; then
  check_fail "subnet $SUBNET_ID not found in $REGION"
else
  SUBNET_VPC="$(jq -r '.Subnets[0].VpcId' <<<"$subnet")"
  SUBNET_AZ="$(jq -r '.Subnets[0].AvailabilityZone' <<<"$subnet")"
  AUTO_PUBLIC="$(jq -r '.Subnets[0].MapPublicIpOnLaunch' <<<"$subnet")"

  if [[ "$SUBNET_VPC" != "$VPC_ID" ]]; then
    check_fail "subnet $SUBNET_ID belongs to $SUBNET_VPC, not to vpc_id=$VPC_ID"
  else
    ok "subnet $SUBNET_ID in $SUBNET_AZ"
  fi

  # This is the check that turns an empty `terraform output web_public_ip` into
  # a sentence. Without a public IP the SSH commands in outputs.tf are
  # "ssh -i key ubuntu@" and every provisioning step fails at the first hop.
  if [[ "$AUTO_PUBLIC" != "true" ]]; then
    warn "subnet does NOT auto-assign public IPs (MapPublicIpOnLaunch=false)"
    warn "set associate_public_ip = true in terraform.tfvars, or the nodes will be unreachable"
  else
    ok "subnet auto-assigns public IPs"
  fi

  # A subnet with no default route reaches neither apt nor GitHub, so cloud-init
  # gets partway and the ready marker never appears.
  rt="$(aws ec2 describe-route-tables \
        --filters "Name=association.subnet-id,Values=$SUBNET_ID" --output json 2>/dev/null)"
  [[ "$(jq '.RouteTables | length' <<<"$rt")" == "0" ]] && \
    rt="$(aws ec2 describe-route-tables \
          --filters "Name=vpc-id,Values=$VPC_ID" "Name=association.main,Values=true" \
          --output json 2>/dev/null)"

  if jq -e '.RouteTables[].Routes[]
            | select(.DestinationCidrBlock=="0.0.0.0/0")
            | select(.GatewayId // .NatGatewayId // "" | test("^(igw|nat)-"))' \
       <<<"$rt" >/dev/null 2>&1; then
    ok "subnet has a default route to the internet"
  else
    check_fail "subnet $SUBNET_ID has no 0.0.0.0/0 route via an internet or NAT gateway.
      cloud-init pulls apt packages, rustup and GitHub — it will hang, and the
      ready marker will never appear."
  fi
fi

# --------------------------------------------------------------------- AMI

step "AMI"

if ! ami="$(aws ec2 describe-images --image-ids "$AMI_ID" --output json 2>/dev/null)" \
   || [[ "$(jq '.Images | length' <<<"$ami")" == "0" ]]; then
  check_fail "AMI $AMI_ID not found in $REGION (AMI ids are region-specific)"
else
  ARCH="$(jq -r '.Images[0].Architecture' <<<"$ami")"
  AMI_NAME="$(jq -r '.Images[0].Name' <<<"$ami")"
  if [[ "$ARCH" != "x86_64" ]]; then
    check_fail "AMI $AMI_ID is $ARCH. Everything compiles -march=x86-64-v3; arm64 will not build."
  else
    ok "$AMI_NAME ($ARCH)"
  fi
  [[ "$AMI_NAME" == *"24.04"* ]] || warn "AMI name does not mention 24.04 — the setup scripts target Ubuntu 24.04 LTS"
fi

# ------------------------------------------------------- instance types & quota

step "instance types and quota"

TOTAL_VCPU=0
for t in "$WEB_TYPE" "$POOL_TYPE" "$BENCH_TYPE"; do
  if ! it="$(aws ec2 describe-instance-types --instance-types "$t" --output json 2>/dev/null)"; then
    check_fail "instance type $t is not available in $REGION"
    continue
  fi
  v="$(jq -r '.InstanceTypes[0].VCpuInfo.DefaultVCpus' <<<"$it")"
  TOTAL_VCPU=$(( TOTAL_VCPU + v ))
done

# The bench node pins core_count, and RunInstances rejects a value the type does
# not support — after the other two instances have already been created.
if bench_it="$(aws ec2 describe-instance-types --instance-types "$BENCH_TYPE" --output json 2>/dev/null)"; then
  valid_cores="$(jq -r '.InstanceTypes[0].VCpuInfo.ValidCores[]?' <<<"$bench_it" | tr '\n' ' ')"
  if [[ -n "$valid_cores" ]] && ! grep -qw "$BENCH_CORES" <<<"$valid_cores"; then
    check_fail "bench_core_count=$BENCH_CORES is not valid for $BENCH_TYPE (valid: $valid_cores)"
  else
    ok "bench_core_count=$BENCH_CORES valid for $BENCH_TYPE, SMT off"
  fi
fi

# Standard on-demand vCPU quota. A brand-new account often has 5, which is not
# enough for one c6i.2xlarge, let alone three instances.
if q="$(aws service-quotas get-service-quota --service-code ec2 \
        --quota-code L-1216C47A --query 'Quota.Value' --output text 2>/dev/null)"; then
  q_int="${q%.*}"
  if (( TOTAL_VCPU > q_int )); then
    check_fail "this deployment needs $TOTAL_VCPU vCPUs; the account's on-demand quota in $REGION is $q_int.
      Request an increase for 'Running On-Demand Standard instances' (L-1216C47A) — it is not instant."
  else
    ok "$TOTAL_VCPU vCPUs needed, quota is $q_int"
  fi
else
  warn "could not read the vCPU quota (needs servicequotas:GetServiceQuota); this deployment needs $TOTAL_VCPU"
fi

# ------------------------------------------------------------ bootstrap deps

step "bootstrap resources"

if aws s3api head-bucket --bucket "$BUCKET" >/dev/null 2>&1; then
  ok "artifact bucket $BUCKET exists"
else
  check_fail "artifact bucket '$BUCKET' does not exist or is not readable.
      Run ops/aws/bootstrap.sh first — without it every /run and /submit returns
      an opaque internal error, because the API stores source in S3 first."
fi

if aws iam get-instance-profile --instance-profile-name "$INSTANCE_PROFILE" >/dev/null 2>&1; then
  ok "instance profile $INSTANCE_PROFILE exists"
else
  check_fail "instance profile '$INSTANCE_PROFILE' does not exist. Run ops/aws/bootstrap.sh."
fi

# ------------------------------------------------------------------ verdict

echo >&2
if (( FAILURES )); then
  die "$FAILURES check(s) failed. Nothing has been created."
fi
printf '%spreflight passed — safe to run ops/aws/deploy.sh%s\n\n' "$C_GREEN$C_BOLD" "$C_RESET" >&2
