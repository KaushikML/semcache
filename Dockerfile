# syntax=docker/dockerfile:1
# semcache image: build with the same pinned inputs as CI (vcpkg baseline, ORT tarball SHA-256,
# model revision SHA-256), ship a small runtime with the model baked in. See docs/deploy.md.
#
#   docker build -t semcache .
#   docker compose up            # demo: semcache + mock LLM, dashboard on http://127.0.0.1:8080/dashboard

ARG BASE=ubuntu:24.04

# ---------- build ----------
FROM ${BASE} AS build
ENV DEBIAN_FRONTEND=noninteractive
# perl: OpenSSL's build; python3: vcpkg's meson tool. Build stage only, never in the runtime image.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git curl ca-certificates zip unzip tar pkg-config perl python3 \
    && rm -rf /var/lib/apt/lists/*

# The dev box has ~8 GB RAM: never more than 4 compile jobs (CLAUDE.md). Same limit here.
ARG JOBS=4
ENV VCPKG_MAX_CONCURRENCY=${JOBS} \
    VCPKG_DEFAULT_BINARY_CACHE=/root/.cache/vcpkg-binary \
    VCPKG_DISABLE_METRICS=1
WORKDIR /src

# Dependencies first, so source edits do not rebuild them.
COPY vcpkg.json ./
COPY scripts/bootstrap-vcpkg.sh scripts/
RUN --mount=type=cache,target=/root/.cache/vcpkg-binary \
    mkdir -p "$VCPKG_DEFAULT_BINARY_CACHE" \
    && scripts/bootstrap-vcpkg.sh \
    && external/vcpkg/vcpkg install --x-install-root=/src/vcpkg_installed

# Model: pinned HuggingFace revision, SHA-256 checked by the script.
COPY scripts/fetch-model.sh scripts/
RUN scripts/fetch-model.sh /opt/semcache/models

COPY . .
# The toolchain file reruns vcpkg (a no-op here); it needs the binary cache directory to exist.
RUN --mount=type=cache,target=/root/.cache/vcpkg-binary \
    cmake --preset release -DSEMCACHE_BUILD_TESTS=OFF -DSEMCACHE_BUILD_BENCH=OFF \
          -DCMAKE_INSTALL_PREFIX=/opt/semcache \
    && cmake --build build/release -j${JOBS} \
    && cmake --install build/release --strip

# ---------- runtime ----------
FROM ${BASE} AS runtime
ENV DEBIAN_FRONTEND=noninteractive
# ca-certificates: the static OpenSSL uses /etc/ssl/certs to verify the upstream (OPENSSLDIR=/etc/ssl).
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --home-dir /var/lib/semcache --shell /usr/sbin/nologin semcache \
    && mkdir -p /var/lib/semcache/data \
    && chown -R semcache:semcache /var/lib/semcache

COPY --from=build /opt/semcache /opt/semcache
COPY config/docker.toml /etc/semcache/semcache.toml
COPY config/docker-demo.toml /etc/semcache/demo.toml

USER semcache
WORKDIR /var/lib/semcache
EXPOSE 8080
# No curl in the image: bash's /dev/tcp is enough for a liveness probe.
HEALTHCHECK --interval=10s --timeout=3s --start-period=30s --retries=3 \
    CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/8080 && printf 'GET /healthz HTTP/1.0\\r\\n\\r\\n' >&3 && head -n1 <&3 | grep -q ' 200 '"]
ENTRYPOINT ["/opt/semcache/bin/semcache"]
CMD ["--config", "/etc/semcache/semcache.toml"]
