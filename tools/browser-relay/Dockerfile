# Build from the repository root:
# docker build --platform linux/amd64 -f tools/browser-relay/Dockerfile -t halo-browser-relay .
# The existing native port uses a 32-bit ABI; the JavaScript frontend is 64-bit.
FROM debian:bookworm-slim AS native
RUN apt-get update && apt-get install -y --no-install-recommends gcc-multilib libc6-dev-i386 make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY port/linux/src/ port/linux/src/
COPY port/third_party/kcp/ port/third_party/kcp/
COPY port/relay/ port/relay/
RUN mkdir -p /out && make -C port/relay OUTPUT=/out/halo-relay-worker

FROM node:24-bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libc6-i386 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY tools/browser-relay/package.json tools/browser-relay/package-lock.json ./
RUN npm ci --omit=dev --ignore-scripts --no-audit --no-fund
COPY tools/browser-relay/server.mjs tools/browser-relay/protocol.mjs ./
COPY --from=native /out/halo-relay-worker /usr/local/bin/halo-relay-worker
ENV NODE_ENV=production RELAY_HOST=0.0.0.0 RELAY_PORT=8789 RELAY_WORKER=/usr/local/bin/halo-relay-worker
USER node
EXPOSE 8789
HEALTHCHECK --interval=30s --timeout=5s CMD node -e "fetch('http://127.0.0.1:8789/health').then(r=>process.exit(r.ok?0:1)).catch(()=>process.exit(1))"
CMD ["node", "server.mjs"]
