# RDP Web Client

Browser-based Remote Desktop client using vanilla JavaScript frontend and a Python WebSocket proxy with native FreeRDP3 integration.

## Architecture

```
┌─────────────┐      WebSocket      ┌─────────────────┐      RDP       ┌──────────────┐
│   Browser   │ ◄─────────────────► │  Python Proxy   │ ◄────────────► │  Windows VM  │
│  (HTML/JS)  │   frames + input    │ (Native FreeRDP)│                │              │
└─────────────┘                     └─────────────────┘                └──────────────┘
```

### Components

- **Frontend**: Vanilla JavaScript SPA served by nginx (port 8000)
- **Backend**: Python WebSocket server with native C library for FreeRDP3 integration (port 8765)
- **Native Bridge**: C library (`librdp_bridge.so`) for direct RDP connection with zero-copy frame capture

## Features

- 🖥️ Real-time screen streaming via WebSocket (WebP encoded frames)
- ⌨️ Full keyboard support with scan code translation
- 🖱️ Mouse support (move, click, drag, wheel - horizontal & vertical)
- 📺 Fullscreen mode with dynamic resolution
- 🔄 Delta frame updates with dirty rectangle tracking
- 📊 Latency monitoring (ping/pong)
- 🩺 Health check endpoint (`/health`)
- 🐳 Docker support with multi-stage builds

## Tech Stack

### Backend
- **Python 3.x** with `websockets` for async WebSocket server
- **Native C library** built with FreeRDP3 SDK
- **PIL/Pillow** for image processing
- **Ubuntu 24.04** base image (provides FreeRDP3 runtime)

### Frontend
- **Vanilla JavaScript** (no frameworks)
- **HTML5 Canvas** for rendering RDP frames
- **nginx:alpine** for static file serving

## Quick Start with Docker (Recommended)

The easiest way to run the application is using Docker Compose:

```bash
# Build and start both services
docker-compose up -d

# View logs
docker-compose logs -f

# Stop services
docker-compose down
```

- **Frontend**: http://localhost:8000
- **Backend WebSocket**: ws://localhost:8765
- **Health Check**: http://localhost:8765/health

## Manual Setup

### Backend

The backend requires building the native C library against FreeRDP3. This is best done inside Docker.

```bash
cd backend

# Using Docker (recommended)
docker build -t rdp-backend .
docker run -p 8765:8765 rdp-backend
```

For local development without Docker (requires FreeRDP3 dev libraries):

```bash
# Install FreeRDP3 dev packages (Ubuntu 24.04+)
sudo apt install freerdp3-dev libwinpr3-dev cmake build-essential

# Build native library
cd native
cmake -B build && cmake --build build
sudo cmake --install build
sudo ldconfig

# Install Python dependencies
cd ..
pip install -r requirements.txt

# Start server
python server.py
```

### Frontend

```bash
cd frontend

# Using Docker
docker build -t rdp-frontend .
docker run -p 8000:8000 rdp-frontend

# Or using Python's built-in server
python -m http.server 8000

# Then open http://localhost:8000
```

## Usage

1. Open http://localhost:8000 in your browser
2. Click **Connect**
3. Enter VM details:
   - **Host**: IP or hostname of Windows VM
   - **Port**: RDP port (default: 3389)
   - **Username**: Windows username
   - **Password**: Windows password
4. Click **Connect** in the modal

## WebSocket Protocol

### Client → Server Messages

| Type | Description | Fields |
|------|-------------|--------|
| `connect` | Start RDP session | `host`, `port`, `username`, `password`, `width`, `height` |
| `disconnect` | End session | - |
| `mouse` | Mouse event | `action`, `x`, `y`, `button`, `deltaX`, `deltaY` |
| `key` | Keyboard event | `action`, `code`, `key` |
| `resize` | Request resolution change | `width`, `height` |
| `ping` | Latency measurement | - |

### Server → Client Messages

| Type | Description | Fields |
|------|-------------|--------|
| `connected` | Session started | `width`, `height` |
| `disconnected` | Session ended | - |
| `error` | Error occurred | `message` |
| `pong` | Ping response | - |
| Binary (WebP) | Full frame | Raw WebP image data |
| Binary (DELT) | Delta frame | Header + dirty rects + WebP patches |

## Configuration

### Backend Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `WS_HOST` | `0.0.0.0` | WebSocket bind address |
| `WS_PORT` | `8765` | WebSocket port |
| `LOG_LEVEL` | `INFO` | Logging verbosity |

### Frontend Configuration (app.js)

```javascript
const config = {
    wsUrl: 'ws://localhost:8765',      // WebSocket server URL
    mouseThrottleMs: 16,                // Mouse event throttling (~60fps)
    resizeDebounceMs: 2000,             // Resize debounce delay
};
```

## Project Structure

```
├── docker-compose.yml      # Multi-service orchestration
├── backend/
│   ├── Dockerfile          # Multi-stage build (Ubuntu 24.04)
│   ├── server.py           # WebSocket server entry point
│   ├── rdp_bridge.py       # Python wrapper for native library
│   ├── requirements.txt    # Python dependencies
│   └── native/
│       ├── CMakeLists.txt  # CMake build configuration
│       ├── rdp_bridge.c    # FreeRDP3 C implementation
│       └── rdp_bridge.h    # Library header
└── frontend/
    ├── Dockerfile          # nginx:alpine image
    ├── index.html          # SPA entry point
    ├── app.js              # RDP client logic
    └── nginx.conf          # nginx configuration
```

## Troubleshooting

### "Failed to load librdp_bridge.so"
The native library wasn't built or installed. Use Docker which handles this automatically.

### "Connection refused" to VM
- Verify the VM IP and RDP port (3389)
- Ensure Remote Desktop is enabled on the Windows VM
- Check firewall allows RDP connections

### Black screen after connecting
- The VM display may need to wake up - try moving the mouse
- Check if the VM is at a lock screen

### High latency / choppy video
- The native library uses WebP compression for bandwidth efficiency
- Delta frame updates reduce data transfer for static screens
- Check network connectivity between backend and VM

### Container health check failing
The backend exposes `/health` endpoint. Test with:
```bash
curl http://localhost:8765/health
```

