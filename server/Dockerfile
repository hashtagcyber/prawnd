FROM node:20-alpine AS build
WORKDIR /app
RUN apk add --no-cache python3 make g++
COPY package.json ./
RUN npm install --omit=dev --no-audit --no-fund \
 && npm cache clean --force
COPY src ./src

FROM node:20-alpine
WORKDIR /app
RUN apk add --no-cache tini wget ffmpeg
COPY --from=build /app /app
RUN mkdir -p /app/data && chown -R node:node /app
USER node
ENV NODE_ENV=production \
    PORT=8080 \
    HOST=0.0.0.0 \
    DATA_DIR=/app/data
VOLUME ["/app/data"]
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s \
  CMD wget -qO- http://127.0.0.1:8080/healthz >/dev/null 2>&1 || exit 1
ENTRYPOINT ["/sbin/tini","--"]
CMD ["node","src/server.js"]
