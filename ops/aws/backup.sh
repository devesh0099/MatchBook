#!/usr/bin/env bash
# backup.sh — pg_dump the platform database to local disk and to S3.
#
#   ops/aws/backup.sh                 # dump, save locally, upload to the bucket
#   ops/aws/backup.sh --no-upload     # local only
#
# The dump is streamed over SSH and uploaded from HERE rather than from the web
# node, because the AWS CLI is deliberately not installed on the nodes — the
# platform's own S3 access goes through the SDK and the instance role, and adding
# a CLI just for backups would widen the node's surface for no reason.
#
# Run this before destroy.sh. The database holds every submission, every state
# transition and the events log — the leaderboard is the least of it.

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

UPLOAD=1
for a in "$@"; do
  case "$a" in
    --no-upload) UPLOAD=0 ;;
    -h|--help) sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's|^# \?||'; exit 0 ;;
    *) die "unknown argument: $a" ;;
  esac
done

need_cmd aws
load_outputs
[[ -n "$WEB_IP" ]] || die "the web node has no public IP; cannot reach it to dump"

BACKUP_DIR="$STATE_DIR/backups"
mkdir -p "$BACKUP_DIR"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="$BACKUP_DIR/mebench-$STAMP.sql.gz"

step "dumping the database"
# -Fp | gzip rather than -Fc: a plain dump can be inspected with zless a year
# from now without a matching pg_restore, which is what you want from a record
# of a one-day event.
if ! node web "docker exec -i flashmatch-postgres-1 pg_dump -U mebench -d mebench --no-owner" \
     | gzip -9 > "$OUT"; then
  rm -f "$OUT"
  die "pg_dump failed — is the postgres container running? (ops/aws/status.sh)"
fi

SIZE="$(du -h "$OUT" | cut -f1)"
# A dump of an empty schema is still a few KB, so size alone is a weak signal;
# check that it actually contains the tables that matter.
if ! zgrep -qE 'COPY public\.(submissions|participants)' "$OUT" 2>/dev/null; then
  warn "the dump contains no submissions or participants rows — verify before relying on it"
fi
ok "$OUT ($SIZE)"

if (( UPLOAD )); then
  [[ -n "$S3_BUCKET_TF" ]] || die "no s3_bucket output; cannot upload"
  export AWS_REGION="$AWS_REGION_TF" AWS_DEFAULT_REGION="$AWS_REGION_TF"
  if profile="$(terraform -chdir="$INFRA_DIR" console <<<'var.aws_profile' 2>/dev/null | tr -d '"')"; then
    [[ -n "$profile" && "$profile" != "null" ]] && export AWS_PROFILE="$profile"
  fi

  step "uploading"
  DEST="s3://$S3_BUCKET_TF/backups/mebench-$STAMP.sql.gz"
  if aws s3 cp "$OUT" "$DEST" --only-show-errors; then
    ok "$DEST"
  else
    warn "upload failed — the local copy at $OUT is intact"
    exit 1
  fi
fi

echo >&2
