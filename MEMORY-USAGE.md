# FreeRDP-Web Backend Memory Guide

This document helps you determine how much memory to allocate for your Backend Docker container.

## Quick Answer: How Much Memory Do I Need?

| Concurrent Sessions | Recommended Memory |
|---------------------|-------------------|
| 1-5 | 128 MB |
| 10 | 256 MB |
| 25 | 550 MB |
| 50 | 1.25 GB |
| 100 | 2.5 GB |

**Rule of thumb:** `25 MB base + (20 MB × sessions)`

## Memory Breakdown

### Base Memory (Always Required)

| Component | Size |
|-----------|------|
| Python runtime | ~10 MB |
| Native library (librdp_bridge.so) | ~5 MB |
| FFmpeg/libavcodec shared libs | ~2 MB |
| One-time initialization overhead | ~6 MB |
| **Total base** | **~23 MB** |

### Per-Session Memory

Each RDP connection uses approximately **20 MB**:

| Component | Size | Notes |
|-----------|------|-------|
| FreeRDP context + GDI | ~12 MB | Wire-through mode with DeactivateClientDecoding |
| GFX event queue | ~2 MB | Encoded frames waiting for streaming |
| Audio buffers | ~450 KB | PCM + Opus ring buffers |
| Surface descriptors | ~72 KB | GFX surface metadata |
| Python overhead | ~100 KB | WebSocket, asyncio, ctypes |
| AVC444 transcoder | +5-10 MB | Only when server sends 4:4:4 H.264 |

**Measured:** 17MB after startup (no connections yet) → 40MB with 1 session → 23MB after disconnect → 40MB with 1 session reconnected → 23MB after disconnect


## Docker Configuration Examples

### Small Deployment (1-5 users)

```yaml
# docker-compose.yml
services:
  rdp-backend:
    image: rdp-backend
    deploy:
      resources:
        limits:
          memory: 128M
        reservations:
          memory: 32M
```

### Medium Deployment (10-25 users)

```yaml
services:
  rdp-backend:
    deploy:
      resources:
        limits:
          memory: 550M
        reservations:
          memory: 128M
```

### Large Deployment (50+ users)

```yaml
services:
  rdp-backend:
    deploy:
      resources:
        limits:
          memory: 1.25G
        reservations:
          memory: 512M
```

### Very Large Deployment (100+ users)

```yaml
services:
  rdp-backend:
    deploy:
      resources:
        limits:
          memory: 2.5G
        reservations:
          memory: 1G
```

## Monitoring Memory Usage

Check container memory usage:

```bash
docker stats rdp-backend
```

## Troubleshooting

### Out of Memory

If containers are being OOM-killed:
1. Increase memory limit
2. Reduce `RDP_MAX_SESSIONS_DEFAULT`
3. Check for session leaks (sessions not properly disconnected)

### High Memory Usage

If memory seems too high:
1. Check session count vs expectations
2. Look for `[GFX] Queue grown` messages (indicates heavy graphics)
3. Verify sessions are being cleaned up on disconnect

