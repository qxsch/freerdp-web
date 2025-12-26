/**
 * RDP Bridge Native Library Header
 * 
 * Direct FreeRDP3 integration for low-latency RDP streaming.
 * Provides frame capture via GDI surface and direct input injection.
 */

#ifndef RDP_BRIDGE_H
#define RDP_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum dirty rectangles per frame.
 * Increased from 64 to 512 to handle complex repaint scenarios like
 * window de-maximize, where many small tiles need updating.
 * When limit is exceeded, we fall back to full frame update. */
#define RDP_MAX_DIRTY_RECTS 512

/* GFX pipeline constants */
#define RDP_MAX_GFX_SURFACES 256
#define RDP_MAX_GFX_CACHE_SLOTS 4096  /* Max bitmap cache slots for GFX */
#define RDP_MAX_H264_FRAMES 16
#define RDP_H264_FRAME_BUFFER_SIZE (2 * 1024 * 1024)  /* 2MB per frame max */

/* Session registry limits (compile-time defaults, runtime configurable) */
#define RDP_MAX_SESSIONS_DEFAULT 100
#define RDP_MAX_SESSIONS_MIN 2
#define RDP_MAX_SESSIONS_MAX 1000

/* Mouse button flags (compatible with FreeRDP PTR_FLAGS_*) */
#define RDP_MOUSE_FLAG_MOVE     0x0800
#define RDP_MOUSE_FLAG_BUTTON1  0x1000  /* Left */
#define RDP_MOUSE_FLAG_BUTTON2  0x2000  /* Right */
#define RDP_MOUSE_FLAG_BUTTON3  0x4000  /* Middle */
#define RDP_MOUSE_FLAG_DOWN     0x8000
#define RDP_MOUSE_FLAG_WHEEL    0x0200
#define RDP_MOUSE_FLAG_HWHEEL   0x0400
#define RDP_MOUSE_FLAG_NEGATIVE 0x0100

/* Keyboard flags */
#define RDP_KBD_FLAG_DOWN       0x0000
#define RDP_KBD_FLAG_RELEASE    0x8000
#define RDP_KBD_FLAG_EXTENDED   0x0100
#define RDP_KBD_FLAG_EXTENDED1  0x0200

/* Session states */
typedef enum {
    RDP_STATE_DISCONNECTED = 0,
    RDP_STATE_CONNECTING,
    RDP_STATE_CONNECTED,
    RDP_STATE_ERROR
} RdpState;

/* Dirty rectangle structure */
typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} RdpRect;

/* GFX codec identifiers (from MS-RDPEGFX) */
typedef enum {
    RDP_GFX_CODEC_UNCOMPRESSED = 0x0000,
    RDP_GFX_CODEC_CLEARCODEC = 0x0003,
    RDP_GFX_CODEC_PLANAR = 0x0004,
    RDP_GFX_CODEC_AVC420 = 0x0009,
    RDP_GFX_CODEC_ALPHA = 0x000A,
    RDP_GFX_CODEC_AVC444 = 0x000B,
    RDP_GFX_CODEC_AVC444v2 = 0x000E,
    RDP_GFX_CODEC_PROGRESSIVE = 0x000C,
    RDP_GFX_CODEC_PROGRESSIVE_V2 = 0x000D
} RdpGfxCodecId;

/* H.264 frame type */
typedef enum {
    RDP_H264_FRAME_TYPE_IDR = 0,  /* Keyframe (I-frame) */
    RDP_H264_FRAME_TYPE_P = 1,    /* Predictive frame */
    RDP_H264_FRAME_TYPE_B = 2     /* Bi-predictive frame */
} RdpH264FrameType;

/* H.264 frame from GFX pipeline */
typedef struct {
    uint32_t frame_id;            /* RDP frame ID for acknowledgment */
    uint16_t surface_id;          /* Target surface ID */
    RdpGfxCodecId codec_id;       /* Actual codec used (AVC420/AVC444/AVC444v2) */
    RdpH264FrameType frame_type;  /* IDR/P/B frame */
    RdpRect dest_rect;            /* Destination rectangle on surface */
    uint32_t nal_size;            /* Size of NAL units */
    uint8_t* nal_data;            /* H.264 NAL units (Annex-B format) */
    /* For AVC444: second chroma stream */
    uint32_t chroma_nal_size;     /* Size of chroma NAL units (0 for AVC420) */
    uint8_t* chroma_nal_data;     /* Chroma NAL units for AVC444 */
    uint64_t timestamp;           /* Capture timestamp (microseconds) */
    bool needs_ack;               /* Whether frame requires acknowledgment */
} RdpH264Frame;

/* GFX surface descriptor */
typedef struct {
    uint16_t surface_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;        /* PIXEL_FORMAT_* constant */
    bool active;
    bool mapped_to_output;        /* Whether mapped to main display */
    int32_t output_x;             /* Output origin X */
    int32_t output_y;             /* Output origin Y */
} RdpGfxSurface;

/* Opaque session handle */
typedef struct RdpSession RdpSession;

/* ============================================================================
 * Session Registry API (for multi-user audio isolation)
 * ============================================================================ */

/**
 * Set the maximum number of concurrent RDP sessions allowed
 * 
 * Must be called before any sessions are created. The limit is clamped to
 * the range [RDP_MAX_SESSIONS_MIN, RDP_MAX_SESSIONS_MAX].
 * 
 * @param limit     Maximum number of sessions (default: 100, range: 2-1000)
 * @return          0 on success, -1 on failure
 */
int rdp_set_max_sessions(int limit);

/**
 * Get the current maximum sessions limit
 * 
 * @return          Current maximum session limit
 */
int rdp_get_max_sessions(void);

/**
 * Create a new RDP session (does not connect yet)
 * 
 * @param host      RDP server hostname or IP
 * @param port      RDP port (usually 3389)
 * @param username  Login username
 * @param password  Login password
 * @param domain    Windows domain (can be NULL or empty)
 * @param width     Initial display width
 * @param height    Initial display height
 * @param bpp       Bits per pixel (16, 24, or 32)
 * @return          Session handle or NULL on failure
 */
RdpSession* rdp_create(
    const char* host,
    uint16_t port,
    const char* username,
    const char* password,
    const char* domain,
    uint32_t width,
    uint32_t height,
    uint32_t bpp
);

/**
 * Connect to the RDP server
 * 
 * @param session   Session handle from rdp_create()
 * @return          0 on success, negative error code on failure
 */
int rdp_connect(RdpSession* session);

/**
 * Get current session state
 */
RdpState rdp_get_state(RdpSession* session);

/**
 * Get last error message
 * 
 * @param session   Session handle
 * @return          Error string (valid until next rdp_* call)
 */
const char* rdp_get_error(RdpSession* session);

/**
 * Poll for events and process frame updates
 * 
 * @param session       Session handle
 * @param timeout_ms    Maximum time to wait for events (0 = non-blocking)
 * @return              1 if new frame available, 0 if no update, negative on error
 */
int rdp_poll(RdpSession* session, int timeout_ms);

/**
 * Lock the frame buffer to prevent reallocation during read
 * 
 * MUST call rdp_unlock_frame_buffer() after reading the buffer!
 * Use this with rdp_get_frame_buffer() for high-performance direct access.
 * 
 * @param session   Session handle
 */
void rdp_lock_frame_buffer(RdpSession* session);

/**
 * Unlock the frame buffer after reading
 * 
 * @param session   Session handle
 */
void rdp_unlock_frame_buffer(RdpSession* session);

/**
 * Get pointer to the frame buffer
 * 
 * IMPORTANT: Caller MUST hold lock via rdp_lock_frame_buffer() before calling
 * and call rdp_unlock_frame_buffer() after reading the buffer!
 * 
 * @param session   Session handle
 * @param width     Output: frame width in pixels
 * @param height    Output: frame height in pixels  
 * @param stride    Output: bytes per row (may include padding)
 * @return          Pointer to pixel data, or NULL if not connected
 */
uint8_t* rdp_get_frame_buffer(
    RdpSession* session,
    int* width,
    int* height,
    int* stride
);

/**
 * Get dirty rectangles from the last frame update
 * 
 * Call after rdp_poll() returns 1 to get the changed regions.
 * 
 * @param session       Session handle
 * @param rects         Output array for rectangles
 * @param max_rects     Maximum number of rectangles to return
 * @return              Number of rectangles written, or negative on error
 */
int rdp_get_dirty_rects(
    RdpSession* session,
    RdpRect* rects,
    int max_rects
);

/**
 * Clear dirty rectangles after processing
 */
void rdp_clear_dirty_rects(RdpSession* session);

/**
 * Peek at the current dirty rectangle count without clearing or copying
 * 
 * @return Number of dirty rectangles currently accumulated
 */
int rdp_peek_dirty_rect_count(RdpSession* session);

/**
 * Check if a full frame refresh is needed
 * 
 * Returns true after connect, resize, or when too many dirty rects accumulated.
 * After calling this, the flag is cleared.
 * Returns false while a GFX frame is in progress.
 */
bool rdp_needs_full_frame(RdpSession* session);

/**
 * Check if a GFX frame is currently being processed
 * 
 * When true, Python should wait before sending frames to avoid sending
 * incomplete buffer state. This is set between StartFrame and EndFrame
 * callbacks in the GFX pipeline.
 * 
 * @return true if a frame is being processed, false if safe to send
 */
bool rdp_gfx_frame_in_progress(RdpSession* session);

/**
 * Get the last completed GFX frame ID
 * 
 * Returns the frame ID of the most recently completed GFX frame (after EndFrame).
 * Python can track this to know when new dirty rects are ready to be sent.
 * Only read dirty rects when this value changes from the last check.
 * 
 * @return Last completed frame ID, or 0 if no frames completed yet
 */
uint32_t rdp_gfx_get_last_completed_frame(RdpSession* session);

/**
 * Send mouse input event
 * 
 * @param session   Session handle
 * @param flags     Combination of RDP_MOUSE_FLAG_* values
 * @param x         X coordinate
 * @param y         Y coordinate
 */
void rdp_send_mouse(RdpSession* session, uint16_t flags, int x, int y);

/**
 * Send keyboard input event
 * 
 * @param session   Session handle
 * @param flags     RDP_KBD_FLAG_DOWN, RDP_KBD_FLAG_RELEASE, optionally | RDP_KBD_FLAG_EXTENDED
 * @param scancode  Keyboard scan code
 */
void rdp_send_keyboard(RdpSession* session, uint16_t flags, uint16_t scancode);

/**
 * Send unicode character input
 * 
 * @param session   Session handle
 * @param flags     RDP_KBD_FLAG_DOWN or RDP_KBD_FLAG_RELEASE
 * @param code      Unicode code point
 */
void rdp_send_unicode(RdpSession* session, uint16_t flags, uint16_t code);

/**
 * Resize the RDP session
 * 
 * May cause brief disconnection depending on server capabilities.
 * 
 * @param session   Session handle
 * @param width     New width
 * @param height    New height
 * @return          0 on success, negative on error
 */
int rdp_resize(RdpSession* session, uint32_t width, uint32_t height);

/**
 * Check if audio data is available
 * 
 * @param session   Session handle
 * @return          true if audio data is available
 */
bool rdp_has_audio_data(RdpSession* session);

/**
 * Get the current audio format
 * 
 * @param session       Session handle
 * @param sample_rate   Output: sample rate in Hz (e.g., 48000)
 * @param channels      Output: number of channels (1=mono, 2=stereo)
 * @param bits          Output: bits per sample (8, 16, or 32)
 * @return              0 on success, negative if audio not initialized
 */
int rdp_get_audio_format(RdpSession* session, int* sample_rate, int* channels, int* bits);

/**
 * Get available audio data
 * 
 * Reads PCM audio data from the internal buffer.
 * 
 * @param session   Session handle
 * @param buffer    Output buffer for audio data
 * @param max_size  Maximum bytes to read
 * @return          Number of bytes read, 0 if no data, negative on error
 */
int rdp_get_audio_data(RdpSession* session, uint8_t* buffer, int max_size);

/**
 * Write audio data to the buffer (internal use by rdpsnd callback)
 * 
 * @param session       Session handle
 * @param data          Audio PCM data
 * @param size          Size in bytes
 * @param sample_rate   Sample rate in Hz
 * @param channels      Number of channels
 * @param bits          Bits per sample
 */
void rdp_write_audio_data(RdpSession* session, const uint8_t* data, size_t size,
                          int sample_rate, int channels, int bits);

/* ============================================================================
 * Opus Audio API (for native audio streaming without PulseAudio)
 * ============================================================================ */

/**
 * AudioContext structure for RDPSND bridge plugin
 * 
 * This is exposed so the rdpsnd_bridge.so plugin can access the audio buffer.
 * The plugin is dynamically loaded by FreeRDP, so we use this as a shared
 * interface between rdp_bridge.so and rdpsnd_bridge.so.
 */
typedef struct {
    uint8_t* opus_buffer;           /* Ring buffer for Opus frames */
    size_t opus_buffer_size;        /* Total buffer size */
    size_t opus_write_pos;          /* Write position */
    size_t opus_read_pos;           /* Read position */
    void* opus_mutex;               /* pthread_mutex_t* for thread-safe access */
    int sample_rate;                /* Current sample rate (e.g., 48000) */
    int channels;                   /* Current channel count (1 or 2) */
    volatile int initialized;       /* Non-zero when audio is ready */
} RdpAudioContext;

/**
 * Set the audio context for the RDPSND bridge plugin
 * 
 * Must be called before rdp_connect() to allow the dynamically loaded
 * rdpsnd_bridge.so plugin to access the audio buffer.
 * 
 * @param session   Session handle
 */
void rdp_set_audio_context(RdpSession* session);

/**
 * Check if Opus audio data is available
 * 
 * @param session   Session handle
 * @return          true if Opus frames are available
 */
bool rdp_has_opus_data(RdpSession* session);

/**
 * Get Opus audio format information
 * 
 * @param session       Session handle
 * @param sample_rate   Output: sample rate in Hz (e.g., 48000)
 * @param channels      Output: number of channels (1 or 2)
 * @return              0 on success, negative if audio not initialized
 */
int rdp_get_opus_format(RdpSession* session, int* sample_rate, int* channels);

/**
 * Get next Opus frame from the buffer
 * 
 * Each Opus frame is a self-contained encoded packet that can be decoded
 * independently by the browser's WebCodecs AudioDecoder.
 * 
 * @param session   Session handle
 * @param buffer    Output buffer for Opus frame data
 * @param max_size  Maximum bytes to read
 * @return          Size of Opus frame, 0 if no data, negative on error
 */
int rdp_get_opus_frame(RdpSession* session, uint8_t* buffer, int max_size);

/**
 * Disconnect from the RDP server
 */
void rdp_disconnect(RdpSession* session);

/**
 * Destroy session and free all resources
 */
void rdp_destroy(RdpSession* session);

/**
 * Get library version string
 */
const char* rdp_version(void);

/* ============================================================================
 * GFX Pipeline / H.264 API (for AVC420/AVC444 streaming)
 * ============================================================================ */

/**
 * Check if GFX pipeline is active (H.264/AVC mode negotiated)
 * 
 * @param session   Session handle
 * @return          true if GFX pipeline is active with H.264 codec
 */
bool rdp_gfx_is_active(RdpSession* session);

/**
 * Get negotiated GFX codec ID
 * 
 * @param session   Session handle
 * @return          Active codec ID (RDP_GFX_CODEC_AVC420, AVC444, etc.) or 0
 */
RdpGfxCodecId rdp_gfx_get_codec(RdpSession* session);

/**
 * Check if H.264 frames are available
 * 
 * @param session   Session handle
 * @return          Number of frames available (0 if none)
 */
int rdp_has_h264_frames(RdpSession* session);

/**
 * Get next H.264 frame from the GFX pipeline queue
 * 
 * The frame data is valid until the next call to rdp_get_h264_frame()
 * or rdp_ack_h264_frame(). For AVC444, both nal_data and chroma_nal_data
 * must be combined per MS-RDPEGFX specification.
 * 
 * @param session   Session handle
 * @param frame     Output: frame descriptor (caller must NOT free nal_data)
 * @return          0 on success, -1 if no frames, -2 on error
 */
int rdp_get_h264_frame(RdpSession* session, RdpH264Frame* frame);

/**
 * Acknowledge an H.264 frame (send RDPGFX_FRAME_ACKNOWLEDGE_PDU)
 * 
 * Must be called after processing each frame to prevent server back-pressure.
 * 
 * @param session   Session handle  
 * @param frame_id  Frame ID from RdpH264Frame.frame_id
 * @return          0 on success, negative on error
 */
int rdp_ack_h264_frame(RdpSession* session, uint32_t frame_id);

/**
 * Get information about a GFX surface
 * 
 * @param session       Session handle
 * @param surface_id    Surface ID
 * @param surface       Output: surface descriptor
 * @return              0 on success, -1 if surface not found
 */
int rdp_gfx_get_surface(RdpSession* session, uint16_t surface_id, RdpGfxSurface* surface);

/**
 * Get the primary output surface ID
 * 
 * @param session   Session handle
 * @return          Primary surface ID or 0 if not mapped
 */
uint16_t rdp_gfx_get_primary_surface(RdpSession* session);

#ifdef __cplusplus
}
#endif

#endif /* RDP_BRIDGE_H */
