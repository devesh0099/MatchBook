#!/usr/bin/env bash
# box-provisioner.sh — the dashboard's hands (M8).
#
# Runs as a systemd service ON THE WEB NODE, separate from the public API
# container so the internet-facing API never holds AWS launch rights. It polls
# box_requests, and for each fulfils the operator's intent against AWS:
#
#   deploy    launch a fresh box from the fleet AMI, bind it to the
#             participant, register it, mark participant ready.
#   redeploy  terminate the participant's current box, then deploy.
#   terminate tear the box down, mark participant boxless.
#
# On-demand launch (the chosen model): a box exists only while deployed, so you
# pay for what is deployed. ~2 min per deploy (AMI boot + bind).
#
# Config comes from /opt/flashmatch/provisioner.env (AMI, subnet, sg, profile,
# type, region, web private ip) and the sealed seeds from
# /opt/flashmatch/platform/.seeds. AWS creds come from the host's environment
# (instance role or a scoped key file); the API container does not have them.

set -uo pipefail

CFG=/opt/flashmatch/provisioner.env
SEEDS=/opt/flashmatch/platform/.seeds
KEY=/home/ubuntu/.ssh/fleet_key
PG='docker exec -i flashmatch-postgres-1 psql -U mebench -d mebench -tAc'

[[ -f $CFG ]] || { echo "missing $CFG"; exit 1; }
# shellcheck disable=SC1090
source "$CFG"
# shellcheck disable=SC1090
source "$SEEDS"

WORKER_ID="provisioner-$(hostname)"
log(){ echo "$(date -u +%H:%M:%S) $*"; }
sql(){ $PG "$1" 2>/dev/null; }
# escape single quotes for SQL string literals
esc(){ printf '%s' "$1" | sed "s/'/''/g"; }
set_detail(){ # participant_id, message
  local m; m=$(esc "$2")
  sql "UPDATE participants SET box_detail='$m' WHERE id=$1" >/dev/null
}

ssh_box(){ ssh -n -i "$KEY" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 -o LogLevel=ERROR "ubuntu@$1" "$2"; }

# launch one instance from the AMI, echo its instance id
launch_instance(){
  local pid="$1" name="flashmatch-box-p${pid}"
  aws ec2 run-instances --region "$REGION" \
    --image-id "$AMI" --instance-type "$ITYPE" \
    --subnet-id "$SUBNET" --security-group-ids "$SG" \
    --iam-instance-profile Name="$PROFILE" \
    --key-name "$KEYNAME" \
    --cpu-options CoreCount="$CORES",ThreadsPerCore=1 \
    --metadata-options 'HttpEndpoint=enabled,HttpTokens=required,HttpPutResponseHopLimit=1' \
    --block-device-mappings 'DeviceName=/dev/sda1,Ebs={VolumeSize=40,VolumeType=gp3,Encrypted=true}' \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Project,Value=flashmatch},{Key=Name,Value=$name},{Key=Role,Value=agent},{Key=Participant,Value=$pid}]" \
    --query 'Instances[0].InstanceId' --output text 2>/tmp/prov-launch.err
}

wait_running_ip(){ # instance_id -> echoes private ip
  local iid="$1" i state ip
  for i in $(seq 40); do
    read -r state ip < <(aws ec2 describe-instances --region "$REGION" --instance-ids "$iid" \
      --query 'Reservations[].Instances[].[State.Name,PrivateIpAddress]' --output text 2>/dev/null)
    [[ "$state" == running && -n "$ip" && "$ip" != None ]] && { echo "$ip"; return 0; }
    sleep 6
  done
  return 1
}

provision_box(){ # participant_id, private_ip -> bind + start agent
  local pid="$1" ip="$2" i
  # wait for cloud-init ready on the AMI-booted box
  for i in $(seq 50); do
    ssh_box "$ip" 'test -f /var/lib/cloud/flashmatch-ready' 2>/dev/null && break
    sleep 6
  done
  # bind this box to the participant; sealed seeds + 2-core steering
  ssh_box "$ip" "sudo tee /opt/mebench/worker.env >/dev/null <<ENVEOF
DATABASE_URL=postgres://mebench:${PGPASS}@${WEB_PRIVATE_IP}:5432/mebench
S3_BUCKET=${S3_BUCKET}
AWS_REGION=${REGION}
MEBENCH_PARTICIPANT_ID=${pid}
MEBENCH_SEED1=${MEBENCH_SEED1}
MEBENCH_SEED2=${MEBENCH_SEED2}
MEBENCH_BENCH_CPUS=1
MEBENCH_COMPILE_CPUS=0
MEBENCH_BAKE=/opt/mebench/bake
ENVEOF
sudo chmod 600 /opt/mebench/worker.env" || return 1
  ssh_box "$ip" 'sudo systemctl daemon-reload && sudo systemctl enable mebench-agent >/dev/null 2>&1; sudo systemctl restart mebench-agent && sleep 3 && systemctl is-active mebench-agent' || return 1
}

terminate_participant_box(){ # participant_id
  local pid="$1" iid
  iid=$(sql "SELECT box_instance_id FROM participants WHERE id=$pid" | tr -d '[:space:]')
  if [[ -n "$iid" && "$iid" != NULL ]]; then
    aws ec2 terminate-instances --region "$REGION" --instance-ids "$iid" >/dev/null 2>&1 || true
    log "terminated $iid for participant $pid"
  fi
  # clear this box's registry rows
  sql "DELETE FROM boxes WHERE participant_id=$pid" >/dev/null
}

# The operator's staged removal order: verify the instance, destroy it, WAIT
# until it is actually gone, clean the registry row, and only THEN set
# removed_at — which is what finally drops the row from the dashboard.
do_remove(){ # participant_id
  local pid="$1" iid i state
  iid=$(sql "SELECT box_instance_id FROM participants WHERE id=$pid" | tr -d '[:space:]')
  if [[ -n "$iid" && "$iid" != NULL ]]; then
    set_detail "$pid" "checking instance $iid"
    state=$(aws ec2 describe-instances --region "$REGION" --instance-ids "$iid" \
      --query 'Reservations[].Instances[].State.Name' --output text 2>/dev/null | tr -d '[:space:]')
    if [[ -n "$state" && "$state" != terminated ]]; then
      set_detail "$pid" "terminating $iid"
      aws ec2 terminate-instances --region "$REGION" --instance-ids "$iid" >/dev/null 2>&1 || true
      # wait until the instance is actually gone (or terminated), up to ~3 min
      for i in $(seq 30); do
        state=$(aws ec2 describe-instances --region "$REGION" --instance-ids "$iid" \
          --query 'Reservations[].Instances[].State.Name' --output text 2>/dev/null | tr -d '[:space:]')
        [[ -z "$state" || "$state" == terminated ]] && break
        set_detail "$pid" "waiting for shutdown ($state)"
        sleep 6
      done
    fi
    log "instance $iid gone for participant $pid"
  fi
  # only after the box is verified gone: clean the registry row, then remove.
  set_detail "$pid" "cleaning registry"
  sql "DELETE FROM boxes WHERE participant_id=$pid" >/dev/null
  sql "UPDATE participants SET removed_at=now(), box_state='none', box_instance_id=NULL, box_detail=NULL WHERE id=$pid" >/dev/null
  log "participant $pid removed (box torn down first)"
}

do_deploy(){ # participant_id
  local pid="$1" iid ip
  set_detail "$pid" "launching instance"
  iid=$(launch_instance "$pid")
  [[ "$iid" == i-* ]] || { set_detail "$pid" "launch failed: $(head -c 120 /tmp/prov-launch.err)"; return 1; }
  sql "UPDATE participants SET box_instance_id='$iid' WHERE id=$pid" >/dev/null
  set_detail "$pid" "waiting for boot ($iid)"
  ip=$(wait_running_ip "$iid") || { set_detail "$pid" "instance never got an IP"; return 1; }
  set_detail "$pid" "provisioning $ip"
  provision_box "$pid" "$ip" || { set_detail "$pid" "provisioning failed on $ip"; return 1; }
  set_detail "$pid" "ready ($ip)"
  return 0
}

log "box provisioner up (ami=$AMI type=$ITYPE region=$REGION)"
while true; do
  # claim one pending request. Wrapped in a CTE + final SELECT so ONLY the
  # tuple is emitted — a bare UPDATE ... RETURNING leaks its "UPDATE 0" status
  # tag on an empty match, which the parser then mistook for a request.
  row=$(sql "WITH c AS (
               UPDATE box_requests SET state='running', claimed_by='$WORKER_ID', updated_at=now()
               WHERE id=(SELECT id FROM box_requests WHERE state='pending' ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED)
               RETURNING id, participant_id, action)
             SELECT id||'|'||participant_id||'|'||action FROM c")
  # Only a real claim contains the delimiter and a numeric id.
  if [[ "$row" != *"|"* ]]; then sleep 3; continue; fi
  IFS='|' read -r req_id pid action <<< "$row"
  [[ "$req_id" =~ ^[0-9]+$ ]] || { sleep 3; continue; }
  log "request $req_id: $action participant $pid"
  ok=1
  case "$action" in
    deploy)     do_deploy "$pid" || ok=0 ;;
    redeploy)   terminate_participant_box "$pid"; do_deploy "$pid" || ok=0 ;;
    terminate)  terminate_participant_box "$pid"
                sql "UPDATE participants SET box_state='none', box_instance_id=NULL, box_detail=NULL WHERE id=$pid" >/dev/null ;;
    remove)     do_remove "$pid" ;;
    *)          log "unknown action $action"; ok=0 ;;
  esac

  # deploy/redeploy own the ready/failed transition; terminate and remove set
  # their own terminal participant state above.
  if [[ "$action" != terminate && "$action" != remove ]]; then
    if [[ "$ok" == 1 ]]; then
      sql "UPDATE participants SET box_state='ready' WHERE id=$pid" >/dev/null
    else
      sql "UPDATE participants SET box_state='failed' WHERE id=$pid" >/dev/null
    fi
  fi
  sql "UPDATE box_requests SET state='$([[ $ok == 1 ]] && echo done || echo failed)', updated_at=now() WHERE id=$req_id" >/dev/null
  log "request $req_id done (ok=$ok)"
done
