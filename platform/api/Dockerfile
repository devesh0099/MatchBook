# Built from the repo root so the shared `common` crate is in context.
#
# The toolchain pin has to be at least as new as the MSRV of everything in
# Cargo.lock, and that ratchets upward on its own. This was 1.83 and had
# silently stopped building: cpufeatures moved to edition 2024, which Cargo 1.83
# cannot even parse, and time/icu_provider now want 1.88. Nothing in this repo
# changed to cause it — the lock file was simply regenerated on a newer host
# toolchain, and no CI builds this image, so it broke unobserved.
#
# Keep this at least as new as the toolchain Cargo.lock was resolved with —
# 1.96 at the time of writing — rather than chasing individual MSRVs upward one
# failure at a time. `docker compose build` on the web node is the only thing
# that exercises this image, so a stale pin is invisible until provisioning day.
#
# The DISTRO is pinned as well as the version. `rust:1.96-slim` is trixie, and
# linking against its glibc produced a binary the bookworm runtime stage below
# could not load at all ("GLIBC_2.39 not found") — a build that succeeds and an
# image that cannot start. Both stages must name the same Debian release.
FROM rust:1.96-slim-bookworm AS build
WORKDIR /src
RUN apt-get update && apt-get install -y --no-install-recommends pkg-config libssl-dev \
    && rm -rf /var/lib/apt/lists/*
COPY platform/Cargo.toml platform/Cargo.lock* ./
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
