#!/usr/bin/env bash
# destroy.sh — stop the meter.
#
#   ops/aws/destroy.sh              # the three instances; backs up first
#   ops/aws/destroy.sh --no-backup  # skip the dump (it will ask twice)
#   ops/aws/destroy.sh --all        # also the bucket and IAM from bootstrap
#
# The instances are the whole cost — about $0.78/hour, or $560 a month if this
# is forgotten, which is the realistic failure rather than the hourly rate.
# Bootstrap's bucket and roles cost essentially nothing, so --all is for
# cleaning up an account, not for saving money.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

DO_BACKUP=1
ALL=0
ASSUME_YES=0

for a in "$@"; do
  case "$a" in
    --no-backup) DO_BACKUP=0 ;;
    --all)       ALL=1 ;;
    --yes|-y)    ASSUME_YES=1 ;;
    -h|--help)   sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's|^# \?||'; exit 0 ;;
    *) die "unknown argument: $a" ;;
  esac
done
export ASSUME_YES

if ! have_state "$INFRA_DIR"; then
  log "infra/ has no state — nothing to destroy"
else
  # ------------------------------------------------------------- back up first
  if (( DO_BACKUP )); then
    step "backing up the database before anything is destroyed"
    if "$AWS_DIR/backup.sh"; then
      ok "backup complete"
    else
      warn "the backup FAILED"
      confirm "Destroy anyway, losing every submission and the events log?" \
        || die "aborted; nothing was destroyed"
    fi
  else
    warn "skipping the backup (--no-backup)"
    confirm "The database holds every submission and the full events log. Really skip it?" \
      || die "aborted; run without --no-backup"
  fi

  # ------------------------------------------------------------------- destroy
  step "planning the teardown"
  tf "$INFRA_DIR" plan -destroy -input=false -out=/tmp/infra-destroy.tfplan
  echo >&2
  confirm "Destroy the three instances and their security groups?" \
    || die "aborted; nothing was destroyed"
  tf "$INFRA_DIR" apply -input=false /tmp/infra-destroy.tfplan
  rm -f /tmp/infra-destroy.tfplan
  ok "instances destroyed — the meter has stopped"
fi

# ------------------------------------------------------------------ bootstrap

if (( ALL )); then
  if ! have_state "$BOOTSTRAP_DIR"; then
    log "infra/bootstrap has no state — nothing to destroy"
  else
    step "bootstrap: the artifact bucket and IAM"
    bucket="$(tf "$BOOTSTRAP_DIR" output -raw s3_bucket 2>/dev/null || echo "?")"
    force="$(terraform -chdir="$BOOTSTRAP_DIR" console <<<'var.force_destroy_bucket' 2>/dev/null | tr -d '"')"

    cat >&2 <<EOF

    ${C_YELLOW}This deletes $bucket, which holds every submission's SOURCE.${C_RESET}
    force_destroy_bucket = $force
EOF
    [[ "$force" == "true" ]] && warn "force_destroy is on: objects will be deleted with the bucket, versions included"

    confirm "Destroy the artifact bucket and the IAM roles?" || die "aborted; bootstrap left intact"
    # An array, because ${ASSUME_YES:+-auto-approve} expands on any non-empty
    # value — including the literal "0" — which would have skipped Terraform's
    # own confirmation on a bucket holding every submission.
    approve=()
    (( ASSUME_YES )) && approve=(-auto-approve)
    tf "$BOOTSTRAP_DIR" destroy -input=false "${approve[@]}"
    ok "bootstrap destroyed"
  fi
else
  log "bootstrap left alone (bucket and IAM cost ~nothing). Pass --all to remove them too."
fi

# Local state that no longer refers to anything. The backups are deliberately
# NOT removed — they are the only remaining record of the event.
if [[ -f "$SECRETS_FILE" ]] && ! have_state "$INFRA_DIR"; then
  rm -f "$SECRETS_FILE" "$KNOWN_HOSTS"
  log "cleared the generated Postgres password and known_hosts"
  [[ -d "$STATE_DIR/backups" ]] && log "backups kept in ${STATE_DIR/#$REPO_ROOT\//}/backups"
fi

echo >&2
printf '%steardown complete.%s Confirm with: ops/aws/status.sh\n\n' "$C_GREEN$C_BOLD" "$C_RESET" >&2
