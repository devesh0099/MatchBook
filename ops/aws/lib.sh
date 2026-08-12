#!/usr/bin/env bash
# Shared by every script in ops/aws/. Not executable on its own.
#
# The single source of truth for where things are is `terraform output`. Nothing
# here takes a hostname or an IP as an argument, because the failure mode that
# costs an event is a stale IP pasted from yesterday into a command that looks
# right.

# shellcheck shell=bash

set -euo pipefail

# ------------------------------------------------------------------- paths

_lib_dir() {
  local src="${BASH_SOURCE[0]}"
  while [[ -L "$src" ]]; do
    local dir
    dir="$(cd -P "$(dirname "$src")" && pwd)"
    src="$(readlink "$src")"
    [[ "$src" != /* ]] && src="$dir/$src"
  done
  cd -P "$(dirname "$src")" && pwd
}

AWS_DIR="$(_lib_dir)"
REPO_ROOT="$(cd "$AWS_DIR/../.." && pwd)"
INFRA_DIR="$REPO_ROOT/infra"
BOOTSTRAP_DIR="$INFRA_DIR/bootstrap"

# Deployment-local state: the generated Postgres password and the known_hosts
# file for instances whose IPs AWS will recycle. Both are gitignored.
STATE_DIR="$AWS_DIR/.state"
SECRETS_FILE="$STATE_DIR/secrets.env"
KNOWN_HOSTS="$STATE_DIR/known_hosts"

export REPO_ROOT INFRA_DIR BOOTSTRAP_DIR STATE_DIR SECRETS_FILE KNOWN_HOSTS

# ----------------------------------------------------------------- logging

if [[ -t 2 ]] && [[ "${NO_COLOR:-}" == "" ]]; then
  C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'
  C_RED=$'\033[31m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_BLUE=$'\033[34m'
else
  C_RESET=""; C_BOLD=""; C_DIM=""; C_RED=""; C_GREEN=""; C_YELLOW=""; C_BLUE=""
fi

step() { printf '\n%s==> %s%s\n' "$C_BOLD$C_BLUE" "$*" "$C_RESET" >&2; }
log()  { printf '    %s\n' "$*" >&2; }
ok()   { printf '    %s✓%s %s\n' "$C_GREEN" "$C_RESET" "$*" >&2; }
warn() { printf '    %s!%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
bad()  { printf '    %s✗%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; }
die()  { printf '\n%serror:%s %s\n' "$C_RED$C_BOLD" "$C_RESET" "$*" >&2; exit 1; }

# Used by the check scripts: record a failure but keep going, so one run tells
# you everything that is wrong rather than the first thing.
FAILURES=0
check_fail() { bad "$*"; FAILURES=$((FAILURES + 1)); }

confirm() {
  local prompt="$1"
  [[ "${ASSUME_YES:-0}" == "1" ]] && return 0
  [[ -t 0 ]] || die "$prompt (refusing to assume yes on a non-interactive run; pass --yes)"
  local reply
  read -r -p "$prompt [y/N] " reply
  [[ "$reply" == "y" || "$reply" == "Y" ]]
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "$1 is required but not on PATH${2:+ ($2)}"
}

# ---------------------------------------------------------------- terraform

tf() { terraform -chdir="$1" "${@:2}"; }

have_state() {
  local dir="$1"
  terraform -chdir="$dir" show -json >/dev/null 2>&1 || return 1
  [[ -n "$(terraform -chdir="$dir" state list 2>/dev/null)" ]]
}

# Terraform outputs are read ONCE per script and cached in the environment.
# Re-shelling out per value is slow enough to be noticeable across a dozen
# lookups, and worse, it can straddle a concurrent apply.
_TF_JSON=""

load_outputs() {
  [[ -n "$_TF_JSON" ]] && return 0

  [[ -d "$INFRA_DIR/.terraform" ]] || die "infra/ is not initialised. Run: ops/aws/deploy.sh, or terraform -chdir=infra init"

  _TF_JSON="$(terraform -chdir="$INFRA_DIR" output -json 2>/dev/null)" \
    || die "could not read terraform outputs from infra/ — has it been applied?"
  [[ "$_TF_JSON" == "{}" || -z "$_TF_JSON" ]] \
    && die "infra/ has no outputs — nothing is deployed. Run ops/aws/deploy.sh first."

  WEB_IP="$(_out web_public_ip)"
  WEB_PRIVATE_IP="$(_out web_private_ip)"
  POOL_IP="$(_out pool_private_ip)"
  BENCH_IP="$(_out bench_private_ip)"
  # Kept only for status/verify reporting; never connected to.
  POOL_PUBLIC_IP="$(_out pool_public_ip)"
  BENCH_PUBLIC_IP="$(_out bench_public_ip)"
  AWS_REGION_TF="$(_out aws_region)"
  S3_BUCKET_TF="$(_out s3_bucket)"
  SSH_IDENTITY="$(_out ssh_identity)"

  # ~ is not expanded inside a Terraform string, so the identity path arrives
  # literally as "~/.ssh/..." and every ssh call would fail on a file that does
  # not exist. Expand it here rather than in each caller.
  SSH_IDENTITY="${SSH_IDENTITY/#\~/$HOME}"

  export WEB_IP WEB_PRIVATE_IP POOL_IP BENCH_IP POOL_PUBLIC_IP BENCH_PUBLIC_IP AWS_REGION_TF S3_BUCKET_TF SSH_IDENTITY
}

_out() {
  local v
  v="$(printf '%s' "$_TF_JSON" | jq -r --arg k "$1" '.[$k].value // empty')"
  printf '%s' "$v"
}

# The private-IP outputs are always populated; the public ones are empty when the
# subnet does not auto-assign. Fail with the cause rather than with "ssh:
# Could not resolve hostname".
require_public_ips() {
  load_outputs
  # Only the web node needs to be reachable from here: it is the jump host for
  # the other two, which are addressed on their private IPs.
  [[ -n "$WEB_IP" ]] || die "the web node has no public IP, so nothing is reachable.
    Set associate_public_ip = true in infra/terraform.tfvars and re-apply."
  local missing=()
  [[ -z "$POOL_IP"  ]] && missing+=(pool)
  [[ -z "$BENCH_IP" ]] && missing+=(bench)
  (( ${#missing[@]} )) && die "no private IP for: ${missing[*]} — is infra/ fully applied?"
  return 0
}

# ---------------------------------------------------------------------- ssh
#
# A dedicated known_hosts file, because AWS recycles public IPs: the address
# that was the bench node last week belongs to somebody else's instance today,
# and the resulting REMOTE HOST IDENTIFICATION HAS CHANGED against the user's
# real ~/.ssh/known_hosts is both alarming and unhelpful. accept-new still
# refuses a key that changed WITHIN this deployment, which is the case that
# would actually matter.

ssh_opts() {
  local role="${1:-web}"
  mkdir -p "$STATE_DIR"

  local -a o=(
    -i "$SSH_IDENTITY"
    -o "UserKnownHostsFile=$KNOWN_HOSTS"
    -o StrictHostKeyChecking=accept-new
    -o ConnectTimeout=10
    -o ServerAliveInterval=15
    -o ServerAliveCountMax=4
    -o LogLevel=ERROR
  )

  # pool and bench accept SSH only from the web security group, so every
  # connection to them is relayed by the web node. An explicit ProxyCommand
  # rather than -J: ProxyJump does not reliably carry -i and the known-hosts
  # file through to the jump connection, and the key here is a dedicated one
  # that ssh will not find on its own.
  if [[ "$role" != "web" ]]; then
    o+=(-o "ProxyCommand=ssh -i $SSH_IDENTITY -o UserKnownHostsFile=$KNOWN_HOSTS -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 -o LogLevel=ERROR -W %h:%p ubuntu@$WEB_IP")
  fi

  printf '%s\n' "${o[@]}"
}

_ip_for_role() {
  case "$1" in
    web)   printf '%s' "$WEB_IP" ;;
    pool)  printf '%s' "$POOL_IP" ;;
    bench) printf '%s' "$BENCH_IP" ;;
    *) die "unknown role '$1' (want: web, pool, bench)" ;;
  esac
}

# node <role> <command...>  — run a command on a node, non-interactively.
#
# -n is load-bearing, not tidiness: deploy.sh runs three of these as background
# jobs, and a background ssh that reads the terminal's stdin takes SIGTTIN and
# stops. The whole deploy then hangs with no output and no error. Nothing here
# ever pipes data in — heredocs live inside the remote command string — so
# redirecting stdin from /dev/null costs nothing.
node() {
  local role="$1"; shift
  load_outputs
  local ip; ip="$(_ip_for_role "$role")"
  [[ -n "$ip" ]] || die "no public IP for the $role node; see require_public_ips"
  local opts; mapfile -t opts < <(ssh_opts "$role")
  ssh -n "${opts[@]}" "ubuntu@$ip" "$@"
}

# Same, but allocates a TTY and streams — for the long provisioning scripts,
# where watching the output IS the point.
node_tty() {
  local role="$1"; shift
  load_outputs
  local ip; ip="$(_ip_for_role "$role")"
  [[ -n "$ip" ]] || die "no public IP for the $role node"
  local opts; mapfile -t opts < <(ssh_opts "$role")
  ssh -t "${opts[@]}" "ubuntu@$ip" "$@"
}

node_scp() {
  local role="$1" src="$2" dest="$3"
  load_outputs
  local ip; ip="$(_ip_for_role "$role")"
  local opts; mapfile -t opts < <(ssh_opts "$role")
  scp "${opts[@]}" "$src" "ubuntu@$ip:$dest"
}

# Poll until cloud-init has finished. user_data writes the ready marker as its
# last action, and a failure marker if it aborts — so this distinguishes "still
# booting" from "booted and broken" instead of hanging forever on both.
wait_ready() {
  local role="$1" timeout="${2:-900}"
  local deadline=$(( SECONDS + timeout ))
  log "waiting for cloud-init on $role (up to $((timeout / 60))m)"
  while (( SECONDS < deadline )); do
    if node "$role" 'test -f /var/lib/cloud/flashmatch-ready' 2>/dev/null; then
      ok "$role ready"
      return 0
    fi
    if node "$role" 'test -f /var/lib/cloud/flashmatch-failed' 2>/dev/null; then
      bad "cloud-init FAILED on $role. Last 40 lines:"
      node "$role" 'sudo tail -40 /var/log/cloud-init-output.log' >&2 || true
      die "provisioning cannot continue with a broken $role node"
    fi
    sleep 10
  done
  die "timed out waiting for $role. Check: ops/aws/logs.sh $role cloud-init"
}

# ------------------------------------------------------------------ secrets
#
# Generated once and kept on the operator's disk. Not in Terraform state and not
# in the repo: POSTGRES_PASSWORD is read only when the cluster is initialised,
# so it must survive between a deploy and every later `docker compose` call, or
# the API crash-loops on "password authentication failed".

load_secrets() {
  mkdir -p "$STATE_DIR"; chmod 700 "$STATE_DIR"
  if [[ ! -f "$SECRETS_FILE" ]]; then
    need_cmd openssl
    umask 077
    printf 'POSTGRES_PASSWORD=%s\n' \
      "$(openssl rand -base64 33 | tr -d '\n/+=' | cut -c1-32)" > "$SECRETS_FILE"
    ok "generated a Postgres password → ${SECRETS_FILE/#$REPO_ROOT\//}"
  fi
  chmod 600 "$SECRETS_FILE"
  # shellcheck disable=SC1090
  source "$SECRETS_FILE"
  [[ -n "${POSTGRES_PASSWORD:-}" ]] || die "POSTGRES_PASSWORD missing from $SECRETS_FILE"
  export POSTGRES_PASSWORD
}

# --------------------------------------------------------------------- misc

# psql on the web node, through the compose container.
web_psql() {
  node web "docker exec -i flashmatch-postgres-1 psql -U mebench -d mebench $*"
}

# The admin API is loopback-only on the web node, so curl it from there rather
# than opening a tunnel for one request.
admin_curl() {
  local method="$1" path="$2"
  node web "curl -fsS -X $method http://127.0.0.1:8081$path"
}

human_duration() {
  local s="$1"
  printf '%dh%02dm' $(( s / 3600 )) $(( (s % 3600) / 60 ))
}
