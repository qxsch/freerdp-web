# RDP WebSocket Proxy Backend - Native FreeRDP3 Bridge with H.264 Support
# Multi-stage build: compile FreeRDP3 with GFX H.264, then build bridge library
#
# NOTE: Ubuntu's FreeRDP3 package is compiled WITHOUT H.264 support (WITH_GFX_H264=OFF)
# because FFmpeg is in 'universe' not 'main'. We must compile FreeRDP3 from source.

# ==============================================================================
# Stage 1: Build FreeRDP3 from source with H.264/AVC444 support
# ==============================================================================
FROM ubuntu:24.04 AS freerdp-builder

# Install FreeRDP build dependencies + FFmpeg for H.264 codec
RUN apt-get update && apt-get install -y --no-install-recommends \
    git \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    libssl-dev \
    libx11-dev \
    libxext-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxkbfile-dev \
    libxv-dev \
    libxi-dev \
    libxdamage-dev \
    libxrandr-dev \
    libxrender-dev \
    libxfixes-dev \
    libfuse3-dev \
    libasound2-dev \
    libcups2-dev \
    libpulse-dev \
    libudev-dev \
    libdbus-glib-1-dev \
    uuid-dev \
    libxml2-dev \
    libgss-dev \
    libpkcs11-helper1-dev \
    libusb-1.0-0-dev \
    libcjson-dev \
    libsdl2-dev \
    libsdl2-ttf-dev \
    libkrb5-dev \
    libpcsclite-dev \
    libswscale-dev \
    libavcodec-dev \
    libavutil-dev \
    libswresample-dev \
    libwebp-dev \
    libopus-dev \
    libsystemd-dev \
    libcairo2-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ARG FREERDP_VERSION=3.20.0

# Clone FreeRDP3 stable branch
WORKDIR /build
RUN git clone --depth 1 --branch ${FREERDP_VERSION} https://github.com/FreeRDP/FreeRDP.git freerdp && \
    echo "FreeRDP ${FREERDP_VERSION} cloned successfully"

# Build FreeRDP3 with H.264/AVC444 support enabled
# H.264 GFX support requires WITH_FFMPEG=ON which enables WITH_VIDEO_FFMPEG
# This compiles h264_ffmpeg.c and defines WITH_GFX_H264
WORKDIR /build/freerdp
RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/opt/freerdp3 \
    -DWITH_VERBOSE_WINPR_ASSERT=OFF \
    -DWITH_SERVER=OFF \
    -DWITH_SAMPLE=OFF \
    -DWITH_MANPAGES=OFF \
    -DWITH_FFMPEG=ON \
    -DWITH_SWSCALE=ON \
    -DWITH_DSP_FFMPEG=ON \
    -DWITH_VIDEO_FFMPEG=ON \
    -DWITH_OPUS=ON \
    -DWITH_WEBVIEW=OFF \
    -DWITH_CLIENT_SDL=OFF \
    -DWITH_PROXY=OFF \
    -DWITH_SHADOW=OFF \
    -DCHANNEL_RDPGFX=ON \
    -DCHANNEL_RDPSND=ON \
    -DCHANNEL_DISP=ON \
    -DCHANNEL_CLIPRDR=ON \
    -DWITH_PULSE=OFF \
    -DWITH_ALSA=OFF \
    && cmake --build build --parallel $(nproc) \
    && cmake --install build

# Verify H.264 support is compiled in
RUN echo "Checking FreeRDP build info..." \
    && ls -la /opt/freerdp3/lib/ \
    && ls -la /opt/freerdp3/lib/freerdp3/ || true \
    && echo "FreeRDP3 with H.264 support built successfully"

# ==============================================================================
# Stage 2: Build the native RDP bridge library against our custom FreeRDP3
# ==============================================================================
FROM ubuntu:24.04 AS bridge-builder

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    pkg-config \
    libssl-dev \
    libopus-dev \
    libavcodec-dev \
    libavutil-dev \
    libswscale-dev \
    libcjson-dev \
    libkrb5-dev \
    libpcsclite-dev \
    libfuse3-dev \
    libwebp-dev \
    libcairo2-dev \
    libswresample-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy FreeRDP3 from the builder stage
COPY --from=freerdp-builder /opt/freerdp3 /opt/freerdp3

# Set up pkg-config to find our FreeRDP3
ENV PKG_CONFIG_PATH=/opt/freerdp3/lib/pkgconfig
ENV LD_LIBRARY_PATH=/opt/freerdp3/lib

# Copy native source code
WORKDIR /build
COPY native/ ./native/

# Rebuild argument to force rebuilds when needed
ARG REBUILD_NEEDED=0

# Build the libraries against our custom FreeRDP3
WORKDIR /build/native
RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_PREFIX_PATH=/opt/freerdp3 \
    -DFREERDP3_DIR=/opt/freerdp3 \
    && cmake --build build --parallel $(nproc) \
    && cmake --install build

# Verify the libraries were built
RUN ls -la /usr/local/lib/librdp_bridge.so* \
    && ls -la /usr/local/lib/freerdp3/librdpsnd-client-bridge.so \
    && echo "Native libraries built successfully"

# ==============================================================================
# Stage 3: Runtime image with H.264-enabled FreeRDP3
# ==============================================================================
FROM ubuntu:24.04

# Install Python and runtime dependencies (NOT the system FreeRDP3 - we use our own)
# Include FFmpeg runtime for AVC444→4:2:0 transcoding with VAAPI support
# Also need X11 libs that FreeRDP3 links against even in headless mode
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 \
    python3-pip \
    python3-venv \
    libssl3 \
    libcjson1 \
    libkrb5-3 \
    libpcsclite1 \
    libfuse3-3 \
    libwebp7 \
    libopus0 \
    libavcodec60 \
    libavutil58 \
    libswscale7 \
    libswresample4 \
    libcairo2 \
    libxkbfile1 \
    libx11-6 \
    libxext6 \
    libxinerama1 \
    libxcursor1 \
    libxv1 \
    libxi6 \
    libxdamage1 \
    libxrandr2 \
    libxrender1 \
    libxfixes3 \
    libusb-1.0-0 \
    libcups2 \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Copy our custom-built FreeRDP3 with H.264 support
COPY --from=freerdp-builder /opt/freerdp3 /opt/freerdp3

# Copy the native libraries from bridge-builder
COPY --from=bridge-builder /usr/local/lib/librdp_bridge.so* /usr/local/lib/
COPY --from=bridge-builder /usr/local/include/rdp_bridge.h /usr/local/include/

# Copy the RDPSND bridge plugin to our custom FreeRDP plugin directory
RUN mkdir -p /opt/freerdp3/lib/freerdp3
COPY --from=bridge-builder /usr/local/lib/freerdp3/librdpsnd-client-bridge.so /opt/freerdp3/lib/freerdp3/

# Update library cache with custom FreeRDP3 and local lib paths
RUN echo "/opt/freerdp3/lib" > /etc/ld.so.conf.d/freerdp3.conf \
    && echo "/usr/local/lib" >> /etc/ld.so.conf.d/freerdp3.conf \
    && ldconfig

# Verify libraries are loadable
RUN python3 -c "import ctypes; ctypes.CDLL('/usr/local/lib/librdp_bridge.so'); print('Native library loaded successfully')"
RUN ls -la /opt/freerdp3/lib/freerdp3/ && echo "RDPSND bridge plugin installed"

# Set working directory
WORKDIR /app

# Create virtual environment to avoid PEP 668 issues
RUN python3 -m venv /app/venv
ENV PATH="/app/venv/bin:$PATH"

# Rebuild argument to force rebuilds when needed
ARG REBUILD_NEEDED=0

# Copy requirements first for better caching
COPY requirements.txt .

# Install Python dependencies
RUN pip install --no-cache-dir -r requirements.txt

# Copy application code
COPY *.py ./
COPY .env.example .env.example

# copy license
COPY LICENSE ./LICENSE

# Create .env if not exists
RUN cp -n .env.example .env 2>/dev/null || true

# Set library path for custom FreeRDP3 (include plugin path)
ENV LD_LIBRARY_PATH=/opt/freerdp3/lib:/usr/local/lib
ENV FREERDP_LIBRARY_PATH=/opt/freerdp3/lib/freerdp3

# Configure glibc malloc to return memory to OS more aggressively
# MALLOC_TRIM_THRESHOLD_: trim arena when this much free (default 128KB, set to 32KB)
# MALLOC_MMAP_THRESHOLD_: use mmap for allocations above this (default 128KB, set to 64KB)
# These settings help reduce memory fragmentation in long-running processes
ENV MALLOC_TRIM_THRESHOLD_=32768
ENV MALLOC_MMAP_THRESHOLD_=65536

# Expose WebSocket port
EXPOSE 8765

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD python3 -c "import socket; s=socket.socket(); s.connect(('127.0.0.1', 8765)); s.close()" || exit 1

# Run the server directly (no PulseAudio needed - native Opus audio streaming)
CMD ["python", "server.py"]
