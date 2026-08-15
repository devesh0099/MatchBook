#!/usr/bin/env bash
# The two Rust version pins must agree: platform/rust-toolchain.toml (the
# source of truth) and the API Dockerfile's FROM tag. They rot independently —
# the lockfile ratchets the required toolchain upward whenever `cargo update`
# runs on a newer host — so this check exists to turn silent drift into a red
# build. Run by CI on every push; cheap enough to run by hand any time.
set -euo pipefail
cd "$(dirname "$0")/.."

pin=$(sed -n 's/^channel = "\(.*\)"/\1/p' platform/rust-toolchain.toml)
tag=$(sed -n 's/^FROM rust:\([0-9.]*\).*/\1/p' platform/api/Dockerfile | head -1)

if [ -z "$pin" ] || [ -z "$tag" ]; then
  echo "check-toolchain: could not read one of the pins (toolchain='$pin', dockerfile='$tag')" >&2
  exit 1
fi

# The Dockerfile tag may be shorter (1.96 vs 1.96.0); prefix-match on dots.
case "$pin" in
  "$tag" | "$tag".*)
    echo "check-toolchain: ok — rust-toolchain.toml $pin, Dockerfile rust:$tag"
    ;;
  *)
    echo "check-toolchain: MISMATCH — rust-toolchain.toml pins $pin but the API" >&2
    echo "Dockerfile builds FROM rust:$tag. Update both in the same commit." >&2
    exit 1
    ;;
esac
