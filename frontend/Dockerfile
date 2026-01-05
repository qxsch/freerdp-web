# RDP Web Client Frontend - Progressive WASM Decoder Build
# Multi-stage build: compile Progressive codec WASM with Emscripten, serve with nginx
#
# NOTE: Keep FreeRDP version in sync with backend/Dockerfile (currently 3.20.0)

# ==============================================================================
# Stage 1: Build Progressive WASM decoder with Emscripten + pthreads
# ==============================================================================
FROM emscripten/emsdk:3.1.51 AS wasm-builder

WORKDIR /build

# Copy Progressive codec sources
COPY progressive/ ./progressive/

# Build WASM with optimizations, SIMD, and pthreads for parallel tile decoding
WORKDIR /build/progressive
RUN mkdir -p build && \
    emcc -O3 -msimd128 -pthread \
        -s WASM=1 \
        -s MODULARIZE=1 \
        -s EXPORT_NAME="ProgressiveDecoderModule" \
        -s EXPORTED_FUNCTIONS="['_prog_create','_prog_free','_prog_create_surface','_prog_delete_surface','_prog_decompress','_prog_decompress_parallel','_prog_get_tile_data','_prog_get_dirty_tile_count','_prog_get_dirty_tile_info','_malloc','_free']" \
        -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap','getValue','setValue']" \
        -s USE_PTHREADS=1 \
        -s PTHREAD_POOL_SIZE=4 \
        -s ALLOW_MEMORY_GROWTH=1 \
        -s INITIAL_MEMORY=33554432 \
        -s STACK_SIZE=1048576 \
        -s ENVIRONMENT='web,worker' \
        -I. \
        rfx_rlgr.c rfx_dwt.c rfx_decode.c progressive_wasm.c \
        -o build/progressive_decoder.js && \
    echo "Progressive WASM decoder built successfully with pthread support" && \
    ls -la build/

# ==============================================================================
# Stage 2: Serve with nginx
# ==============================================================================
FROM nginx:alpine

# Copy static files (excluding progressive source)
COPY index.html /usr/share/nginx/html/
COPY rdp-client.js /usr/share/nginx/html/
COPY wire-format.js /usr/share/nginx/html/
COPY gfx-worker.js /usr/share/nginx/html/
COPY favicons/ /usr/share/nginx/html/favicons/

# Copy built WASM from builder stage (includes pthread worker)
COPY --from=wasm-builder /build/progressive/build/progressive_decoder.js /usr/share/nginx/html/progressive/
COPY --from=wasm-builder /build/progressive/build/progressive_decoder.wasm /usr/share/nginx/html/progressive/
COPY --from=wasm-builder /build/progressive/build/progressive_decoder.worker.js /usr/share/nginx/html/progressive/

# Custom nginx config with COOP/COEP headers
COPY nginx.conf /etc/nginx/conf.d/default.conf

# Expose HTTP port
EXPOSE 8000

# Default nginx command
CMD ["nginx", "-g", "daemon off;"]
