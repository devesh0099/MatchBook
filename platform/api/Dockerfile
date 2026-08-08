# Built from the repo root so the shared `common` crate is in context.
FROM rust:1.83-slim AS build
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
