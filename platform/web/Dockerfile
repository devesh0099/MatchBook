# Built from the repo root: the sync step reads engine/ for the frozen headers,
# the skeleton and the spec, so the build context has to include them.
#
#   docker build -f platform/web/Dockerfile .
#
# compose.yaml sets the context accordingly.

FROM node:22-alpine AS build
WORKDIR /app

COPY platform/web/package.json platform/web/package-lock.json* ./
RUN npm ci --no-audit --no-fund

COPY platform/web/ ./
# The sync step runs INSIDE the image build. It vendors Monaco into public/ and
# generates the starting buffer from engine/, so both must be present here — an
# image built without it starts an editor with no content and no editor.
# sync-assets resolves these as <web>/../../engine and <web>/../../spec, which
# from /app is exactly /engine and /spec.
COPY engine/ /engine/
COPY spec/ /spec/

# API_ORIGIN is deliberately unset: in production Caddy puts the frontend and the
# API on one origin and the browser calls /api directly, so no rewrite is needed
# and none is baked in.
RUN npm run build

FROM node:22-alpine AS run
WORKDIR /app
ENV NODE_ENV=production PORT=3000 HOSTNAME=0.0.0.0

# `output: standalone` emits a self-contained server plus only the node_modules
# it actually needs.
COPY --from=build /app/.next/standalone ./
COPY --from=build /app/.next/static ./.next/static
COPY --from=build /app/public ./public

EXPOSE 3000
CMD ["node", "server.js"]
