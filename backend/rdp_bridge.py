"""
RDP Bridge Module - Native FreeRDP3 Integration

Uses a native C library (librdp_bridge.so) for direct RDP connection with:
- Zero-copy frame capture via GDI surface
- Dirty rectangle tracking for delta updates
- Direct input injection (no X11/xdotool)
- WebP encoding for efficient bandwidth usage
"""

import asyncio
import ctypes
import io
import logging
import os
import struct
from ctypes import (
    POINTER, Structure, c_bool, c_char_p, c_int, c_int32, c_uint8,
    c_uint16, c_uint32, c_void_p
)
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from PIL import Image

logger = logging.getLogger('rdp-bridge')


@dataclass
class RDPConfig:
    """RDP connection configuration"""
    host: str
    port: int = 3389
    username: str = ''
    password: str = ''
    domain: str = ''
    width: int = 1280
    height: int = 720
    color_depth: int = 32


# Mouse button flags (matching native library)
RDP_MOUSE_FLAG_MOVE = 0x0800
RDP_MOUSE_FLAG_BUTTON1 = 0x1000  # Left
RDP_MOUSE_FLAG_BUTTON2 = 0x2000  # Right
RDP_MOUSE_FLAG_BUTTON3 = 0x4000  # Middle
RDP_MOUSE_FLAG_DOWN = 0x8000
RDP_MOUSE_FLAG_WHEEL = 0x0200
RDP_MOUSE_FLAG_HWHEEL = 0x0400
RDP_MOUSE_FLAG_NEGATIVE = 0x0100

# Keyboard flags
RDP_KBD_FLAG_DOWN = 0x0000
RDP_KBD_FLAG_RELEASE = 0x8000
RDP_KBD_FLAG_EXTENDED = 0x0100

# Session states
RDP_STATE_DISCONNECTED = 0
RDP_STATE_CONNECTING = 1
RDP_STATE_CONNECTED = 2
RDP_STATE_ERROR = 3

# Delta frame magic header
DELTA_FRAME_MAGIC = b'DELT'

# Audio frame magic header
AUDIO_FRAME_MAGIC = b'AUDI'


class RdpRect(Structure):
    """Dirty rectangle structure (matches C struct)"""
    _fields_ = [
        ('x', c_int32),
        ('y', c_int32),
        ('width', c_int32),
        ('height', c_int32),
    ]


# Keyboard scan code mapping (JS code -> RDP scan code)
SCANCODE_MAP = {
    'Escape': 0x01, 'Digit1': 0x02, 'Digit2': 0x03, 'Digit3': 0x04,
    'Digit4': 0x05, 'Digit5': 0x06, 'Digit6': 0x07, 'Digit7': 0x08,
    'Digit8': 0x09, 'Digit9': 0x0A, 'Digit0': 0x0B, 'Minus': 0x0C,
    'Equal': 0x0D, 'Backspace': 0x0E, 'Tab': 0x0F, 'KeyQ': 0x10,
    'KeyW': 0x11, 'KeyE': 0x12, 'KeyR': 0x13, 'KeyT': 0x14,
    'KeyY': 0x15, 'KeyU': 0x16, 'KeyI': 0x17, 'KeyO': 0x18,
    'KeyP': 0x19, 'BracketLeft': 0x1A, 'BracketRight': 0x1B,
    'Enter': 0x1C, 'ControlLeft': 0x1D, 'KeyA': 0x1E, 'KeyS': 0x1F,
    'KeyD': 0x20, 'KeyF': 0x21, 'KeyG': 0x22, 'KeyH': 0x23,
    'KeyJ': 0x24, 'KeyK': 0x25, 'KeyL': 0x26, 'Semicolon': 0x27,
    'Quote': 0x28, 'Backquote': 0x29, 'ShiftLeft': 0x2A,
    'Backslash': 0x2B, 'KeyZ': 0x2C, 'KeyX': 0x2D, 'KeyC': 0x2E,
    'KeyV': 0x2F, 'KeyB': 0x30, 'KeyN': 0x31, 'KeyM': 0x32,
    'Comma': 0x33, 'Period': 0x34, 'Slash': 0x35, 'ShiftRight': 0x36,
    'NumpadMultiply': 0x37, 'AltLeft': 0x38, 'Space': 0x39,
    'CapsLock': 0x3A, 'F1': 0x3B, 'F2': 0x3C, 'F3': 0x3D,
    'F4': 0x3E, 'F5': 0x3F, 'F6': 0x40, 'F7': 0x41, 'F8': 0x42,
    'F9': 0x43, 'F10': 0x44, 'NumLock': 0x45, 'ScrollLock': 0x46,
    'Numpad7': 0x47, 'Numpad8': 0x48, 'Numpad9': 0x49,
    'NumpadSubtract': 0x4A, 'Numpad4': 0x4B, 'Numpad5': 0x4C,
    'Numpad6': 0x4D, 'NumpadAdd': 0x4E, 'Numpad1': 0x4F,
    'Numpad2': 0x50, 'Numpad3': 0x51, 'Numpad0': 0x52,
    'NumpadDecimal': 0x53, 'F11': 0x57, 'F12': 0x58,
    # Extended keys (need EXTENDED flag)
    'NumpadEnter': 0x1C, 'ControlRight': 0x1D, 'NumpadDivide': 0x35,
    'PrintScreen': 0x37, 'AltRight': 0x38, 'Home': 0x47,
    'ArrowUp': 0x48, 'PageUp': 0x49, 'ArrowLeft': 0x4B,
    'ArrowRight': 0x4D, 'End': 0x4F, 'ArrowDown': 0x50,
    'PageDown': 0x51, 'Insert': 0x52, 'Delete': 0x53,
    'MetaLeft': 0x5B, 'MetaRight': 0x5C, 'ContextMenu': 0x5D,
}

# Extended keys that need the EXTENDED flag
EXTENDED_KEYS = {
    'NumpadEnter', 'ControlRight', 'NumpadDivide', 'PrintScreen',
    'AltRight', 'Home', 'ArrowUp', 'PageUp', 'ArrowLeft',
    'ArrowRight', 'End', 'ArrowDown', 'PageDown', 'Insert', 'Delete',
    'MetaLeft', 'MetaRight', 'ContextMenu',
}


class NativeLibrary:
    """Wrapper for the native RDP bridge library"""
    
    _instance: Optional['NativeLibrary'] = None
    _lib: Optional[ctypes.CDLL] = None
    
    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._load_library()
        return cls._instance
    
    def _load_library(self):
        """Load the native library and set up function signatures"""
        lib_paths = [
            'librdp_bridge.so',
            '/usr/local/lib/librdp_bridge.so',
            str(Path(__file__).parent / 'libs' / 'librdp_bridge.so'),
        ]
        
        for path in lib_paths:
            try:
                self._lib = ctypes.CDLL(path)
                logger.info(f"Loaded native library from: {path}")
                break
            except OSError:
                continue
        
        if self._lib is None:
            raise RuntimeError(
                "Failed to load librdp_bridge.so. "
                "Ensure the native library is built and installed."
            )
        
        self._setup_functions()
    
    def _setup_functions(self):
        """Define function signatures for type safety"""
        lib = self._lib
        
        # rdp_create
        lib.rdp_create.argtypes = [
            c_char_p, c_uint16, c_char_p, c_char_p, c_char_p,
            c_uint32, c_uint32, c_uint32
        ]
        lib.rdp_create.restype = c_void_p
        
        # rdp_connect
        lib.rdp_connect.argtypes = [c_void_p]
        lib.rdp_connect.restype = c_int
        
        # rdp_get_state
        lib.rdp_get_state.argtypes = [c_void_p]
        lib.rdp_get_state.restype = c_int
        
        # rdp_get_error
        lib.rdp_get_error.argtypes = [c_void_p]
        lib.rdp_get_error.restype = c_char_p
        
        # rdp_poll
        lib.rdp_poll.argtypes = [c_void_p, c_int]
        lib.rdp_poll.restype = c_int
        
        # rdp_get_frame_buffer
        lib.rdp_get_frame_buffer.argtypes = [
            c_void_p, POINTER(c_int), POINTER(c_int), POINTER(c_int)
        ]
        lib.rdp_get_frame_buffer.restype = POINTER(c_uint8)
        
        # rdp_get_dirty_rects
        lib.rdp_get_dirty_rects.argtypes = [c_void_p, POINTER(RdpRect), c_int]
        lib.rdp_get_dirty_rects.restype = c_int
        
        # rdp_clear_dirty_rects
        lib.rdp_clear_dirty_rects.argtypes = [c_void_p]
        lib.rdp_clear_dirty_rects.restype = None
        
        # rdp_needs_full_frame
        lib.rdp_needs_full_frame.argtypes = [c_void_p]
        lib.rdp_needs_full_frame.restype = c_bool
        
        # rdp_send_mouse
        lib.rdp_send_mouse.argtypes = [c_void_p, c_uint16, c_int, c_int]
        lib.rdp_send_mouse.restype = None
        
        # rdp_send_keyboard
        lib.rdp_send_keyboard.argtypes = [c_void_p, c_uint16, c_uint16]
        lib.rdp_send_keyboard.restype = None
        
        # rdp_send_unicode
        lib.rdp_send_unicode.argtypes = [c_void_p, c_uint16, c_uint16]
        lib.rdp_send_unicode.restype = None
        
        # rdp_resize
        lib.rdp_resize.argtypes = [c_void_p, c_uint32, c_uint32]
        lib.rdp_resize.restype = c_int
        
        # rdp_disconnect
        lib.rdp_disconnect.argtypes = [c_void_p]
        lib.rdp_disconnect.restype = None
        
        # rdp_destroy
        lib.rdp_destroy.argtypes = [c_void_p]
        lib.rdp_destroy.restype = None
        
        # rdp_version
        lib.rdp_version.argtypes = []
        lib.rdp_version.restype = c_char_p
        
        # rdp_has_audio_data
        lib.rdp_has_audio_data.argtypes = [c_void_p]
        lib.rdp_has_audio_data.restype = c_bool
        
        # rdp_get_audio_format
        lib.rdp_get_audio_format.argtypes = [
            c_void_p, POINTER(c_int), POINTER(c_int), POINTER(c_int)
        ]
        lib.rdp_get_audio_format.restype = c_int
        
        # rdp_get_audio_data
        lib.rdp_get_audio_data.argtypes = [c_void_p, POINTER(c_uint8), c_int]
        lib.rdp_get_audio_data.restype = c_int
    
    def __getattr__(self, name):
        """Proxy attribute access to the underlying library"""
        return getattr(self._lib, name)


class RDPBridge:
    """
    Bridge between WebSocket client and RDP session using native FreeRDP3.
    
    Features:
    - Direct frame capture from GDI surface (no Xvfb/screenshots)
    - Dirty rectangle tracking for efficient delta updates
    - WebP encoding for optimal compression
    - Direct input injection (no xdotool)
    """
    
    def __init__(self, config: RDPConfig, websocket):
        self.config = config
        self.websocket = websocket
        self._session: Optional[c_void_p] = None
        self._lib: Optional[NativeLibrary] = None
        self.running = False
        self._frame_task: Optional[asyncio.Task] = None
        self._audio_task: Optional[asyncio.Task] = None
        self._frame_count = 0
        
        # Frame rate control
        self._target_fps = 60
        self._frame_interval = 1.0 / self._target_fps
        
        # WebP encoding settings
        self._webp_quality = 80  # 0-100, higher = better quality
        self._webp_method = 0   # 0-6, 0 = fastest
        
        # Audio settings
        self._audio_enabled = True
        self._audio_buffer_size = 8192  # PCM buffer size for reading
    
    async def connect(self) -> bool:
        """Connect to the RDP server"""
        try:
            # Load native library
            try:
                self._lib = NativeLibrary()
                version = self._lib.rdp_version().decode('utf-8')
                logger.info(f"Native RDP bridge version: {version}")
            except RuntimeError as e:
                logger.error(f"Failed to load native library: {e}")
                return False
            
            # Create session
            self._session = self._lib.rdp_create(
                self.config.host.encode('utf-8'),
                self.config.port,
                self.config.username.encode('utf-8') if self.config.username else b'',
                self.config.password.encode('utf-8') if self.config.password else b'',
                self.config.domain.encode('utf-8') if self.config.domain else b'',
                self.config.width,
                self.config.height,
                self.config.color_depth
            )
            
            if not self._session:
                logger.error("Failed to create RDP session")
                return False
            
            # Connect (this may block briefly)
            logger.info(f"Connecting to {self.config.host}:{self.config.port}...")
            result = await asyncio.get_event_loop().run_in_executor(
                None, self._lib.rdp_connect, self._session
            )
            
            if result != 0:
                error = self._lib.rdp_get_error(self._session)
                error_msg = error.decode('utf-8') if error else 'Unknown error'
                logger.error(f"RDP connection failed: {error_msg}")
                self._lib.rdp_destroy(self._session)
                self._session = None
                return False
            
            logger.info("RDP connection established")
            self.running = True
            
            # Start frame streaming
            self._frame_task = asyncio.create_task(self._stream_frames())
            
            # Start audio streaming
            if self._audio_enabled:
                self._audio_task = asyncio.create_task(self._stream_audio())
            
            return True
            
        except Exception as e:
            logger.error(f"Connection error: {e}")
            return False
    
    async def resize(self, width: int, height: int) -> bool:
        """Resize the RDP session"""
        try:
            logger.info(f"Resizing session to {width}x{height}")
            
            self.config.width = width
            self.config.height = height
            
            if not self._session or not self._lib:
                return False
            
            result = self._lib.rdp_resize(self._session, width, height)
            return result == 0
            
        except Exception as e:
            logger.error(f"Resize error: {e}")
            return False
    
    async def _stream_frames(self):
        """Stream frames from native library with delta updates"""
        logger.info("Starting frame streaming")
        
        # Allocate dirty rect buffer
        max_rects = 64
        rects = (RdpRect * max_rects)()
        poll_count = 0
        
        while self.running:
            try:
                # Poll for events (non-blocking)
                result = await asyncio.get_event_loop().run_in_executor(
                    None, self._lib.rdp_poll, self._session, 16  # 16ms timeout
                )
                
                poll_count += 1
                if poll_count <= 5 or poll_count % 100 == 0:
                    logger.debug(f"Poll #{poll_count}: result={result}")
                
                if result < 0:
                    # Error or disconnected
                    error = self._lib.rdp_get_error(self._session)
                    logger.error(f"RDP poll error: {error.decode('utf-8') if error else 'Unknown'}")
                    break
                
                if result == 0:
                    # No frame update, but still connected
                    await asyncio.sleep(0.001)
                    continue
                
                # Check if we need a full frame
                needs_full = self._lib.rdp_needs_full_frame(self._session)
                if poll_count <= 10:
                    logger.debug(f"Frame update available: needs_full={needs_full}")
                
                # Get frame buffer
                width = c_int()
                height = c_int()
                stride = c_int()
                buffer_ptr = self._lib.rdp_get_frame_buffer(
                    self._session,
                    ctypes.byref(width),
                    ctypes.byref(height),
                    ctypes.byref(stride)
                )
                
                if not buffer_ptr:
                    logger.warning("Frame buffer not available")
                    continue
                
                w, h, s = width.value, height.value, stride.value
                
                if needs_full:
                    # Send full frame
                    logger.info(f"Sending full frame: {w}x{h}, stride={s}")
                    await self._send_full_frame(buffer_ptr, w, h, s)
                else:
                    # Get dirty rects and send delta
                    rect_count = self._lib.rdp_get_dirty_rects(
                        self._session, rects, max_rects
                    )
                    
                    if rect_count > 0:
                        await self._send_delta_frame(buffer_ptr, w, h, s, rects, rect_count)
                
                # Clear dirty rects after processing
                self._lib.rdp_clear_dirty_rects(self._session)
                self._frame_count += 1
                
            except asyncio.CancelledError:
                break
            except Exception as e:
                logger.error(f"Frame streaming error: {e}")
                await asyncio.sleep(0.1)
        
        logger.info(f"Frame streaming ended after {self._frame_count} frames")
    
    async def _send_full_frame(self, buffer_ptr, width: int, height: int, stride: int):
        """Encode and send a full frame as WebP"""
        try:
            # Read pixel data from native buffer
            buffer_size = height * stride
            pixel_data = ctypes.string_at(buffer_ptr, buffer_size)
            
            # Create PIL image from BGRA data
            img = Image.frombytes('RGBA', (width, height), pixel_data, 'raw', 'BGRA', stride)
            
            # Convert to RGB and encode as WebP
            img_rgb = img.convert('RGB')
            buffer = io.BytesIO()
            img_rgb.save(
                buffer,
                format='WEBP',
                quality=self._webp_quality,
                method=self._webp_method
            )
            
            # Send as binary (full frame = raw WebP)
            await self.websocket.send(buffer.getvalue())
            
        except Exception as e:
            logger.error(f"Full frame encoding error: {e}")
    
    async def _send_delta_frame(
        self, buffer_ptr, width: int, height: int, stride: int,
        rects: 'ctypes.Array[RdpRect]', rect_count: int
    ):
        """Encode and send delta frame with dirty rectangles"""
        try:
            # Read full pixel buffer
            buffer_size = height * stride
            pixel_data = ctypes.string_at(buffer_ptr, buffer_size)
            
            # Build delta message
            rect_list = []
            tile_data = io.BytesIO()
            
            for i in range(rect_count):
                rect = rects[i]
                rx, ry, rw, rh = rect.x, rect.y, rect.width, rect.height
                
                # Clamp to frame bounds
                if rx < 0: rx = 0
                if ry < 0: ry = 0
                if rx + rw > width: rw = width - rx
                if ry + rh > height: rh = height - ry
                if rw <= 0 or rh <= 0:
                    continue
                
                rect_list.append({
                    'x': rx, 'y': ry, 'w': rw, 'h': rh
                })
                
                # Extract tile from full buffer
                # BGRA, 4 bytes per pixel
                tile_bytes = bytearray(rw * rh * 4)
                for row in range(rh):
                    src_offset = (ry + row) * stride + rx * 4
                    dst_offset = row * rw * 4
                    tile_bytes[dst_offset:dst_offset + rw * 4] = \
                        pixel_data[src_offset:src_offset + rw * 4]
                
                # Create tile image and encode to WebP
                tile_img = Image.frombytes('RGBA', (rw, rh), bytes(tile_bytes), 'raw', 'BGRA')
                tile_rgb = tile_img.convert('RGB')
                tile_buffer = io.BytesIO()
                tile_rgb.save(
                    tile_buffer,
                    format='WEBP',
                    quality=self._webp_quality,
                    method=self._webp_method
                )
                
                # Store size and data
                tile_webp = tile_buffer.getvalue()
                rect_list[-1]['size'] = len(tile_webp)
                tile_data.write(tile_webp)
            
            if not rect_list:
                return
            
            # Build delta message:
            # [DELT magic (4)] [JSON length (4)] [JSON] [tile data...]
            import json
            rect_json = json.dumps({'rects': rect_list}).encode('utf-8')
            
            message = io.BytesIO()
            message.write(DELTA_FRAME_MAGIC)
            message.write(struct.pack('<I', len(rect_json)))
            message.write(rect_json)
            message.write(tile_data.getvalue())
            
            await self.websocket.send(message.getvalue())
            
        except Exception as e:
            logger.error(f"Delta frame encoding error: {e}")
    
    async def _stream_audio(self):
        """Stream audio by capturing from PulseAudio's null sink monitor"""
        logger.info("Starting audio streaming from PulseAudio monitor")
        
        # Audio format settings
        sample_rate = 48000
        channels = 2
        bits = 16
        
        # Frame size: 50ms of audio at 48kHz stereo 16-bit = 9600 bytes
        # Larger frames = fewer packets = smoother playback
        frame_size = int(sample_rate * channels * (bits // 8) * 0.05)
        
        try:
            # Use parec to capture from the null sink's monitor
            # The monitor source is named "virtual_sink.monitor"
            process = await asyncio.create_subprocess_exec(
                'parec',
                '--rate', str(sample_rate),
                '--channels', str(channels),
                '--format', 's16le',
                '--device', 'virtual_sink.monitor',
                '--raw',
                '--latency-msec', '50',
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL
            )
            
            logger.info("parec process started for audio capture")
            
            while self.running and process.returncode is None:
                try:
                    # Read a frame of audio data
                    audio_data = await asyncio.wait_for(
                        process.stdout.read(frame_size),
                        timeout=0.2
                    )
                    
                    if audio_data and len(audio_data) > 0:
                        # Quick silence check using sum of absolute values
                        # This is faster than iterating byte by byte
                        is_silent = sum(audio_data[:200:2]) == 0
                        
                        if not is_silent:
                            # Send audio packet
                            # Format: [AUDI magic (4)] [sample_rate (4)] [channels (2)] [bits (2)] [PCM data...]
                            message = io.BytesIO()
                            message.write(AUDIO_FRAME_MAGIC)
                            message.write(struct.pack('<I', sample_rate))
                            message.write(struct.pack('<H', channels))
                            message.write(struct.pack('<H', bits))
                            message.write(audio_data)
                            
                            await self.websocket.send(message.getvalue())
                    
                except asyncio.TimeoutError:
                    continue
                except asyncio.CancelledError:
                    break
                except Exception as e:
                    logger.error(f"Audio read error: {e}")
                    await asyncio.sleep(0.1)
            
            # Clean up process
            if process.returncode is None:
                process.terminate()
                await process.wait()
                
        except FileNotFoundError:
            logger.warning("parec not found - audio capture disabled")
        except Exception as e:
            logger.error(f"Audio streaming error: {e}")
        
        logger.info("Audio streaming ended")
    
    async def disconnect(self):
        """Disconnect from the RDP server"""
        self.running = False
        
        if self._frame_task:
            self._frame_task.cancel()
            try:
                await self._frame_task
            except asyncio.CancelledError:
                pass
        
        if self._audio_task:
            self._audio_task.cancel()
            try:
                await self._audio_task
            except asyncio.CancelledError:
                pass
        
        if self._session and self._lib:
            self._lib.rdp_disconnect(self._session)
            self._lib.rdp_destroy(self._session)
            self._session = None
        
        logger.info("RDP session disconnected")
    
    async def send_mouse_event(
        self, action: str, x: int, y: int,
        button: int = 0, delta_x: int = 0, delta_y: int = 0
    ):
        """Send mouse event to VM"""
        if not self.running:
            return
        
        if not self._session or not self._lib:
            return
        
        try:
            flags = 0
            
            if action == 'move':
                flags = RDP_MOUSE_FLAG_MOVE
            elif action == 'down':
                # Map button index to flag
                button_map = {
                    0: RDP_MOUSE_FLAG_BUTTON1,  # Left
                    1: RDP_MOUSE_FLAG_BUTTON3,  # Middle (button index 1 in JS)
                    2: RDP_MOUSE_FLAG_BUTTON2,  # Right
                }
                flags = button_map.get(button, RDP_MOUSE_FLAG_BUTTON1) | RDP_MOUSE_FLAG_DOWN
            elif action == 'up':
                button_map = {
                    0: RDP_MOUSE_FLAG_BUTTON1,
                    1: RDP_MOUSE_FLAG_BUTTON3,
                    2: RDP_MOUSE_FLAG_BUTTON2,
                }
                flags = button_map.get(button, RDP_MOUSE_FLAG_BUTTON1)
            elif action == 'wheel':
                # Wheel delta: positive deltaY = scroll down, negative = scroll up
                delta = int(delta_y) if delta_y else 0
                if delta == 0:
                    return  # No wheel movement, skip
                
                # RDP wheel encoding:
                # - Lower 9 bits (0x01FF) = wheel rotation mask
                # - Bit 8 (0x0100) = negative flag (scroll up)
                # - Bits 0-7 = absolute rotation value (clamped to 0-255)
                # Standard wheel click = 120 units
                # Browser: positive deltaY = scroll down, negative = scroll up
                # RDP: negative flag = scroll UP (opposite direction)
                rotation = min(255, abs(delta))
                flags = RDP_MOUSE_FLAG_WHEEL
                if delta > 0:
                    # Scroll down in browser = scroll up in RDP (inverted)
                    flags |= RDP_MOUSE_FLAG_NEGATIVE
                # Encode rotation in lower bits (0xFF mask, not 0x1FF since bit 8 is negative flag)
                flags |= (rotation & 0xFF)
            
            self._lib.rdp_send_mouse(self._session, flags, x, y)
            
        except Exception as e:
            logger.error(f"Mouse event error: {e}")
    
    async def send_key_event(
        self, action: str, key: str, code: str,
        key_code: int = 0, ctrl: bool = False,
        shift: bool = False, alt: bool = False,
        meta: bool = False
    ):
        """Send keyboard event to VM"""
        if not self.running:
            return
        
        if not self._session or not self._lib:
            return
        
        try:
            # Get scancode from mapping
            scancode = SCANCODE_MAP.get(code)
            
            if scancode is None:
                logger.debug(f"Unknown key code: {code}")
                return
            
            # Build flags
            flags = RDP_KBD_FLAG_DOWN if action == 'down' else RDP_KBD_FLAG_RELEASE
            
            # Check if extended key
            if code in EXTENDED_KEYS:
                flags |= RDP_KBD_FLAG_EXTENDED
            
            self._lib.rdp_send_keyboard(self._session, flags, scancode)
            
        except Exception as e:
            logger.error(f"Key event error: {e}")
    
    async def send_key_combo(self, combo: str):
        """Send a key combination like Ctrl+Alt+Delete"""
        if not self.running:
            return
        
        logger.info(f"Sending key combo: {combo}")
        
        keys = combo.split('+')
        
        # Press all keys
        for key in keys:
            code = self._key_to_code(key)
            await self.send_key_event('down', key, code)
            await asyncio.sleep(0.05)
        
        # Release all keys in reverse
        for key in reversed(keys):
            code = self._key_to_code(key)
            await self.send_key_event('up', key, code)
            await asyncio.sleep(0.05)
    
    def _key_to_code(self, key: str) -> str:
        """Convert key name to JS code"""
        key_map = {
            'Ctrl': 'ControlLeft',
            'Control': 'ControlLeft',
            'Alt': 'AltLeft',
            'Shift': 'ShiftLeft',
            'Meta': 'MetaLeft',
            'Win': 'MetaLeft',
            'Delete': 'Delete',
            'Enter': 'Enter',
            'Tab': 'Tab',
            'Escape': 'Escape',
            'Esc': 'Escape',
        }
        return key_map.get(key, f'Key{key.upper()}')
