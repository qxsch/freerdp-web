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

# Audio frame magic header (PCM - legacy)
AUDIO_FRAME_MAGIC = b'AUDI'

# Opus audio frame magic header
OPUS_AUDIO_MAGIC = b'OPUS'

# H.264 frame magic header (GFX pipeline)
H264_FRAME_MAGIC = b'H264'

# GFX codec identifiers - These are loaded from the native library at runtime
# to avoid value drift from FreeRDP's rdpgfx.h header.
# Default fallback values (will be overwritten when library loads):
RDP_GFX_CODEC_UNCOMPRESSED = 0x0000
RDP_GFX_CODEC_CAVIDEO = 0x0003
RDP_GFX_CODEC_CLEARCODEC = 0x0008
RDP_GFX_CODEC_PROGRESSIVE = 0x0009
RDP_GFX_CODEC_PLANAR = 0x000A
RDP_GFX_CODEC_AVC420 = 0x000B
RDP_GFX_CODEC_ALPHA = 0x000C
RDP_GFX_CODEC_PROGRESSIVE_V2 = 0x000D
RDP_GFX_CODEC_AVC444 = 0x000E
RDP_GFX_CODEC_AVC444v2 = 0x000F

def _load_codec_constants_from_lib(lib) -> dict:
    """Load GFX codec constants from native library to avoid drift from C headers."""
    global RDP_GFX_CODEC_UNCOMPRESSED, RDP_GFX_CODEC_CAVIDEO, RDP_GFX_CODEC_CLEARCODEC
    global RDP_GFX_CODEC_PROGRESSIVE, RDP_GFX_CODEC_PLANAR, RDP_GFX_CODEC_AVC420
    global RDP_GFX_CODEC_ALPHA, RDP_GFX_CODEC_PROGRESSIVE_V2, RDP_GFX_CODEC_AVC444
    global RDP_GFX_CODEC_AVC444v2
    
    try:
        # Set up function bindings for codec getters
        lib.rdp_gfx_codec_uncompressed.argtypes = []
        lib.rdp_gfx_codec_uncompressed.restype = c_uint16
        lib.rdp_gfx_codec_cavideo.argtypes = []
        lib.rdp_gfx_codec_cavideo.restype = c_uint16
        lib.rdp_gfx_codec_clearcodec.argtypes = []
        lib.rdp_gfx_codec_clearcodec.restype = c_uint16
        lib.rdp_gfx_codec_planar.argtypes = []
        lib.rdp_gfx_codec_planar.restype = c_uint16
        lib.rdp_gfx_codec_avc420.argtypes = []
        lib.rdp_gfx_codec_avc420.restype = c_uint16
        lib.rdp_gfx_codec_alpha.argtypes = []
        lib.rdp_gfx_codec_alpha.restype = c_uint16
        lib.rdp_gfx_codec_avc444.argtypes = []
        lib.rdp_gfx_codec_avc444.restype = c_uint16
        lib.rdp_gfx_codec_avc444v2.argtypes = []
        lib.rdp_gfx_codec_avc444v2.restype = c_uint16
        lib.rdp_gfx_codec_progressive.argtypes = []
        lib.rdp_gfx_codec_progressive.restype = c_uint16
        lib.rdp_gfx_codec_progressive_v2.argtypes = []
        lib.rdp_gfx_codec_progressive_v2.restype = c_uint16
        
        # Load the actual values from FreeRDP headers via the native library
        RDP_GFX_CODEC_UNCOMPRESSED = lib.rdp_gfx_codec_uncompressed()
        RDP_GFX_CODEC_CAVIDEO = lib.rdp_gfx_codec_cavideo()
        RDP_GFX_CODEC_CLEARCODEC = lib.rdp_gfx_codec_clearcodec()
        RDP_GFX_CODEC_PLANAR = lib.rdp_gfx_codec_planar()
        RDP_GFX_CODEC_AVC420 = lib.rdp_gfx_codec_avc420()
        RDP_GFX_CODEC_ALPHA = lib.rdp_gfx_codec_alpha()
        RDP_GFX_CODEC_AVC444 = lib.rdp_gfx_codec_avc444()
        RDP_GFX_CODEC_AVC444v2 = lib.rdp_gfx_codec_avc444v2()
        RDP_GFX_CODEC_PROGRESSIVE = lib.rdp_gfx_codec_progressive()
        RDP_GFX_CODEC_PROGRESSIVE_V2 = lib.rdp_gfx_codec_progressive_v2()
        
        logger.debug(
            f"Loaded GFX codec constants from native lib: "
            f"AVC420=0x{RDP_GFX_CODEC_AVC420:04X}, AVC444=0x{RDP_GFX_CODEC_AVC444:04X}, "
            f"ClearCodec=0x{RDP_GFX_CODEC_CLEARCODEC:04X}"
        )
        
        return {
            'UNCOMPRESSED': RDP_GFX_CODEC_UNCOMPRESSED,
            'CAVIDEO': RDP_GFX_CODEC_CAVIDEO,
            'CLEARCODEC': RDP_GFX_CODEC_CLEARCODEC,
            'PLANAR': RDP_GFX_CODEC_PLANAR,
            'AVC420': RDP_GFX_CODEC_AVC420,
            'ALPHA': RDP_GFX_CODEC_ALPHA,
            'AVC444': RDP_GFX_CODEC_AVC444,
            'AVC444v2': RDP_GFX_CODEC_AVC444v2,
            'PROGRESSIVE': RDP_GFX_CODEC_PROGRESSIVE,
            'PROGRESSIVE_V2': RDP_GFX_CODEC_PROGRESSIVE_V2,
        }
    except Exception as e:
        logger.warning(f"Failed to load codec constants from native lib: {e}, using fallback values")
        return {}

# H.264 frame types
RDP_H264_FRAME_TYPE_IDR = 0  # Keyframe
RDP_H264_FRAME_TYPE_P = 1    # Predictive
RDP_H264_FRAME_TYPE_B = 2    # Bi-predictive


class RdpRect(Structure):
    """Dirty rectangle structure (matches C struct)"""
    _fields_ = [
        ('x', c_int32),
        ('y', c_int32),
        ('width', c_int32),
        ('height', c_int32),
    ]


class RdpH264Frame(Structure):
    """H.264 frame from GFX pipeline (matches C struct)"""
    _fields_ = [
        ('frame_id', c_uint32),
        ('surface_id', c_uint16),
        ('codec_id', c_int),           # RdpGfxCodecId enum
        ('frame_type', c_int),         # RdpH264FrameType enum
        ('dest_rect', RdpRect),
        ('nal_size', c_uint32),
        ('nal_data', POINTER(c_uint8)),
        ('chroma_nal_size', c_uint32),
        ('chroma_nal_data', POINTER(c_uint8)),
        ('timestamp', ctypes.c_uint64),
        ('needs_ack', c_bool),
    ]


class RdpGfxSurface(Structure):
    """GFX surface descriptor (matches C struct)"""
    _fields_ = [
        ('surface_id', c_uint16),
        ('width', c_uint32),
        ('height', c_uint32),
        ('pixel_format', c_uint32),
        ('active', c_bool),
        ('mapped_to_output', c_bool),
        ('output_x', c_int32),
        ('output_y', c_int32),
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
                # Use RTLD_GLOBAL so symbols are visible to plugins loaded by FreeRDP
                # The rdpsnd bridge plugin uses dlsym(RTLD_DEFAULT, ...) to find
                # rdp_get_current_audio_context() exported by this library
                self._lib = ctypes.CDLL(path, mode=ctypes.RTLD_GLOBAL)
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
        
        # Opus audio API functions
        # rdp_has_opus_data
        lib.rdp_has_opus_data.argtypes = [c_void_p]
        lib.rdp_has_opus_data.restype = c_bool
        
        # rdp_get_opus_format
        lib.rdp_get_opus_format.argtypes = [
            c_void_p, POINTER(c_int), POINTER(c_int)
        ]
        lib.rdp_get_opus_format.restype = c_int
        
        # rdp_get_opus_frame
        lib.rdp_get_opus_frame.argtypes = [c_void_p, POINTER(c_uint8), c_int]
        lib.rdp_get_opus_frame.restype = c_int
        
        # GFX/H.264 API functions
        # rdp_gfx_is_active
        lib.rdp_gfx_is_active.argtypes = [c_void_p]
        lib.rdp_gfx_is_active.restype = c_bool
        
        # rdp_gfx_get_codec
        lib.rdp_gfx_get_codec.argtypes = [c_void_p]
        lib.rdp_gfx_get_codec.restype = c_int
        
        # rdp_has_h264_frames
        lib.rdp_has_h264_frames.argtypes = [c_void_p]
        lib.rdp_has_h264_frames.restype = c_int
        
        # rdp_get_h264_frame
        lib.rdp_get_h264_frame.argtypes = [c_void_p, POINTER(RdpH264Frame)]
        lib.rdp_get_h264_frame.restype = c_int
        
        # rdp_ack_h264_frame
        lib.rdp_ack_h264_frame.argtypes = [c_void_p, c_uint32]
        lib.rdp_ack_h264_frame.restype = c_int
        
        # rdp_gfx_get_surface
        lib.rdp_gfx_get_surface.argtypes = [c_void_p, c_uint16, POINTER(RdpGfxSurface)]
        lib.rdp_gfx_get_surface.restype = c_int
        
        # rdp_gfx_get_primary_surface
        lib.rdp_gfx_get_primary_surface.argtypes = [c_void_p]
        lib.rdp_gfx_get_primary_surface.restype = c_uint16
        
        # Session registry functions
        # rdp_set_max_sessions
        lib.rdp_set_max_sessions.argtypes = [c_int]
        lib.rdp_set_max_sessions.restype = c_int
        
        # rdp_get_max_sessions
        lib.rdp_get_max_sessions.argtypes = []
        lib.rdp_get_max_sessions.restype = c_int
        
        # Load GFX codec constants from native library (avoid value drift from C headers)
        _load_codec_constants_from_lib(lib)
        
        # Initialize session registry with configurable limit
        self._init_session_registry()
    
    def _init_session_registry(self):
        """Initialize the session registry with configurable limit from RDP_MAX_SESSIONS env var"""
        # Read environment variable with validation
        default_max = 100
        min_max = 2
        max_max = 1000
        
        max_sessions_str = os.environ.get('RDP_MAX_SESSIONS', '')
        if max_sessions_str:
            try:
                max_sessions = int(max_sessions_str)
                if max_sessions < min_max:
                    logger.warning(
                        f"RDP_MAX_SESSIONS={max_sessions} is below minimum {min_max}, using {min_max}"
                    )
                    max_sessions = min_max
                elif max_sessions > max_max:
                    logger.warning(
                        f"RDP_MAX_SESSIONS={max_sessions} exceeds maximum {max_max}, using {max_max}"
                    )
                    max_sessions = max_max
                else:
                    logger.info(f"RDP_MAX_SESSIONS configured: {max_sessions}")
            except ValueError:
                logger.warning(
                    f"RDP_MAX_SESSIONS='{max_sessions_str}' is not a valid integer, using default {default_max}"
                )
                max_sessions = default_max
        else:
            max_sessions = default_max
            logger.info(f"RDP_MAX_SESSIONS not set, using default: {max_sessions}")
        
        # Initialize the native session registry
        result = self._lib.rdp_set_max_sessions(max_sessions)
        if result == 0:
            actual_max = self._lib.rdp_get_max_sessions()
            logger.info(f"Session registry initialized: max_sessions={actual_max}")
        else:
            logger.error("Failed to initialize session registry")
    
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
            # Skip if dimensions haven't changed
            if self.config.width == width and self.config.height == height:
                logger.debug(f"Skipping redundant resize to {width}x{height}")
                return True
            
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
        """Stream frames from native library - prioritizes H.264/GFX when available"""
        logger.info("Starting frame streaming")
        
        # Allocate dirty rect buffer for GDI fallback
        max_rects = 64
        rects = (RdpRect * max_rects)()
        poll_count = 0
        h264_frame = RdpH264Frame()
        gfx_mode_logged = False
        h264_ever_received = False  # Once H.264 starts, skip WebP entirely
        
        while self.running:
            try:
                # Poll for events (non-blocking)
                result = await asyncio.get_event_loop().run_in_executor(
                    None, self._lib.rdp_poll, self._session, 16  # 16ms timeout
                )
                
                poll_count += 1                
                if result < 0:
                    # Error or disconnected
                    error = self._lib.rdp_get_error(self._session)
                    logger.error(f"RDP poll error: {error.decode('utf-8') if error else 'Unknown'}")
                    break
                
                # Check if GFX/H.264 pipeline is active
                gfx_active = self._lib.rdp_gfx_is_active(self._session)
                
                if gfx_active and not gfx_mode_logged:
                    codec = self._lib.rdp_gfx_get_codec(self._session)
                    codec_name = {
                        RDP_GFX_CODEC_AVC420: "AVC420",
                        RDP_GFX_CODEC_AVC444: "AVC444", 
                        RDP_GFX_CODEC_AVC444v2: "AVC444v2",
                        RDP_GFX_CODEC_PROGRESSIVE: "Progressive",
                    }.get(codec, f"Unknown({codec})")
                    logger.info(f"GFX pipeline active with codec: {codec_name}")
                    gfx_mode_logged = True
                
                # Priority 1: Stream H.264 frames from GFX pipeline
                if gfx_active:
                    h264_count = self._lib.rdp_has_h264_frames(self._session)
                    h264_sent = 0
                    while h264_count > 0:
                        ret = self._lib.rdp_get_h264_frame(
                            self._session, ctypes.byref(h264_frame)
                        )
                        if ret == 0:
                            await self._send_h264_frame(h264_frame)
                            h264_count -= 1
                            h264_sent += 1
                            h264_ever_received = True  # Mark that H.264 is now active
                        else:
                            break
                    
                    # If we sent H.264 frames, consume full frame flag but still check dirty rects
                    # (SolidFill, SurfaceToSurface, CacheToSurface operations add dirty rects
                    # that need to be sent as WebP delta - these are used for scrolling!)
                    if h264_sent > 0:
                        # Consume the full frame flag to prevent full WebP
                        self._lib.rdp_needs_full_frame(self._session)
                        # But DON'T clear dirty rects - fall through to process them!
                    
                    # GFX is active - check for dirty rects from non-H.264 operations
                    # (scrolling, fills, cache blits, etc.)
                    if result != 0 or h264_ever_received:
                        rect_count = self._lib.rdp_get_dirty_rects(
                            self._session, rects, max_rects
                        )
                        
                        if rect_count > 0:
                            # Send delta frame for non-H.264 operations
                            width = c_int()
                            height = c_int()
                            stride = c_int()
                            buffer_ptr = self._lib.rdp_get_frame_buffer(
                                self._session,
                                ctypes.byref(width),
                                ctypes.byref(height),
                                ctypes.byref(stride)
                            )
                            
                            if buffer_ptr:
                                w, h, s = width.value, height.value, stride.value
                                await self._send_delta_frame(buffer_ptr, w, h, s, rects, rect_count)
                        
                        self._lib.rdp_clear_dirty_rects(self._session)
                        
                        # If we sent H.264 and/or dirty rects, continue
                        if h264_sent > 0 or rect_count > 0:
                            continue
                    
                    # GFX active but no updates - wait for more
                    if result == 0 or h264_ever_received:
                        await asyncio.sleep(0.001)
                        continue
                
                # Fallback: GDI mode (legacy WebP encoding)
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
    
    async def _send_h264_frame(self, frame: RdpH264Frame):
        """Send H.264 frame from GFX pipeline to browser
        
        Binary format:
        [H264 magic (4)] [frame_id (4)] [surface_id (2)] [codec_id (2)]
        [frame_type (1)] [x (2)] [y (2)] [w (2)] [h (2)]
        [nal_size (4)] [chroma_nal_size (4)]
        [nal_data...] [chroma_nal_data...]
        """
        try:
            if not frame.nal_data or frame.nal_size == 0:
                return
            
            # Read NAL data from native buffer
            nal_data = ctypes.string_at(frame.nal_data, frame.nal_size)
            
            # Read chroma NAL data for AVC444
            chroma_data = b''
            if frame.chroma_nal_data and frame.chroma_nal_size > 0:
                chroma_data = ctypes.string_at(frame.chroma_nal_data, frame.chroma_nal_size)
            
            # Build binary message
            message = io.BytesIO()
            message.write(H264_FRAME_MAGIC)  # 4 bytes
            message.write(struct.pack('<I', frame.frame_id))  # 4 bytes
            message.write(struct.pack('<H', frame.surface_id))  # 2 bytes
            message.write(struct.pack('<H', frame.codec_id))  # 2 bytes
            message.write(struct.pack('<B', frame.frame_type))  # 1 byte
            message.write(struct.pack('<h', frame.dest_rect.x))  # 2 bytes
            message.write(struct.pack('<h', frame.dest_rect.y))  # 2 bytes
            message.write(struct.pack('<H', frame.dest_rect.width))  # 2 bytes
            message.write(struct.pack('<H', frame.dest_rect.height))  # 2 bytes
            message.write(struct.pack('<I', frame.nal_size))  # 4 bytes
            message.write(struct.pack('<I', len(chroma_data)))  # 4 bytes
            message.write(nal_data)
            if chroma_data:
                message.write(chroma_data)
            
            await self.websocket.send(message.getvalue())
            self._frame_count += 1
            
            # Log first few H.264 frames for debugging
            if self._frame_count <= 5:
                codec_name = {
                    RDP_GFX_CODEC_AVC420: "AVC420",
                    RDP_GFX_CODEC_AVC444: "AVC444",
                    RDP_GFX_CODEC_AVC444v2: "AVC444v2",
                }.get(frame.codec_id, f"0x{frame.codec_id:X}")
                frame_type = {0: "IDR", 1: "P", 2: "B"}.get(frame.frame_type, "?")
                logger.info(
                    f"H.264 frame #{self._frame_count}: {codec_name} {frame_type} "
                    f"{frame.dest_rect.width}x{frame.dest_rect.height} "
                    f"NAL:{frame.nal_size}b chroma:{len(chroma_data)}b"
                )
                
        except Exception as e:
            logger.error(f"H.264 frame send error: {e}")
    
    async def ack_h264_frame(self, frame_id: int):
        """Acknowledge an H.264 frame to prevent server back-pressure
        
        NOTE: Currently disabled because FreeRDP's GFX channel automatically
        sends frame acknowledgments in its EndFrame handler. Sending additional
        acks from the browser causes protocol errors (duplicate acks).
        
        To enable browser-controlled acks, we would need to:
        1. Set OnOpen callback with *do_frame_acks = FALSE
        2. Manually track which frames need acking
        3. Send acks only after browser confirms decode
        """
        # Disabled: FreeRDP handles acks automatically in EndFrame
        # if self._session and self._lib:
        #     await asyncio.get_event_loop().run_in_executor(
        #         None, self._lib.rdp_ack_h264_frame, self._session, frame_id
        #     )
        pass
    
    async def _stream_audio(self):
        """Stream Opus audio from native FreeRDP buffer (no PulseAudio required)"""
        logger.info("Starting native Opus audio streaming")
        
        # Opus frame buffer (max ~4KB per frame)
        opus_buffer_size = 4096
        opus_buffer = (c_uint8 * opus_buffer_size)()
        
        # Track format for header
        sample_rate = c_int()
        channels = c_int()
        last_sample_rate = 48000
        last_channels = 2
        
        frames_sent = 0
        last_frame_time = 0
        
        # Target ~20ms between sends (Opus frame duration)
        target_interval = 0.018  # slightly less than 20ms to avoid underruns
        
        while self.running:
            try:
                # Check if Opus data is available
                if not self._lib.rdp_has_opus_data(self._session):
                    await asyncio.sleep(0.002)  # 2ms polling when idle
                    continue
                
                # Get audio format
                if self._lib.rdp_get_opus_format(
                    self._session,
                    ctypes.byref(sample_rate),
                    ctypes.byref(channels)
                ) == 0:
                    last_sample_rate = sample_rate.value
                    last_channels = channels.value
                
                # Send multiple frames if they've accumulated
                frames_this_batch = 0
                max_frames_per_batch = 5  # Limit to avoid large bursts
                
                while self._lib.rdp_has_opus_data(self._session) and frames_this_batch < max_frames_per_batch:
                    # Read Opus frame from native buffer
                    frame_size = self._lib.rdp_get_opus_frame(
                        self._session,
                        opus_buffer,
                        opus_buffer_size
                    )
                    
                    if frame_size > 0:
                        # Build Opus audio message:
                        # [OPUS magic (4)] [sample_rate (4)] [channels (2)] [frame_size (2)] [Opus data...]
                        message = io.BytesIO()
                        message.write(OPUS_AUDIO_MAGIC)
                        message.write(struct.pack('<I', last_sample_rate))
                        message.write(struct.pack('<H', last_channels))
                        message.write(struct.pack('<H', frame_size))
                        message.write(ctypes.string_at(opus_buffer, frame_size))
                        
                        await self.websocket.send(message.getvalue())
                        frames_sent += 1
                        frames_this_batch += 1
                        
                        if frames_sent <= 5:
                            logger.debug(f"Sent Opus frame #{frames_sent}: {frame_size} bytes, "
                                       f"{last_sample_rate}Hz, {last_channels}ch")
                    elif frame_size == -2:
                        logger.warning("Opus frame buffer too small")
                        break
                    else:
                        break
                
                # Small delay to prevent tight loop and allow batching
                await asyncio.sleep(0.001)
                
            except asyncio.CancelledError:
                break
            except Exception as e:
                logger.error(f"Native audio streaming error: {e}")
                await asyncio.sleep(0.1)
        
        logger.info(f"Native audio streaming ended after {frames_sent} frames")
    
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
