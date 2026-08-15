# Built from the repo root so the shared `common` crate is in context.
#
# The toolchain version is pinned ONCE, in platform/rust-toolchain.toml, and
# copied in below: the rust image ships rustup, so cargo auto-installs the
# pinned version if this FROM tag ever drifts from it — the build self-heals
# instead of failing on an MSRV the stale image cannot parse. Keep the tag
# matching the pin anyway (ops/check-toolchain.sh enforces it, and CI builds
# this image on every push) so the normal case downloads nothing.
#
# History of why this exists: the pin was once 1.83 and had silently stopped
# building — Cargo.lock was regenerated on a newer host toolchain, cpufeatures
# moved to edition 2024 which Cargo 1.83 cannot even parse, and nothing built
# this image between provisioning days.
#
# The DISTRO is pinned as well as the version. `rust:1.96-slim` is trixie, and
# linking against its glibc produced a binary the bookworm runtime stage below
# could not load at all ("GLIBC_2.39 not found") — a build that succeeds and an
# image that cannot start. Both stages must name the same Debian release.
FROM rust:1.96-slim-bookworm AS build
WORKDIR /src
RUN apt-get update && apt-get install -y --no-install-recommends pkg-config libssl-dev \
    && rm -rf /var/lib/apt/lists/*
COPY platform/rust-toolchain.toml platform/Cargo.toml platform/Cargo.lock* ./
COPY platform/common ./common
COPY platform/api ./api
COPY platform/worker ./worker
RUN cargo build --release --bin api

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/target/release/api /usr/local/bin/api
# Queries are runtime-checked, so this image builds without a live database.
ENV BIND_ADDR=0.0.0.0:8080 ADMIN_BIND_ADDR=127.0.0.1:8081
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/api"]
