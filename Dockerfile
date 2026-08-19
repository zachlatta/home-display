# Gateway image for Coolify. Only server/ ships: the firmware and the simulator
# are build-time tooling and have no place in a long-running container.

FROM node:22-slim AS deps
WORKDIR /app
COPY package.json package-lock.json ./
# sharp ships prebuilt libvips binaries for linux x64 and arm64, so no toolchain
# is needed here.
RUN npm ci --omit=dev && npm cache clean --force

FROM node:22-slim
ENV NODE_ENV=production \
    HOME_DISPLAY_HOST=0.0.0.0 \
    HOME_DISPLAY_PORT=8787 \
    HOME_DISPLAY_CACHE_FILE=/data/dashboard-cache.json
WORKDIR /app

# The 24-hour last-good payload lives on a volume so a redeploy does not cost the
# display its fallback.
RUN mkdir -p /data && chown node:node /data

# Root-owned, runtime user has read-only access. The process must not be able to
# rewrite its own source, its dependencies, or the SQL it runs.
COPY --from=deps --chown=root:root /app/node_modules ./node_modules
COPY --chown=root:root package.json ./
COPY --chown=root:root server ./server

USER node
EXPOSE 8787
VOLUME ["/data"]

HEALTHCHECK --interval=60s --timeout=5s --start-period=10s --retries=3 \
  CMD node -e "fetch('http://127.0.0.1:'+(process.env.HOME_DISPLAY_PORT||8787)+'/health').then(r=>process.exit(r.ok?0:1)).catch(()=>process.exit(1))"

CMD ["node", "server/dashboard.mjs"]
