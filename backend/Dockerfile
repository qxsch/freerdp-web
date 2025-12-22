# RDP WebSocket Proxy Backend - Native FreeRDP3 Bridge
# Multi-stage build: compile C library, then create runtime image

# ==============================================================================
# Stage 1: Build the native RDP bridge library (Ubuntu 24.04 has FreeRDP3)
# ==============================================================================
FROM ubuntu:24.04 AS builder

# Install build dependencies for FreeRDP3
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    freerdp3-dev \
    libwinpr3-dev \
    zlib1g-dev \
    libssl-dev \
    libopus-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy native source code
WORKDIR /build
COPY native/ ./native/

# Build the libraries (main library + RDPSND bridge plugin)
WORKDIR /build/native
RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build build --parallel $(nproc) \
    && cmake --install build

# Verify the libraries were built
RUN ls -la /usr/local/lib/librdp_bridge.so* \
    && ls -la /usr/local/lib/freerdp3/librdpsnd-client-bridge.so \
    && echo "Native libraries built successfully"

# ==============================================================================
# Stage 2: Runtime image (Ubuntu 24.04 for FreeRDP3 runtime libs)
# ==============================================================================
FROM ubuntu:24.04

# Install Python and FreeRDP3 runtime libraries (no PulseAudio needed)
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 \
    python3-pip \
    python3-venv \
    libfreerdp3-3 \
    libfreerdp-client3-3 \
    libwinpr3-3 \
    libwebp7 \
    libopus0 \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Copy the native libraries from builder
COPY --from=builder /usr/local/lib/librdp_bridge.so* /usr/local/lib/
COPY --from=builder /usr/local/include/rdp_bridge.h /usr/local/include/

# Copy the RDPSND bridge plugin to FreeRDP plugin directory
# FreeRDP3 searches for plugins in /usr/lib/x86_64-linux-gnu/freerdp3
RUN mkdir -p /usr/lib/x86_64-linux-gnu/freerdp3
COPY --from=builder /usr/local/lib/freerdp3/librdpsnd-client-bridge.so /usr/lib/x86_64-linux-gnu/freerdp3/

# Update library cache with explicit path
RUN echo "/usr/local/lib" > /etc/ld.so.conf.d/local.conf && ldconfig

# Verify libraries are loadable
RUN python3 -c "import ctypes; ctypes.CDLL('/usr/local/lib/librdp_bridge.so'); print('Native library loaded successfully')"
RUN ls -la /usr/lib/x86_64-linux-gnu/freerdp3/ && echo "RDPSND bridge plugin installed"

# Set working directory
WORKDIR /app

# Create virtual environment to avoid PEP 668 issues
RUN python3 -m venv /app/venv
ENV PATH="/app/venv/bin:$PATH"

# Copy requirements first for better caching
COPY requirements.txt .

# Install Python dependencies
RUN pip install --no-cache-dir -r requirements.txt

# Verify Pillow has WebP support
RUN python3 -c "from PIL import features; print('WebP support:', features.check('webp'))"

# Copy application code
COPY *.py ./
COPY .env.example .env.example

# Create .env if not exists
RUN cp -n .env.example .env 2>/dev/null || true

# Set library path (include FreeRDP plugin path)
ENV LD_LIBRARY_PATH=/usr/local/lib
ENV FREERDP_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/freerdp3

# Expose WebSocket port
EXPOSE 8765

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD python3 -c "import socket; s=socket.socket(); s.connect(('127.0.0.1', 8765)); s.close()" || exit 1

# Run the server directly (no PulseAudio needed - native Opus audio streaming)
CMD ["python", "server.py"]
