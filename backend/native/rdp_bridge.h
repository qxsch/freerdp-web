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

/* Maximum dirty rectangles per frame */
#define RDP_MAX_DIRTY_RECTS 64

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

/* Opaque session handle */
typedef struct RdpSession RdpSession;

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
 * Get pointer to the frame buffer
 * 
 * The buffer contains BGRA pixel data (32-bit per pixel).
 * Valid until session is destroyed or resized.
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
 * Check if a full frame refresh is needed
 * 
 * Returns true after connect, resize, or when too many dirty rects accumulated.
 * After calling this, the flag is cleared.
 */
bool rdp_needs_full_frame(RdpSession* session);

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

#ifdef __cplusplus
}
#endif

#endif /* RDP_BRIDGE_H */
