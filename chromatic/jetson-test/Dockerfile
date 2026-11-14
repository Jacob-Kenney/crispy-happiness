FROM node:22-alpine
WORKDIR /app
COPY app.mjs protocol.mjs sglink.wasm ./
USER node
CMD ["node", "app.mjs"]
