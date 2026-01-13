# RDP Web Client Frontend - Progressive & ClearCodec WASM Decoder Build
# Multi-stage build: compile WASM decoders with Emscripten, serve with nginx
#
# NOTE: Keep FreeRDP version in sync with backend/Dockerfile (currently 3.20.0)

# ==============================================================================
# Stage 1: Build Progressive WASM decoder with Emscripten + pthreads
# ==============================================================================
FROM emscripten/emsdk:3.1.51 AS wasm-builder

WORKDIR /build

# Copy Progressive codec sources
COPY progressive/ ./progressive/

# Copy ClearCodec codec sources
COPY clearcodec/ ./clearcodec/

# Build Progressive WASM using CMakeLists.txt (single source of truth for build config)
WORKDIR /build/progressive
RUN mkdir -p build && cd build && \
    emcmake cmake .. && \
    emmake make && \
    echo "Progressive WASM decoder built successfully with pthread support" && \
    ls -la

# Build ClearCodec WASM using CMakeLists.txt (single source of truth for build config)
WORKDIR /build/clearcodec
RUN mkdir -p build && cd build && \
    emcmake cmake .. && \
    emmake make && \
    echo "ClearCodec WASM decoder built successfully" && \
    ls -la

# ==============================================================================
# Stage 2: Serve with nginx
# ==============================================================================
FROM nginx:alpine

# Copy static files (excluding progressive source)
COPY index*.html /usr/share/nginx/html/
COPY rdp-client.js /usr/share/nginx/html/
COPY rdp-themes.js /usr/share/nginx/html/
COPY wire-format.js /usr/share/nginx/html/
COPY gfx-worker.js /usr/share/nginx/html/
COPY favicons/ /usr/share/nginx/html/favicons/

# Inject build timestamp into gfx-worker.js
RUN BUILD_TIME=$(date +%H:%M:%S) && \
    sed -i "s/__BUILD_TIME__/$BUILD_TIME/g" /usr/share/nginx/html/gfx-worker.js && \
    echo "gfx-worker.js build time: $BUILD_TIME"

# Copy built WASM from builder stage (includes pthread worker for progressive)
COPY --from=wasm-builder /build/progressive/build/progressive_decoder.js /usr/share/nginx/html/progressive/
COPY --from=wasm-builder /build/progressive/build/progressive_decoder.wasm /usr/share/nginx/html/progressive/
COPY --from=wasm-builder /build/progressive/build/progressive_decoder.worker.js /usr/share/nginx/html/progressive/

# Copy ClearCodec WASM
COPY --from=wasm-builder /build/clearcodec/build/clearcodec_decoder.js /usr/share/nginx/html/clearcodec/
COPY --from=wasm-builder /build/clearcodec/build/clearcodec_decoder.wasm /usr/share/nginx/html/clearcodec/

# Custom nginx config with COOP/COEP headers
COPY nginx.conf /etc/nginx/conf.d/default.conf

# Expose HTTP port
EXPOSE 8000

# Default nginx command
CMD ["nginx", "-g", "daemon off;"]
