#!/usr/bin/env bash
# install-isolate.sh — build and install ioi/isolate.
#
# Run as root on the correctness pool and on the benchmark node. Also usable on
# a dev box to exercise the worker's sandbox paths.
#
# Note: cms-dev/isolate is the deprecated fork; this is ioi/isolate, maintained
# by Martin Mares and purpose-built for grading untrusted contest submissions.
#
# isolate is setuid root by design. That is not incidental: it needs to create
# namespaces and cgroups. Read the Makefile before running this on anything you
# care about.

set -euo pipefail

ISOLATE_REF="${ISOLATE_REF:-cf03a90}"   # pinned; bump deliberately, not by drift
SRC_DIR="${SRC_DIR:-/usr/local/src/isolate}"

if [[ $EUID -ne 0 ]]; then
  echo "must run as root (isolate installs setuid root)" >&2
  exit 1
fi

echo "==> checking cgroup version"
CGROUP_FS=$(stat -fc %T /sys/fs/cgroup/)
if [[ "$CGROUP_FS" != "cgroup2fs" ]]; then
  echo "warning: expected cgroup v2 (cgroup2fs), found '$CGROUP_FS'." >&2
  echo "isolate's cgroup flags differ between v1 and v2 — check 'man isolate'." >&2
fi

echo "==> installing build dependencies"
export DEBIAN_FRONTEND=noninteractive

# A broken THIRD-PARTY repo must not stop us installing from the distro repos.
# apt-get update exits non-zero if any configured source fails, and a developer
# box accumulates sources that have nothing to do with this. The real gate is
# the install below: if a package genuinely cannot be found, that fails loudly.
apt-get update -qq || echo "  (apt-get update reported errors; continuing — see above)"

# libseccomp: the syscall filter. Here it is a FAIRNESS mechanism as much as a
# security one — blocking clone stops thread-based gaming of the wall clock.
# libsystemd: needed by isolate-cg-keeper, which owns the cgroup hierarchy.
DEPS=(build-essential git pkg-config libcap-dev libseccomp-dev libsystemd-dev)
if ! apt-get install -y --no-install-recommends "${DEPS[@]}"; then
  echo >&2
  echo "could not install build dependencies. If apt-get update failed above," >&2
  echo "the package lists may be stale. Disable the broken source and retry:" >&2
  echo "  grep -rl 'aaddrick\\|claude-desktop' /etc/apt/sources.list.d/" >&2
  exit 1
fi

for d in libseccomp-dev libsystemd-dev libcap-dev; do
  dpkg -s "$d" >/dev/null 2>&1 || { echo "missing dependency: $d" >&2; exit 1; }
done

echo "==> fetching isolate @ ${ISOLATE_REF}"
if [[ -d "$SRC_DIR/.git" ]]; then
  git -C "$SRC_DIR" fetch --depth 50 origin
else
  rm -rf "$SRC_DIR"
  git clone --quiet https://github.com/ioi/isolate "$SRC_DIR"
fi
git -C "$SRC_DIR" checkout --quiet "$ISOLATE_REF"

echo "==> building"
make -C "$SRC_DIR" isolate isolate-cg-keeper

echo "==> installing"
make -C "$SRC_DIR" install

# isolate 2.6 runs each box under a distinct subordinate UID, and reads the
# range from /etc/subuid for the user named by `subid_user` in its config
# (default: isolate). Without this, isolate-cg-keeper exits with
# "User isolate not found in /etc/subuid" and --cg does not work at all.
#
# Matching upstream's Debian postinst: the account is deliberately NOT a system
# user, because adduser cannot assign subuid ranges to system users.
echo "==> creating the isolate user and its subordinate UID range"
if ! getent group isolate >/dev/null; then
  addgroup --quiet --system isolate
fi
if ! getent passwd isolate >/dev/null; then
  ADDUSER_CMT=--comment
  . /etc/os-release
  [[ "${UBUNTU_CODENAME:-}" == jammy ]] && ADDUSER_CMT=--gecos
  adduser --quiet --disabled-login --ingroup isolate --home /nonexistent \
          --no-create-home --shell /bin/false "$ADDUSER_CMT" "" isolate
fi

# adduser only assigns subuid/subgid ranges on some configurations, so make it
# explicit rather than hoping. 65536 ids is far more than the handful of boxes
# this platform uses.
for f in /etc/subuid /etc/subgid; do
  touch "$f"
  if ! grep -q '^isolate:' "$f"; then
    echo "isolate:1000000:65536" >> "$f"
    echo "  added isolate range to $f"
  fi
done

# isolate-cg-keeper owns the cgroup subtree isolate puts boxes into. Without it
# running, --cg fails on cgroup v2.
if [[ -f "$SRC_DIR/systemd/isolate.service" ]]; then
  systemctl daemon-reload
  systemctl enable --now isolate.service || {
    echo "could not start isolate.service; on a container or a host without" >&2
    echo "systemd you will need to run isolate-cg-keeper by hand." >&2
  }
fi

echo "==> verifying"
isolate --version
# isolate ships its own environment checker. It reports the things that make
# measurements unreliable — frequency scaling, turbo, address space
# randomisation — which is exactly what the benchmark node cares about.
if command -v isolate-check-environment >/dev/null; then
  echo "--- isolate-check-environment (advisory) ---"
  isolate-check-environment || true
fi

echo
echo "isolate installed. Smoke test:"
echo "  isolate --cg --box-id 0 --init"
echo "  isolate --cg --box-id 0 --run -- /bin/echo hello"
echo "  isolate --cg --box-id 0 --cleanup"
