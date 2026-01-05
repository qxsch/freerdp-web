/**
 * RDP Bridge Native Library Implementation
 * 
 * Uses FreeRDP3 libfreerdp for direct RDP connection with:
 * - GFX pipeline with H.264/AVC444 support for low-latency video
 * - GDI software rendering as fallback
 * - Dirty rectangle tracking for delta updates
 * - Direct input injection (no X11/xdotool)
 */

#include "rdp_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <dlfcn.h>
#include <time.h>

/* FreeRDP3 headers */
#include <freerdp/freerdp.h>
#include <freerdp/client.h>
#include <freerdp/addin.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/rdpsnd.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client/disp.h>
#include <freerdp/client/rdpsnd.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/client/channels.h>
#include <freerdp/event.h>
#include <freerdp/codec/clear.h>
#include <freerdp/codec/planar.h>
#include <freerdp/codec/progressive.h>
#include <freerdp/codec/color.h>
#include <opus/opus.h>
#include <winpr/crt.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/collections.h>
#include <freerdp/codec/region.h>

/* FFmpeg for AVC444 transcoding (4:4:4 → 4:2:0) */
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define RDP_BRIDGE_VERSION "1.6.0"
#define MAX_ERROR_LEN 512

/* Debug flags - set to 1 to enable verbose logging for specific subsystems */
#define DEBUG_GFX_CACHE 0        /* Log all SurfaceToCache/CacheToSurface operations */
#define DEBUG_GFX_FILL 0         /* Log all SolidFill operations */
#define DEBUG_GFX_COPY 0         /* Log all SurfaceToSurface operations */

/* Session registry limits */
#define RDP_MAX_SESSIONS_DEFAULT 100
#define RDP_MAX_SESSIONS_MIN 2
#define RDP_MAX_SESSIONS_MAX 1000

/* Internal H.264 frame queue entry */
typedef struct {
    uint32_t frame_id;
    uint16_t surface_id;
    RdpGfxCodecId codec_id;
    RdpH264FrameType frame_type;
    RdpRect dest_rect;
    uint32_t nal_size;
    uint8_t* nal_data;           /* Allocated buffer */
    uint32_t chroma_nal_size;
    uint8_t* chroma_nal_data;    /* Allocated buffer for AVC444 */
    uint64_t timestamp;
    bool needs_ack;
    bool valid;                  /* Slot in use */
} H264FrameEntry;

/* GFX cache entry - for SurfaceToCache/CacheToSurface operations */
typedef struct {
    bool valid;
    uint32_t width;
    uint32_t height;
    uint8_t* data;               /* BGRA32 pixel data */
    size_t data_size;
} GfxCacheEntry;

/* Extended client context */
typedef struct {
    rdpClientContext common;        /* Must be first */
    
    /* Our custom fields */
    RdpState state;
    char error_msg[MAX_ERROR_LEN];
    
    /* Frame buffer (GDI fallback) */
    uint8_t* frame_buffer;
    int frame_width;
    int frame_height;
    int frame_stride;
    
    /* Dirty rectangle tracking */
    RdpRect dirty_rects[RDP_MAX_DIRTY_RECTS];
    int dirty_rect_count;
    bool needs_full_frame;
    pthread_mutex_t rect_mutex;
    
    /* Resize pending */
    bool resize_pending;
    uint32_t pending_width;
    uint32_t pending_height;
    
    /* Display control channel */
    DispClientContext* disp;
    
    /* Graphics pipeline channel */
    RdpgfxClientContext* gfx;
    bool gfx_active;                /* GFX pipeline successfully initialized */
    bool gfx_disconnecting;         /* Connection is being torn down - don't call GDI */
    RdpGfxCodecId gfx_codec;        /* Negotiated codec */
    bool gfx_pipeline_needs_init;   /* Deferred GDI pipeline init flag */
    bool gfx_pipeline_ready;        /* GDI pipeline initialized for decoding */
    rdpGdi* gdi;                    /* GDI pointer for chaining to GDI handlers */
    
    /* Saved GDI pipeline callbacks for chaining */
    pcRdpgfxSurfaceCommand gdi_surface_command;
    pcRdpgfxResetGraphics gdi_reset_graphics;
    pcRdpgfxCreateSurface gdi_create_surface;
    pcRdpgfxDeleteSurface gdi_delete_surface;
    pcRdpgfxMapSurfaceToOutput gdi_map_surface;
    pcRdpgfxStartFrame gdi_start_frame;
    pcRdpgfxEndFrame gdi_end_frame;
    
    /* GFX surfaces */
    RdpGfxSurface surfaces[RDP_MAX_GFX_SURFACES];
    uint16_t primary_surface_id;
    uint32_t current_frame_id;      /* Current frame ID from start_frame */
    uint32_t last_completed_frame_id; /* Last frame ID that completed (EndFrame called) */
    uint32_t frame_cmd_count;       /* Commands received in current frame */
    bool gfx_frame_in_progress;     /* True between StartFrame and EndFrame */
    pthread_mutex_t gfx_mutex;
    
    /* H.264 frame queue (ring buffer) */
    H264FrameEntry h264_frames[RDP_MAX_H264_FRAMES];
    int h264_write_idx;
    int h264_read_idx;
    int h264_count;
    bool h264_queued_this_frame;  /* H.264 was queued in current GFX frame */
    pthread_mutex_t h264_mutex;
    
    /* Audio playback */
    rdpsndDevicePlugin* rdpsnd;
    OpusEncoder* opus_encoder;
    uint8_t* audio_buffer;
    size_t audio_buffer_size;
    size_t audio_buffer_pos;
    size_t audio_read_pos;
    pthread_mutex_t audio_mutex;
    int audio_sample_rate;
    int audio_channels;
    int audio_bits;
    bool audio_initialized;
    
    /* Opus audio buffer (for native audio streaming) */
    uint8_t* opus_buffer;
    size_t opus_buffer_size;
    size_t opus_write_pos;
    size_t opus_read_pos;
    pthread_mutex_t opus_mutex;
    int opus_sample_rate;
    int opus_channels;
    volatile int opus_initialized;
    
    /* AVC444 transcoder (4:4:4 → 4:2:0 for browser compatibility) */
    AVCodecContext* avc_decoder_luma;
    AVCodecContext* avc_decoder_chroma;
    AVCodecContext* avc_encoder;
    struct SwsContext* sws_ctx;
    AVFrame* decoded_frame_luma;
    AVFrame* decoded_frame_chroma;
    AVFrame* combined_frame;       /* Combined YUV444 */
    AVFrame* output_frame;         /* Converted YUV420 */
    AVPacket* encode_pkt;
    bool transcoder_initialized;
    
    /* Codec decoders for non-H.264 GFX codecs (thread-safe, no GDI dependency) */
    CLEAR_CONTEXT* clear_decoder;
    BITMAP_PLANAR_CONTEXT* planar_decoder;
    PROGRESSIVE_CONTEXT* progressive_decoder;
    
    /* Per-surface pixel buffers for codec decoding (indexed by surface ID) */
    uint8_t* surface_buffers[RDP_MAX_GFX_SURFACES];
    
    /* GFX bitmap cache for SurfaceToCache/CacheToSurface operations */
    GfxCacheEntry* gfx_cache;     /* Dynamically allocated cache */
    int gfx_cache_size;           /* Number of slots allocated */
    
    /* GFX event queue for wire format streaming (Python consumption) */
    RdpGfxEvent gfx_events[RDP_MAX_GFX_EVENTS];
    int gfx_event_write_idx;
    int gfx_event_read_idx;
    int gfx_event_count;
    pthread_mutex_t gfx_event_mutex;
    
} BridgeContext;

/* Forward declarations */
static BOOL bridge_pre_connect(freerdp* instance);
static BOOL bridge_post_connect(freerdp* instance);
static void bridge_post_disconnect(freerdp* instance);
static BOOL bridge_begin_paint(rdpContext* context);
static BOOL bridge_end_paint(rdpContext* context);
static BOOL bridge_desktop_resize(rdpContext* context);
static void bridge_on_channel_connected(void* ctx, const ChannelConnectedEventArgs* e);
static void bridge_on_channel_disconnected(void* ctx, const ChannelDisconnectedEventArgs* e);

/* GFX pipeline callback forward declarations */
static UINT gfx_on_caps_confirm(RdpgfxClientContext* context, const RDPGFX_CAPS_CONFIRM_PDU* caps);
static UINT gfx_on_reset_graphics(RdpgfxClientContext* context, const RDPGFX_RESET_GRAPHICS_PDU* reset);
static UINT gfx_on_create_surface(RdpgfxClientContext* context, const RDPGFX_CREATE_SURFACE_PDU* create);
static UINT gfx_on_delete_surface(RdpgfxClientContext* context, const RDPGFX_DELETE_SURFACE_PDU* del);
static UINT gfx_on_map_surface(RdpgfxClientContext* context, const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU* map);
static UINT gfx_on_map_surface_scaled(RdpgfxClientContext* context, const RDPGFX_MAP_SURFACE_TO_SCALED_OUTPUT_PDU* map);
static UINT gfx_on_map_surface_window(RdpgfxClientContext* context, const RDPGFX_MAP_SURFACE_TO_WINDOW_PDU* map);
static UINT gfx_on_map_surface_scaled_window(RdpgfxClientContext* context, const RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU* map);
static UINT gfx_on_surface_command(RdpgfxClientContext* context, const RDPGFX_SURFACE_COMMAND* cmd);
static UINT gfx_on_start_frame(RdpgfxClientContext* context, const RDPGFX_START_FRAME_PDU* start);
static UINT gfx_on_end_frame(RdpgfxClientContext* context, const RDPGFX_END_FRAME_PDU* end);
static UINT gfx_on_solid_fill(RdpgfxClientContext* context, const RDPGFX_SOLID_FILL_PDU* fill);
static UINT gfx_on_surface_to_surface(RdpgfxClientContext* context, const RDPGFX_SURFACE_TO_SURFACE_PDU* copy);
static UINT gfx_on_surface_to_cache(RdpgfxClientContext* context, const RDPGFX_SURFACE_TO_CACHE_PDU* cache);
static UINT gfx_on_cache_to_surface(RdpgfxClientContext* context, const RDPGFX_CACHE_TO_SURFACE_PDU* cache);
static UINT gfx_on_evict_cache(RdpgfxClientContext* context, const RDPGFX_EVICT_CACHE_ENTRY_PDU* evict);
static UINT gfx_on_delete_encoding_context(RdpgfxClientContext* context, const RDPGFX_DELETE_ENCODING_CONTEXT_PDU* del);
static UINT gfx_on_cache_import_reply(RdpgfxClientContext* context, const RDPGFX_CACHE_IMPORT_REPLY_PDU* reply);

/* GFX event queue helper */
static void gfx_queue_event(BridgeContext* ctx, const RdpGfxEvent* event);

/* Transcoder forward declarations */
static bool init_transcoder(BridgeContext* ctx, int width, int height);
static void cleanup_transcoder(BridgeContext* ctx);
static bool transcode_avc444(BridgeContext* ctx,
                             const uint8_t* luma_data, uint32_t luma_size,
                             const uint8_t* chroma_data, uint32_t chroma_size,
                             uint8_t** out_data, uint32_t* out_size);

/* Deferred GDI pipeline initialization - call from main thread */
static void maybe_init_gfx_pipeline(BridgeContext* bctx);

/* Global audio context structure for plugin communication.
 * This is a regular global (not thread-local) because the rdpsnd plugin
 * runs in a different thread from the main Python thread, and we need
 * both threads to access the same buffer.
 * 
 * MULTI-SESSION NOTE: This global is protected by g_connect_mutex during the
 * connect phase. Each session has its own audio buffer; this global just
 * serves as a handoff mechanism to the plugin during Open callback.
 * 
 * For multi-session support, write_pos and read_pos are POINTERS to the
 * actual positions in the BridgeContext. */
static struct {
    uint8_t* opus_buffer;
    size_t opus_buffer_size;
    size_t* opus_write_pos;         /* POINTER to BridgeContext.opus_write_pos */
    size_t* opus_read_pos;          /* POINTER to BridgeContext.opus_read_pos */
    void* opus_mutex;
    int sample_rate;
    int channels;
    volatile int* initialized;      /* POINTER to BridgeContext.opus_initialized */
} g_audio_ctx;

/* Mutex to protect the connect phase (g_audio_ctx handoff to plugin) */
static pthread_mutex_t g_connect_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================================
 * Session Registry for Multi-User Audio Isolation
 * 
 * Maps rdpContext pointers to BridgeContext pointers so the RDPSND plugin
 * can look up the correct audio buffer for each session.
 * ============================================================================ */

typedef struct {
    rdpContext* rdp_ctx;
    BridgeContext* bridge_ctx;
} SessionEntry;

static SessionEntry* g_session_registry = NULL;
static int g_session_count = 0;
static int g_max_sessions = RDP_MAX_SESSIONS_DEFAULT;
static pthread_mutex_t g_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_registry_initialized = 0;

/* Set maximum allowed sessions (called from Python at startup) */
__attribute__((visibility("default")))
int rdp_set_max_sessions(int limit)
{
    pthread_mutex_lock(&g_registry_mutex);
    
    if (limit < RDP_MAX_SESSIONS_MIN) {
        fprintf(stderr, "[rdp_bridge] Warning: RDP_MAX_SESSIONS=%d is below minimum %d, using %d\n",
                limit, RDP_MAX_SESSIONS_MIN, RDP_MAX_SESSIONS_MIN);
        limit = RDP_MAX_SESSIONS_MIN;
    } else if (limit > RDP_MAX_SESSIONS_MAX) {
        fprintf(stderr, "[rdp_bridge] Warning: RDP_MAX_SESSIONS=%d exceeds maximum %d, using %d\n",
                limit, RDP_MAX_SESSIONS_MAX, RDP_MAX_SESSIONS_MAX);
        limit = RDP_MAX_SESSIONS_MAX;
    }
    
    /* Only allow changing if no sessions are active */
    if (g_session_count > 0) {
        fprintf(stderr, "[rdp_bridge] Warning: Cannot change max sessions while sessions are active\n");
        pthread_mutex_unlock(&g_registry_mutex);
        return -1;
    }
    
    /* Free old registry if it exists */
    if (g_session_registry) {
        free(g_session_registry);
        g_session_registry = NULL;
    }
    
    g_max_sessions = limit;
    g_session_registry = calloc(g_max_sessions, sizeof(SessionEntry));
    if (!g_session_registry) {
        fprintf(stderr, "[rdp_bridge] ERROR: Failed to allocate session registry for %d sessions\n", g_max_sessions);
        pthread_mutex_unlock(&g_registry_mutex);
        return -1;
    }
    
    g_registry_initialized = 1;
    fprintf(stderr, "[rdp_bridge] Session registry initialized: max_sessions=%d\n", g_max_sessions);
    
    pthread_mutex_unlock(&g_registry_mutex);
    return 0;
}

/* Get current max sessions limit */
__attribute__((visibility("default")))
int rdp_get_max_sessions(void)
{
    return g_max_sessions;
}

/* Register a session in the registry */
static int session_registry_add(rdpContext* rdp_ctx, BridgeContext* bridge_ctx)
{
    pthread_mutex_lock(&g_registry_mutex);
    
    /* Initialize registry if not done yet */
    if (!g_registry_initialized) {
        g_session_registry = calloc(g_max_sessions, sizeof(SessionEntry));
        if (!g_session_registry) {
            fprintf(stderr, "[rdp_bridge] ERROR: Failed to allocate session registry\n");
            pthread_mutex_unlock(&g_registry_mutex);
            return -1;
        }
        g_registry_initialized = 1;
        fprintf(stderr, "[rdp_bridge] Session registry initialized on first use: max_sessions=%d\n", g_max_sessions);
    }
    
    /* Check if we're at capacity */
    if (g_session_count >= g_max_sessions) {
        fprintf(stderr, "[rdp_bridge] ERROR: Session limit reached (%d/%d) - cannot create new session\n",
                g_session_count, g_max_sessions);
        pthread_mutex_unlock(&g_registry_mutex);
        return -2; /* Distinct error code for limit reached */
    }
    
    /* Find empty slot */
    for (int i = 0; i < g_max_sessions; i++) {
        if (g_session_registry[i].rdp_ctx == NULL) {
            g_session_registry[i].rdp_ctx = rdp_ctx;
            g_session_registry[i].bridge_ctx = bridge_ctx;
            g_session_count++;
            fprintf(stderr, "[rdp_bridge] Session registered: %d/%d active\n", 
                    g_session_count, g_max_sessions);
            pthread_mutex_unlock(&g_registry_mutex);
            return 0;
        }
    }
    
    fprintf(stderr, "[rdp_bridge] ERROR: No empty slot found in session registry\n");
    pthread_mutex_unlock(&g_registry_mutex);
    return -1;
}

/* Unregister a session from the registry */
static void session_registry_remove(rdpContext* rdp_ctx)
{
    pthread_mutex_lock(&g_registry_mutex);
    
    if (!g_session_registry) {
        pthread_mutex_unlock(&g_registry_mutex);
        return;
    }
    
    for (int i = 0; i < g_max_sessions; i++) {
        if (g_session_registry[i].rdp_ctx == rdp_ctx) {
            g_session_registry[i].rdp_ctx = NULL;
            g_session_registry[i].bridge_ctx = NULL;
            g_session_count--;
            fprintf(stderr, "[rdp_bridge] Session unregistered: %d/%d active\n",
                    g_session_count, g_max_sessions);
            break;
        }
    }
    
    pthread_mutex_unlock(&g_registry_mutex);
}

/* Look up BridgeContext by rdpContext (exported for plugin use) */
__attribute__((visibility("default")))
void* rdp_lookup_session_by_rdpcontext(void* rdp_ctx)
{
    if (!rdp_ctx) return NULL;
    
    pthread_mutex_lock(&g_registry_mutex);
    
    if (!g_session_registry) {
        pthread_mutex_unlock(&g_registry_mutex);
        return NULL;
    }
    
    for (int i = 0; i < g_max_sessions; i++) {
        if (g_session_registry[i].rdp_ctx == (rdpContext*)rdp_ctx) {
            BridgeContext* ctx = g_session_registry[i].bridge_ctx;
            pthread_mutex_unlock(&g_registry_mutex);
            return ctx;
        }
    }
    
    pthread_mutex_unlock(&g_registry_mutex);
    return NULL;
}

/* Get audio context for a specific session (exported for plugin use)
 * Returns a thread-local struct with POINTERS to the BridgeContext fields */
__attribute__((visibility("default")))
void* rdp_get_session_audio_context(void* session_ptr)
{
    BridgeContext* ctx = (BridgeContext*)session_ptr;
    if (!ctx) return NULL;
    
    /* Return a pointer to a structure matching what the plugin expects
     * Using pointers for mutable fields so plugin writes update BridgeContext */
    static __thread struct {
        uint8_t* opus_buffer;
        size_t opus_buffer_size;
        size_t* opus_write_pos;
        size_t* opus_read_pos;
        void* opus_mutex;
        int sample_rate;
        int channels;
        volatile int* initialized;
    } session_audio_ctx;
    
    session_audio_ctx.opus_buffer = ctx->opus_buffer;
    session_audio_ctx.opus_buffer_size = ctx->opus_buffer_size;
    session_audio_ctx.opus_write_pos = &ctx->opus_write_pos;
    session_audio_ctx.opus_read_pos = &ctx->opus_read_pos;
    session_audio_ctx.opus_mutex = &ctx->opus_mutex;
    session_audio_ctx.sample_rate = ctx->opus_sample_rate;
    session_audio_ctx.channels = ctx->opus_channels;
    session_audio_ctx.initialized = &ctx->opus_initialized;
    
    return &session_audio_ctx;
}

/* ============================================================================
 * Session Lifecycle
 * ============================================================================ */

RdpSession* rdp_create(
    const char* host,
    uint16_t port,
    const char* username,
    const char* password,
    const char* domain,
    uint32_t width,
    uint32_t height,
    uint32_t bpp)
{
    rdpContext* context = NULL;
    freerdp* instance = NULL;
    rdpSettings* settings = NULL;
    RDP_CLIENT_ENTRY_POINTS clientEntryPoints = { 0 };
    
    /* Initialize client entry points */
    clientEntryPoints.Size = sizeof(RDP_CLIENT_ENTRY_POINTS);
    clientEntryPoints.Version = RDP_CLIENT_INTERFACE_VERSION;
    clientEntryPoints.ContextSize = sizeof(BridgeContext);
    
    /* Create FreeRDP context (FreeRDP3 returns rdpContext*, not freerdp*) */
    context = freerdp_client_context_new(&clientEntryPoints);
    if (!context) {
        return NULL;
    }
    
    /* Get the instance from context */
    instance = context->instance;
    
    BridgeContext* ctx = (BridgeContext*)context;
    ctx->state = RDP_STATE_DISCONNECTED;
    ctx->needs_full_frame = true;
    ctx->dirty_rect_count = 0;
    pthread_mutex_init(&ctx->rect_mutex, NULL);
    pthread_mutex_init(&ctx->audio_mutex, NULL);
    pthread_mutex_init(&ctx->opus_mutex, NULL);
    pthread_mutex_init(&ctx->gfx_mutex, NULL);
    pthread_mutex_init(&ctx->h264_mutex, NULL);
    pthread_mutex_init(&ctx->gfx_event_mutex, NULL);
    ctx->audio_initialized = false;
    ctx->audio_buffer = NULL;
    ctx->audio_buffer_size = 0;
    ctx->opus_encoder = NULL;
    
    /* Initialize GFX/H.264 structures */
    ctx->gfx = NULL;
    ctx->gfx_active = false;
    ctx->gfx_codec = RDP_GFX_CODEC_UNCOMPRESSED;
    ctx->primary_surface_id = 0;
    memset(ctx->surfaces, 0, sizeof(ctx->surfaces));
    memset(ctx->surface_buffers, 0, sizeof(ctx->surface_buffers));
    memset(ctx->h264_frames, 0, sizeof(ctx->h264_frames));
    ctx->h264_write_idx = 0;
    ctx->h264_read_idx = 0;
    ctx->h264_count = 0;
    
    /* Initialize GFX event queue for wire format streaming */
    memset(ctx->gfx_events, 0, sizeof(ctx->gfx_events));
    ctx->gfx_event_write_idx = 0;
    ctx->gfx_event_read_idx = 0;
    ctx->gfx_event_count = 0;
    
    /* Initialize codec decoders for non-H.264 GFX codecs */
    ctx->clear_decoder = clear_context_new(FALSE);  /* FALSE = decompressor */
    ctx->planar_decoder = freerdp_bitmap_planar_context_new(0, 64, 64);  /* Will resize as needed */
    ctx->progressive_decoder = progressive_context_new(FALSE);  /* FALSE = decompressor */
    
    /* Initialize GFX bitmap cache */
    ctx->gfx_cache_size = RDP_MAX_GFX_CACHE_SLOTS;
    ctx->gfx_cache = (GfxCacheEntry*)calloc(ctx->gfx_cache_size, sizeof(GfxCacheEntry));
    
    /* Initialize Opus buffer for native audio streaming.
     * 256KB allows ~4 seconds of Opus audio at 64kbps, which provides
     * enough headroom during graphics-intensive operations (window moves,
     * video playback) when the Python audio streaming loop may be delayed. */
    ctx->opus_buffer_size = 256 * 1024;  /* 256KB for ~4 seconds of Opus at 64kbps */
    ctx->opus_buffer = (uint8_t*)calloc(1, ctx->opus_buffer_size);
    ctx->opus_write_pos = 0;
    ctx->opus_read_pos = 0;
    ctx->opus_sample_rate = 48000;
    ctx->opus_channels = 2;
    ctx->opus_initialized = 0;
    
    /* NOTE: g_audio_ctx is now set in rdp_connect() under g_connect_mutex lock
     * to ensure thread-safe handoff to the plugin during connect */
    
    /* Set callbacks */
    instance->PreConnect = bridge_pre_connect;
    instance->PostConnect = bridge_post_connect;
    instance->PostDisconnect = bridge_post_disconnect;
    
    /* Configure settings */
    settings = context->settings;
    
    /* Connection */
    if (!freerdp_settings_set_string(settings, FreeRDP_ServerHostname, host)) goto fail;
    if (!freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, port)) goto fail;
    
    /* Credentials */
    if (username && *username) {
        if (!freerdp_settings_set_string(settings, FreeRDP_Username, username)) goto fail;
    }
    if (password && *password) {
        if (!freerdp_settings_set_string(settings, FreeRDP_Password, password)) goto fail;
    }
    if (domain && *domain) {
        if (!freerdp_settings_set_string(settings, FreeRDP_Domain, domain)) goto fail;
    }
    
    /* Display settings */
    if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, width)) goto fail;
    if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, height)) goto fail;
    if (!freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, bpp)) goto fail;
    
    /* Enable GFX pipeline with H.264/AVC444 for modern, low-latency graphics.
     * This enables the RDPEGFX channel which carries H.264-encoded frames.
     * Server must have "Prioritize H.264/AVC 444" policy enabled for best results. */
    if (!freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_GfxH264, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, TRUE)) goto fail;
    /* DISABLE progressive codec - it causes crashes in GDI's gdi_OutputUpdate.
     * The server will use H.264 if available, otherwise fall back to other codecs.
     * Progressive codec can be enabled via RDP_ENABLE_PROGRESSIVE=1 environment variable.
     * When enabled, Progressive tiles are sent to the browser for WASM-based decoding. */
    {
        const char* prog_env = getenv("RDP_ENABLE_PROGRESSIVE");
        BOOL enable_progressive = (prog_env && (strcmp(prog_env, "1") == 0 || strcasecmp(prog_env, "true") == 0));
        if (enable_progressive) {
            fprintf(stderr, "[rdp_bridge] Progressive codec ENABLED via RDP_ENABLE_PROGRESSIVE\n");
        }
        if (!freerdp_settings_set_bool(settings, FreeRDP_GfxProgressive, enable_progressive)) goto fail;
        if (!freerdp_settings_set_bool(settings, FreeRDP_GfxProgressiveV2, enable_progressive)) goto fail;
    }
    /* Disable legacy codecs - we want H.264 only */
    if (!freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, FALSE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_NSCodec, FALSE)) goto fail;
    /* GFX options for optimal streaming.
     * GfxSmallCache = TRUE: Tells server to use smaller tile cache, resulting in more
     * direct WireToSurface updates and fewer SurfaceToCache/CacheToSurface operations.
     * This avoids leftover artifacts when windows are de-maximized within the RDP session,
     * as the server won't rely on our client-side cache for desktop background repaints.
     * GfxThinClient = FALSE: Keep full AVC444/H.264 quality (ThinClient would reduce it). */
    if (!freerdp_settings_set_bool(settings, FreeRDP_GfxSmallCache, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_GfxThinClient, FALSE)) goto fail;
    
    /* Audio playback - configure rdpsnd with our bridge device plugin.
     * We add rdpsnd to BOTH static and dynamic channel collections with sys:bridge
     * to ensure our bridge plugin is used regardless of which channel Windows prefers.
     * AudioPlayback must be TRUE for the server to send audio data. */
    if (!freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_AudioCapture, FALSE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_RemoteConsoleAudio, FALSE)) goto fail;
    
    /* Add rdpsnd to STATIC channel collection with sys:bridge */
    {
        ADDIN_ARGV* args = freerdp_addin_argv_new(2, (const char*[]){"rdpsnd", "sys:bridge"});
        if (args) {
            if (!freerdp_static_channel_collection_add(settings, args)) {
                fprintf(stderr, "[rdp_bridge] Warning: Could not add rdpsnd static channel\n");
                freerdp_addin_argv_free(args);
            } else {
                fprintf(stderr, "[rdp_bridge] Added rdpsnd static channel with sys:bridge\n");
            }
        }
    }
    
    /* Add rdpsnd to DYNAMIC channel collection with sys:bridge */
    {
        ADDIN_ARGV* args = freerdp_addin_argv_new(2, (const char*[]){"rdpsnd", "sys:bridge"});
        if (args) {
            if (!freerdp_dynamic_channel_collection_add(settings, args)) {
                fprintf(stderr, "[rdp_bridge] Warning: Could not add rdpsnd dynamic channel\n");
                freerdp_addin_argv_free(args);
            } else {
                fprintf(stderr, "[rdp_bridge] Added rdpsnd dynamic channel with sys:bridge\n");
            }
        }
    }
    
    /* Performance optimizations */
    if (!freerdp_settings_set_bool(settings, FreeRDP_FastPathOutput, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_FastPathInput, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled, TRUE)) goto fail;
    
    /* Compression */
    if (!freerdp_settings_set_bool(settings, FreeRDP_CompressionEnabled, TRUE)) goto fail;
    if (!freerdp_settings_set_uint32(settings, FreeRDP_CompressionLevel, 2)) goto fail;
    
    /* Disable features we don't need */
    if (!freerdp_settings_set_bool(settings, FreeRDP_Workarea, FALSE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_Fullscreen, FALSE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_GrabKeyboard, FALSE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_Decorations, FALSE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_AllowDesktopComposition, FALSE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_DisableWallpaper, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_DisableFullWindowDrag, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_DisableMenuAnims, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_DisableThemes, TRUE)) goto fail;
    
    /* Disable device redirection channels we don't need (prevents RDPDR errors) */
    if (!freerdp_settings_set_bool(settings, FreeRDP_DeviceRedirection, FALSE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_RedirectDrives, FALSE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_RedirectPrinters, FALSE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_RedirectSmartCards, FALSE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_RedirectSerialPorts, FALSE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_RedirectParallelPorts, FALSE)) goto fail;
    
    /* Certificate handling - ignore for simplicity */
    if (!freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_AutoAcceptCertificate, TRUE)) goto fail;
    
    /* Clipboard (enabled) */
    if (!freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE)) goto fail;
    
    /* Dynamic resolution updates */
    if (!freerdp_settings_set_bool(settings, FreeRDP_SupportDisplayControl, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_DynamicResolutionUpdate, TRUE)) goto fail;
    
    /* Register session in the registry for multi-user audio isolation */
    int reg_result = session_registry_add(context, ctx);
    if (reg_result == -2) {
        /* Session limit reached */
        snprintf(ctx->error_msg, MAX_ERROR_LEN, "Session limit reached (%d max)", g_max_sessions);
        ctx->state = RDP_STATE_ERROR;
        goto fail;
    } else if (reg_result != 0) {
        snprintf(ctx->error_msg, MAX_ERROR_LEN, "Failed to register session");
        ctx->state = RDP_STATE_ERROR;
        goto fail;
    }
    
    /* Return context cast as session (context contains instance pointer) */
    return (RdpSession*)context;
    
fail:
    if (context) {
        freerdp_client_context_free(context);
    }
    return NULL;
}

int rdp_connect(RdpSession* session)
{
    rdpContext* context = (rdpContext*)session;
    freerdp* instance = context->instance;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (!context) return -1;
    
    ctx->state = RDP_STATE_CONNECTING;
    
    /* Lock to protect g_audio_ctx handoff to the plugin during connect.
     * The plugin's Open callback reads g_audio_ctx, so we must ensure
     * no other session overwrites it during our connect operation. */
    pthread_mutex_lock(&g_connect_mutex);
    
    /* Set up the global audio context to point to THIS session's buffer.
     * The plugin will read this during its Open callback.
     * Use POINTERS for write_pos, read_pos, initialized so the plugin
     * can update the actual values in BridgeContext. */
    g_audio_ctx.opus_buffer = ctx->opus_buffer;
    g_audio_ctx.opus_buffer_size = ctx->opus_buffer_size;
    g_audio_ctx.opus_write_pos = &ctx->opus_write_pos;
    g_audio_ctx.opus_read_pos = &ctx->opus_read_pos;
    g_audio_ctx.opus_mutex = &ctx->opus_mutex;
    g_audio_ctx.sample_rate = ctx->opus_sample_rate;
    g_audio_ctx.channels = ctx->opus_channels;
    g_audio_ctx.initialized = &ctx->opus_initialized;
    
    if (!freerdp_connect(instance)) {
        pthread_mutex_unlock(&g_connect_mutex);
        UINT32 error = freerdp_get_last_error(context);
        snprintf(ctx->error_msg, MAX_ERROR_LEN, 
                 "Connection failed: 0x%08X", error);
        ctx->state = RDP_STATE_ERROR;
        return -1;
    }
    
    /* Set up audio context for the RDPSND bridge plugin AFTER connecting.
     * The plugin is loaded by FreeRDP during freerdp_connect(), so we need
     * to call rdp_set_audio_context after the plugin is available. */
    rdp_set_audio_context(session);
    
    pthread_mutex_unlock(&g_connect_mutex);
    
    ctx->state = RDP_STATE_CONNECTED;
    return 0;
}

RdpState rdp_get_state(RdpSession* session)
{
    if (!session) return RDP_STATE_DISCONNECTED;
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    return ctx->state;
}

const char* rdp_get_error(RdpSession* session)
{
    if (!session) return "Invalid session";
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    return ctx->error_msg;
}

void rdp_disconnect(RdpSession* session)
{
    if (!session) return;
    rdpContext* context = (rdpContext*)session;
    freerdp* instance = context->instance;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (ctx->state == RDP_STATE_CONNECTED || ctx->state == RDP_STATE_CONNECTING) {
        freerdp_disconnect(instance);
    }
    ctx->state = RDP_STATE_DISCONNECTED;
}

void rdp_destroy(RdpSession* session)
{
    if (!session) return;
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    /* Unregister from session registry BEFORE cleanup */
    session_registry_remove(context);
    
    rdp_disconnect(session);
    
    /* Cleanup transcoder */
    cleanup_transcoder(ctx);
    
    pthread_mutex_destroy(&ctx->rect_mutex);
    pthread_mutex_destroy(&ctx->audio_mutex);
    pthread_mutex_destroy(&ctx->opus_mutex);
    pthread_mutex_destroy(&ctx->h264_mutex);
    pthread_mutex_destroy(&ctx->gfx_mutex);
    
    /* Free H.264 frame queue */
    for (int i = 0; i < RDP_MAX_H264_FRAMES; i++) {
        if (ctx->h264_frames[i].nal_data) {
            free(ctx->h264_frames[i].nal_data);
        }
        if (ctx->h264_frames[i].chroma_nal_data) {
            free(ctx->h264_frames[i].chroma_nal_data);
        }
    }
    
    /* Free audio resources */
    if (ctx->opus_encoder) {
        opus_encoder_destroy(ctx->opus_encoder);
        ctx->opus_encoder = NULL;
    }
    if (ctx->audio_buffer) {
        free(ctx->audio_buffer);
        ctx->audio_buffer = NULL;
    }
    if (ctx->opus_buffer) {
        free(ctx->opus_buffer);
        ctx->opus_buffer = NULL;
    }
    
    freerdp_client_context_free(context);
}

/* ============================================================================
 * Deferred GDI Pipeline Initialization
 * ============================================================================ */

static void maybe_init_gfx_pipeline(BridgeContext* bctx)
{
    /* Check if deferred initialization is needed */
    pthread_mutex_lock(&bctx->gfx_mutex);
    bool needs_init = bctx->gfx_pipeline_needs_init && !bctx->gfx_pipeline_ready;
    RdpgfxClientContext* gfx = bctx->gfx;
    pthread_mutex_unlock(&bctx->gfx_mutex);
    
    if (!needs_init || !gfx) {
        return;
    }
    
    rdpContext* context = (rdpContext*)bctx;
    rdpGdi* gdi = context->gdi;
    
    if (!gdi) {
        fprintf(stderr, "[rdp_bridge] Cannot init GFX pipeline: GDI not available\n");
        return;
    }
    
    /* Check if FreeRDP set up the FrameAcknowledge callback */
    if (!gfx->FrameAcknowledge) {
        fprintf(stderr, "[rdp_bridge] WARNING: FrameAcknowledge callback is NULL - acks won't be sent!\n");
    }
    
    /* PURE GFX MODE: Do NOT call gdi_graphics_pipeline_init()!
     * 
     * Per RDPEGFX spec, when GFX is active we handle graphics ONLY via GFX callbacks.
     * gdi_graphics_pipeline_init registers GDI handlers that expect to run on the
     * main thread and call gdi_OutputUpdate(), causing crashes on the GFX thread.
     * 
     * Instead, we:
     * 1. Keep gdi_init() for the primary framebuffer only
     * 2. Manage GFX surfaces ourselves (surface_buffers array)
     * 3. Decode ClearCodec/Planar using FreeRDP's standalone codec APIs
     * 4. Copy decoded pixels directly to primary_buffer
     * 5. Send FrameAcknowledge ourselves
     */
    
    pthread_mutex_lock(&bctx->gfx_mutex);
    bctx->gdi = gdi;
    bctx->gfx_pipeline_needs_init = false;
    bctx->gfx_pipeline_ready = true;
    pthread_mutex_unlock(&bctx->gfx_mutex);
    
    /* GFX callbacks are already set in bridge_on_channel_connected */
}

/* ============================================================================
 * Event Processing & Frame Capture
 * ============================================================================ */

int rdp_poll(RdpSession* session, int timeout_ms)
{
    if (!session) return -1;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (ctx->state != RDP_STATE_CONNECTED) {
        return -1;
    }
    
    /* Check if we already have updates pending - return immediately */
    pthread_mutex_lock(&ctx->rect_mutex);
    int pending_update = (ctx->dirty_rect_count > 0 || ctx->needs_full_frame) ? 1 : 0;
    pthread_mutex_unlock(&ctx->rect_mutex);
    if (pending_update) {
        return 1;
    }
    
    /* Handle pending resize - use display control channel if available.
     * IMPORTANT: Don't process resize during GFX pipeline init to avoid race conditions. */
    pthread_mutex_lock(&ctx->gfx_mutex);
    bool gfx_initializing = ctx->gfx_pipeline_needs_init && !ctx->gfx_pipeline_ready;
    pthread_mutex_unlock(&ctx->gfx_mutex);
    
    if (ctx->resize_pending && !gfx_initializing) {
        ctx->resize_pending = false;
        
        uint32_t new_width = ctx->pending_width;
        uint32_t new_height = ctx->pending_height;
        
        /* Skip if dimensions haven't actually changed */
        if (ctx->frame_width == (int)new_width && ctx->frame_height == (int)new_height) {
            fprintf(stderr, "[rdp_bridge] Skipping redundant resize in poll\n");
        }
        /* Try to use Display Control channel for dynamic resize */
        else if (ctx->disp && ctx->disp->SendMonitorLayout) {
            DISPLAY_CONTROL_MONITOR_LAYOUT layout = { 0 };
            layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
            layout.Left = 0;
            layout.Top = 0;
            layout.Width = new_width;
            layout.Height = new_height;
            layout.PhysicalWidth = new_width;
            layout.PhysicalHeight = new_height;
            layout.Orientation = ORIENTATION_LANDSCAPE;
            layout.DesktopScaleFactor = 100;
            layout.DeviceScaleFactor = 100;
            
            fprintf(stderr, "[rdp_bridge] Sending MonitorLayout resize to %ux%u\n", new_width, new_height);
            ctx->disp->SendMonitorLayout(ctx->disp, 1, &layout);
            
            /* Mark for full frame after resize - only when we actually send a resize */
            pthread_mutex_lock(&ctx->rect_mutex);
            ctx->needs_full_frame = true;
            pthread_mutex_unlock(&ctx->rect_mutex);
        }
    }
    
    /* Deferred GDI pipeline initialization (safe from main thread) */
    maybe_init_gfx_pipeline(ctx);
    
    /* Get file descriptors for select/poll */
    HANDLE handles[MAXIMUM_WAIT_OBJECTS] = { 0 };
    DWORD nCount = freerdp_get_event_handles(context, handles, ARRAYSIZE(handles));
    
    if (nCount == 0) {
        snprintf(ctx->error_msg, MAX_ERROR_LEN, "Failed to get event handles");
        return -1;
    }
    
    /* Wait for events */
    DWORD waitStatus = WaitForMultipleObjects(nCount, handles, FALSE, (DWORD)timeout_ms);
    
    if (waitStatus == WAIT_FAILED) {
        return 0; /* No events, not an error */
    }
    
    /* Check if connection is still valid */
    if (!freerdp_check_event_handles(context)) {
        UINT32 error = freerdp_get_last_error(context);
        if (error != FREERDP_ERROR_SUCCESS) {
            snprintf(ctx->error_msg, MAX_ERROR_LEN, 
                     "Event handling error: 0x%08X", error);
            ctx->state = RDP_STATE_ERROR;
            
            /* Mark as disconnecting to prevent GDI handler calls from other threads */
            pthread_mutex_lock(&ctx->gfx_mutex);
            ctx->gfx_disconnecting = true;
            pthread_mutex_unlock(&ctx->gfx_mutex);
            
            return -1;
        }
    }
    
    /* Check if we have dirty rects (frame update available) */
    pthread_mutex_lock(&ctx->rect_mutex);
    int has_update = (ctx->dirty_rect_count > 0 || ctx->needs_full_frame) ? 1 : 0;
    pthread_mutex_unlock(&ctx->rect_mutex);
    
    return has_update;
}

/* Lock the frame buffer to prevent reallocation during read.
 * MUST call rdp_unlock_frame_buffer() after reading!
 * For high-performance direct buffer access. */
void rdp_lock_frame_buffer(RdpSession* session)
{
    if (!session) return;
    BridgeContext* ctx = (BridgeContext*)session;
    pthread_mutex_lock(&ctx->gfx_mutex);
}

/* Unlock the frame buffer after reading */
void rdp_unlock_frame_buffer(RdpSession* session)
{
    if (!session) return;
    BridgeContext* ctx = (BridgeContext*)session;
    pthread_mutex_unlock(&ctx->gfx_mutex);
}

/* Get frame buffer pointer - CALLER MUST hold lock via rdp_lock_frame_buffer()! */
uint8_t* rdp_get_frame_buffer(RdpSession* session, int* width, int* height, int* stride)
{
    if (!session) return NULL;
    
    BridgeContext* ctx = (BridgeContext*)session;
    
    /* In pure GFX mode, use the GFX surface buffer directly */
    if (ctx->gfx_active) {
        uint16_t primary_id = ctx->primary_surface_id;
        
        if (primary_id < RDP_MAX_GFX_SURFACES && 
            ctx->surfaces[primary_id].active &&
            ctx->surface_buffers[primary_id]) {
            
            uint32_t w = ctx->surfaces[primary_id].width;
            uint32_t h = ctx->surfaces[primary_id].height;
            uint8_t* buf = ctx->surface_buffers[primary_id];
            
            if (width) *width = w;
            if (height) *height = h;
            if (stride) *stride = w * 4;  /* BGRA32, no padding */
            
            return buf;
        }
        return NULL;
    }
    
    /* Fallback to GDI for non-GFX mode */
    rdpContext* context = (rdpContext*)session;
    rdpGdi* gdi = context->gdi;
    
    if (!gdi || !gdi->primary_buffer) {
        return NULL;
    }
    
    if (width) *width = gdi->width;
    if (height) *height = gdi->height;
    if (stride) *stride = gdi->stride;
    
    return gdi->primary_buffer;
}

int rdp_get_dirty_rects(RdpSession* session, RdpRect* rects, int max_rects)
{
    if (!session || !rects || max_rects <= 0) return -1;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    pthread_mutex_lock(&ctx->rect_mutex);
    
    /* Issue 12m: Atomic read-and-clear to prevent race condition.
     * Previously, Python would read rects then call clear_dirty_rects() separately.
     * Between these calls, a new frame could start and add rects, which would
     * then be incorrectly cleared. Now we read AND clear in one atomic operation. */
    
    int count = ctx->dirty_rect_count;
    if (count > max_rects) count = max_rects;
    
    for (int i = 0; i < count; i++) {
        rects[i] = ctx->dirty_rects[i];
    }
    
    /* Clear only the rects we returned - shift remaining rects down */
    if (count > 0 && count < ctx->dirty_rect_count) {
        /* Partial read - shift remaining rects to front */
        int remaining = ctx->dirty_rect_count - count;
        for (int i = 0; i < remaining; i++) {
            ctx->dirty_rects[i] = ctx->dirty_rects[count + i];
        }
        ctx->dirty_rect_count = remaining;
    } else {
        /* Full read or exact match - clear all */
        ctx->dirty_rect_count = 0;
    }
    
    pthread_mutex_unlock(&ctx->rect_mutex);
    
    return count;
}

int rdp_peek_dirty_rect_count(RdpSession* session)
{
    if (!session) return 0;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    pthread_mutex_lock(&ctx->rect_mutex);
    int count = ctx->dirty_rect_count;
    pthread_mutex_unlock(&ctx->rect_mutex);
    
    return count;
}

void rdp_clear_dirty_rects(RdpSession* session)
{
    if (!session) return;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    pthread_mutex_lock(&ctx->rect_mutex);
    ctx->dirty_rect_count = 0;
    pthread_mutex_unlock(&ctx->rect_mutex);
}

bool rdp_needs_full_frame(RdpSession* session)
{
    if (!session) return false;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    pthread_mutex_lock(&ctx->rect_mutex);
    
    /* Note: We allow checking/consuming full_frame flag even while a frame
     * is in progress. Blocking here caused stalls when frames arrived faster
     * than Python could process them. The flag is set atomically under mutex. */
    
    bool needs = ctx->needs_full_frame;
    ctx->needs_full_frame = false;
    pthread_mutex_unlock(&ctx->rect_mutex);
    
    return needs;
}

bool rdp_gfx_frame_in_progress(RdpSession* session)
{
    if (!session) return false;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    pthread_mutex_lock(&ctx->rect_mutex);
    bool in_progress = ctx->gfx_frame_in_progress;
    pthread_mutex_unlock(&ctx->rect_mutex);
    
    return in_progress;
}

uint32_t rdp_gfx_get_last_completed_frame(RdpSession* session)
{
    if (!session) return 0;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    pthread_mutex_lock(&ctx->rect_mutex);
    uint32_t frame_id = ctx->last_completed_frame_id;
    pthread_mutex_unlock(&ctx->rect_mutex);
    
    return frame_id;
}

/* ============================================================================
 * Input Handling
 * ============================================================================ */

void rdp_send_mouse(RdpSession* session, uint16_t flags, int x, int y)
{
    if (!session) return;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (ctx->state != RDP_STATE_CONNECTED) return;
    
    rdpInput* input = context->input;
    if (input && input->MouseEvent) {
        input->MouseEvent(input, flags, (UINT16)x, (UINT16)y);
    }
}

void rdp_send_keyboard(RdpSession* session, uint16_t flags, uint16_t scancode)
{
    if (!session) return;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (ctx->state != RDP_STATE_CONNECTED) return;
    
    rdpInput* input = context->input;
    if (input && input->KeyboardEvent) {
        input->KeyboardEvent(input, flags, scancode);
    }
}

void rdp_send_unicode(RdpSession* session, uint16_t flags, uint16_t code)
{
    if (!session) return;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (ctx->state != RDP_STATE_CONNECTED) return;
    
    rdpInput* input = context->input;
    if (input && input->UnicodeKeyboardEvent) {
        input->UnicodeKeyboardEvent(input, flags, code);
    }
}

/* ============================================================================
 * Resize
 * ============================================================================ */

int rdp_resize(RdpSession* session, uint32_t width, uint32_t height)
{
    if (!session) return -1;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (ctx->state != RDP_STATE_CONNECTED) {
        return -1;
    }
    
    /* Skip redundant resize requests - these can cause race conditions
     * with GFX pipeline initialization during early connection */
    if (ctx->frame_width == (int)width && ctx->frame_height == (int)height) {
        fprintf(stderr, "[rdp_bridge] Skipping redundant resize to %ux%u\n", width, height);
        return 0;
    }
    
    fprintf(stderr, "[rdp_bridge] Queuing resize from %dx%d to %ux%u\n",
            ctx->frame_width, ctx->frame_height, width, height);
    
    /* Queue resize for next poll - don't set needs_full_frame here!
     * The bridge_desktop_resize callback will set it AFTER the resize completes.
     * This prevents sending a frame before the resize is processed. */
    pthread_mutex_lock(&ctx->rect_mutex);
    ctx->resize_pending = true;
    ctx->pending_width = width;
    ctx->pending_height = height;
    /* Clear dirty rects since they're for the old size */
    ctx->dirty_rect_count = 0;
    pthread_mutex_unlock(&ctx->rect_mutex);
    
    return 0;
}

/* ============================================================================
 * FreeRDP Callbacks
 * ============================================================================ */

static BOOL bridge_pre_connect(freerdp* instance)
{
    rdpSettings* settings = instance->context->settings;
    
    /* Ensure we have proper settings */
    if (!freerdp_settings_get_string(settings, FreeRDP_ServerHostname)) {
        return FALSE;
    }
    
    /* Load required channels using FreeRDP3 API */
    if (!freerdp_client_load_channels(instance)) {
        /* Try without channels if loading fails */
    }
    
    return TRUE;
}

static BOOL bridge_post_connect(freerdp* instance)
{
    BridgeContext* ctx = (BridgeContext*)instance->context;
    rdpContext* context = instance->context;
    
    /* Initialize GDI for software rendering */
    if (!gdi_init(instance, PIXEL_FORMAT_BGRA32)) {
        snprintf(ctx->error_msg, MAX_ERROR_LEN, "GDI initialization failed");
        return FALSE;
    }
    
    rdpGdi* gdi = context->gdi;
    
    /* Note: GFX pipeline initialization is handled via channel connection callback.
     * The gdi_graphics_pipeline_init() requires RdpgfxClientContext which is
     * obtained when the RDPGFX channel connects. For now, we rely on GDI mode
     * with GFX pipeline enabled in settings - the actual pipeline init happens
     * in bridge_on_channel_connected when RDPGFX channel connects. */
    
    /* Set up our paint callbacks */
    context->update->BeginPaint = bridge_begin_paint;
    context->update->EndPaint = bridge_end_paint;
    context->update->DesktopResize = bridge_desktop_resize;
    
    /* Store frame dimensions */
    ctx->frame_width = gdi->width;
    ctx->frame_height = gdi->height;
    ctx->frame_stride = gdi->stride;
    ctx->frame_buffer = gdi->primary_buffer;
    
    /* Subscribe to channel events to capture Display Control DVC when it connects */
    PubSub_SubscribeChannelConnected(context->pubSub, bridge_on_channel_connected);
    PubSub_SubscribeChannelDisconnected(context->pubSub, bridge_on_channel_disconnected);
    
    /* GFX pipeline is enabled for H.264/progressive codec support */
    
    /* Mark for full frame update - ensures first frame is sent */
    pthread_mutex_lock(&ctx->rect_mutex);
    ctx->needs_full_frame = true;
    ctx->dirty_rect_count = 0;
    pthread_mutex_unlock(&ctx->rect_mutex);
    
    return TRUE;
}

static void bridge_post_disconnect(freerdp* instance)
{
    rdpContext* context = instance->context;
    BridgeContext* ctx = (BridgeContext*)context;
    
    /* Unsubscribe from channel events */
    PubSub_UnsubscribeChannelConnected(context->pubSub, bridge_on_channel_connected);
    PubSub_UnsubscribeChannelDisconnected(context->pubSub, bridge_on_channel_disconnected);
    
    ctx->disp = NULL;
    ctx->gfx = NULL;
    ctx->rdpsnd = NULL;
    ctx->audio_initialized = false;
    ctx->state = RDP_STATE_DISCONNECTED;
    ctx->frame_buffer = NULL;
    
    /* Free codec decoders */
    if (ctx->clear_decoder) {
        clear_context_free(ctx->clear_decoder);
        ctx->clear_decoder = NULL;
    }
    if (ctx->planar_decoder) {
        freerdp_bitmap_planar_context_free(ctx->planar_decoder);
        ctx->planar_decoder = NULL;
    }
    if (ctx->progressive_decoder) {
        progressive_context_free(ctx->progressive_decoder);
        ctx->progressive_decoder = NULL;
    }
    
    /* Free surface buffers */
    for (int i = 0; i < RDP_MAX_GFX_SURFACES; i++) {
        if (ctx->surface_buffers[i]) {
            free(ctx->surface_buffers[i]);
            ctx->surface_buffers[i] = NULL;
        }
    }
    
    /* Free GFX cache */
    if (ctx->gfx_cache) {
        for (int i = 0; i < ctx->gfx_cache_size; i++) {
            if (ctx->gfx_cache[i].data) {
                free(ctx->gfx_cache[i].data);
            }
        }
        free(ctx->gfx_cache);
        ctx->gfx_cache = NULL;
    }
    
    gdi_free(instance);
}

static void bridge_on_channel_connected(void* ctx, const ChannelConnectedEventArgs* e)
{
    rdpContext* context = (rdpContext*)ctx;
    BridgeContext* bctx = (BridgeContext*)context;
    
    if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
        bctx->disp = (DispClientContext*)e->pInterface;
    }
    else if (strcmp(e->name, RDPSND_CHANNEL_NAME) == 0) {
        /* Audio channel connected - initialize audio buffer */
        pthread_mutex_lock(&bctx->audio_mutex);
        if (!bctx->audio_buffer) {
            /* 1 second buffer at 48kHz stereo 16-bit = 192KB */
            bctx->audio_buffer_size = 48000 * 2 * 2;
            bctx->audio_buffer = (uint8_t*)calloc(1, bctx->audio_buffer_size);
            bctx->audio_buffer_pos = 0;
            bctx->audio_read_pos = 0;
        }
        /* Default format - will be updated when audio format is received */
        bctx->audio_sample_rate = 48000;
        bctx->audio_channels = 2;
        bctx->audio_bits = 16;
        bctx->audio_initialized = true;
        pthread_mutex_unlock(&bctx->audio_mutex);
        
        /* Mark Opus audio as initialized for native streaming */
        pthread_mutex_lock(&bctx->opus_mutex);
        bctx->opus_initialized = 1;
        pthread_mutex_unlock(&bctx->opus_mutex);
    }
    else if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
        /* GFX pipeline connected - save context and set deferred init flag.
         * 
         * We do NOT call gdi_graphics_pipeline_init() here because it causes
         * thread-safety issues (GDI reinit in different thread).
         * Instead, we set a flag and initialize from the main poll thread.
         */
        RdpgfxClientContext* gfx = (RdpgfxClientContext*)e->pInterface;
        bctx->gfx = gfx;
        
        if (gfx) {
            /* Store our context for callbacks */
            gfx->custom = bctx;
            
            pthread_mutex_lock(&bctx->gfx_mutex);
            bctx->gfx_active = true;
            bctx->gfx_pipeline_needs_init = true;  /* Deferred init in main thread */
            pthread_mutex_unlock(&bctx->gfx_mutex);
            
            /* Set up ALL GFX callbacks for proper protocol handling.
             * Missing callbacks can cause the server to abort the connection.
             * 
             * PURE GFX MODE: We handle graphics via RDPEGFX callbacks only.
             * H.264/AVC frames are captured and passed to WebSocket clients.
             * Non-H.264 codecs are decoded to the primary buffer.
             */
            gfx->CapsConfirm = gfx_on_caps_confirm;
            gfx->ResetGraphics = gfx_on_reset_graphics;
            gfx->StartFrame = gfx_on_start_frame;
            gfx->EndFrame = gfx_on_end_frame;
            gfx->SurfaceCommand = gfx_on_surface_command;
            gfx->CreateSurface = gfx_on_create_surface;
            gfx->DeleteSurface = gfx_on_delete_surface;
            gfx->MapSurfaceToOutput = gfx_on_map_surface;
            gfx->MapSurfaceToScaledOutput = gfx_on_map_surface_scaled;
            gfx->MapSurfaceToWindow = gfx_on_map_surface_window;
            gfx->MapSurfaceToScaledWindow = gfx_on_map_surface_scaled_window;
            gfx->SolidFill = gfx_on_solid_fill;
            gfx->SurfaceToSurface = gfx_on_surface_to_surface;
            gfx->SurfaceToCache = gfx_on_surface_to_cache;
            gfx->CacheToSurface = gfx_on_cache_to_surface;
            gfx->EvictCacheEntry = gfx_on_evict_cache;
            gfx->DeleteEncodingContext = gfx_on_delete_encoding_context;
            gfx->CacheImportReply = gfx_on_cache_import_reply;
        }
    }
}

static void bridge_on_channel_disconnected(void* ctx, const ChannelDisconnectedEventArgs* e)
{
    rdpContext* context = (rdpContext*)ctx;
    BridgeContext* bctx = (BridgeContext*)context;
    
    if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
        bctx->disp = NULL;
    }
    else if (strcmp(e->name, RDPSND_CHANNEL_NAME) == 0) {
        pthread_mutex_lock(&bctx->audio_mutex);
        bctx->audio_initialized = false;
        pthread_mutex_unlock(&bctx->audio_mutex);
        
        pthread_mutex_lock(&bctx->opus_mutex);
        bctx->opus_initialized = 0;
        pthread_mutex_unlock(&bctx->opus_mutex);
    }
    else if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
        pthread_mutex_lock(&bctx->gfx_mutex);
        if (bctx->gfx_active && context->gdi) {
            gdi_graphics_pipeline_uninit(context->gdi, bctx->gfx);
        }
        bctx->gfx = NULL;
        bctx->gfx_active = false;
        pthread_mutex_unlock(&bctx->gfx_mutex);
        
        /* Clear H.264 frame queue */
        pthread_mutex_lock(&bctx->h264_mutex);
        for (int i = 0; i < RDP_MAX_H264_FRAMES; i++) {
            if (bctx->h264_frames[i].nal_data) {
                free(bctx->h264_frames[i].nal_data);
                bctx->h264_frames[i].nal_data = NULL;
            }
            if (bctx->h264_frames[i].chroma_nal_data) {
                free(bctx->h264_frames[i].chroma_nal_data);
                bctx->h264_frames[i].chroma_nal_data = NULL;
            }
            bctx->h264_frames[i].valid = false;
        }
        bctx->h264_count = 0;
        bctx->h264_write_idx = 0;
        bctx->h264_read_idx = 0;
        pthread_mutex_unlock(&bctx->h264_mutex);
    }
}

static BOOL bridge_begin_paint(rdpContext* context)
{
    rdpGdi* gdi = context->gdi;
    
    if (!gdi || !gdi->primary || !gdi->primary->hdc) {
        return FALSE;
    }
    
    /* Clear the invalid region before painting */
    HGDI_WND hwnd = gdi->primary->hdc->hwnd;
    if (hwnd) {
        hwnd->invalid->null = TRUE;
        hwnd->ninvalid = 0;
    }
    
    return TRUE;
}

static BOOL bridge_end_paint(rdpContext* context)
{
    BridgeContext* ctx = (BridgeContext*)context;
    rdpGdi* gdi = context->gdi;
    
    if (!gdi || !gdi->primary || !gdi->primary->hdc) {
        return FALSE;
    }
    
    HGDI_WND hwnd = gdi->primary->hdc->hwnd;
    if (!hwnd || hwnd->invalid->null) {
        return TRUE; /* No invalid region */
    }
    
    pthread_mutex_lock(&ctx->rect_mutex);
    
    /* Add the invalid region as a dirty rect */
    if (ctx->dirty_rect_count < RDP_MAX_DIRTY_RECTS) {
        RdpRect* rect = &ctx->dirty_rects[ctx->dirty_rect_count];
        rect->x = hwnd->invalid->x;
        rect->y = hwnd->invalid->y;
        rect->width = hwnd->invalid->w;
        rect->height = hwnd->invalid->h;
        ctx->dirty_rect_count++;
    } else {
        /* Too many rects, request full frame */
        ctx->needs_full_frame = true;
        ctx->dirty_rect_count = 0;
    }
    
    pthread_mutex_unlock(&ctx->rect_mutex);
    
    return TRUE;
}

static BOOL bridge_desktop_resize(rdpContext* context)
{
    BridgeContext* ctx = (BridgeContext*)context;
    rdpGdi* gdi = context->gdi;
    
    /* NOTE: When called from gdi_ResetGraphics (GFX Reset PDU), the GDI layer
     * handles its own resize internally. We must NOT call gdi_free/gdi_init here
     * as that would corrupt the GDI state that gdi_ResetGraphics is still using.
     * 
     * When called from standard DesktopResize update (non-GFX), gdi_resize() 
     * should be used instead of gdi_free/gdi_init.
     * 
     * For now, we just update our cached dimensions from the current GDI state. */
    
    if (!gdi) {
        fprintf(stderr, "[rdp_bridge] DesktopResize: GDI not available\n");
        return FALSE;
    }
    
    fprintf(stderr, "[rdp_bridge] DesktopResize: %dx%d\n", gdi->width, gdi->height);
    
    /* Update stored dimensions from current GDI state */
    ctx->frame_width = gdi->width;
    ctx->frame_height = gdi->height;
    ctx->frame_stride = gdi->stride;
    ctx->frame_buffer = gdi->primary_buffer;
    
    /* Mark for full frame update */
    pthread_mutex_lock(&ctx->rect_mutex);
    ctx->needs_full_frame = true;
    ctx->dirty_rect_count = 0;
    pthread_mutex_unlock(&ctx->rect_mutex);
    
    return TRUE;
}

/* ============================================================================
 * AVC444 Transcoder (4:4:4 → 4:2:0 for browser compatibility)
 * 
 * Per MS-RDPEGFX spec, AVC444 uses two H.264 streams:
 * - Stream 0: Luma (Y plane) encoded as YUV420
 * - Stream 1: Chroma (U/V planes) encoded separately at full resolution
 * 
 * Browsers only decode standard YUV420, so we must:
 * 1. Decode both streams
 * 2. Combine into YUV444
 * 3. Convert to YUV420
 * 4. Re-encode to H.264
 * ============================================================================ */

static bool init_transcoder(BridgeContext* ctx, int width, int height)
{
    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    
    if (!decoder || !encoder) {
        fprintf(stderr, "[rdp_bridge] H.264 codec not found\n");
        return false;
    }
    
    /* Luma decoder */
    ctx->avc_decoder_luma = avcodec_alloc_context3(decoder);
    if (!ctx->avc_decoder_luma) goto fail;
    
    ctx->avc_decoder_luma->thread_count = 2;  /* Low latency */
    ctx->avc_decoder_luma->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->avc_decoder_luma->flags2 |= AV_CODEC_FLAG2_FAST;
    
    if (avcodec_open2(ctx->avc_decoder_luma, decoder, NULL) < 0) {
        fprintf(stderr, "[rdp_bridge] Failed to open luma decoder\n");
        goto fail;
    }
    
    /* Chroma decoder (separate instance for AVC444) */
    ctx->avc_decoder_chroma = avcodec_alloc_context3(decoder);
    if (!ctx->avc_decoder_chroma) goto fail;
    
    ctx->avc_decoder_chroma->thread_count = 2;
    ctx->avc_decoder_chroma->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->avc_decoder_chroma->flags2 |= AV_CODEC_FLAG2_FAST;
    
    if (avcodec_open2(ctx->avc_decoder_chroma, decoder, NULL) < 0) {
        fprintf(stderr, "[rdp_bridge] Failed to open chroma decoder\n");
        goto fail;
    }
    
    /* H.264 encoder for output (4:2:0) */
    ctx->avc_encoder = avcodec_alloc_context3(encoder);
    if (!ctx->avc_encoder) goto fail;
    
    ctx->avc_encoder->width = width;
    ctx->avc_encoder->height = height;
    ctx->avc_encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->avc_encoder->time_base = (AVRational){1, 60};
    ctx->avc_encoder->framerate = (AVRational){60, 1};
    ctx->avc_encoder->thread_count = 2;
    ctx->avc_encoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->avc_encoder->max_b_frames = 0;  /* No B-frames for low latency */
    ctx->avc_encoder->gop_size = 60;     /* Keyframe every 60 frames */
    
    /* Tune for low latency (zerolatency preset equivalent) */
    AVDictionary* opts = NULL;
    av_dict_set(&opts, "preset", "ultrafast", 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
    av_dict_set(&opts, "crf", "23", 0);
    
    if (avcodec_open2(ctx->avc_encoder, encoder, &opts) < 0) {
        fprintf(stderr, "[rdp_bridge] Failed to open H.264 encoder\n");
        av_dict_free(&opts);
        goto fail;
    }
    av_dict_free(&opts);
    
    /* Allocate frames */
    ctx->decoded_frame_luma = av_frame_alloc();
    ctx->decoded_frame_chroma = av_frame_alloc();
    ctx->combined_frame = av_frame_alloc();
    ctx->output_frame = av_frame_alloc();
    ctx->encode_pkt = av_packet_alloc();
    
    if (!ctx->decoded_frame_luma || !ctx->decoded_frame_chroma || 
        !ctx->combined_frame || !ctx->output_frame || !ctx->encode_pkt) {
        goto fail;
    }
    
    /* Setup output frame (YUV420P) */
    ctx->output_frame->format = AV_PIX_FMT_YUV420P;
    ctx->output_frame->width = width;
    ctx->output_frame->height = height;
    if (av_frame_get_buffer(ctx->output_frame, 0) < 0) {
        fprintf(stderr, "[rdp_bridge] Failed to allocate output frame buffer\n");
        goto fail;
    }
    
    /* Setup combined frame (YUV444P for intermediate) */
    ctx->combined_frame->format = AV_PIX_FMT_YUV444P;
    ctx->combined_frame->width = width;
    ctx->combined_frame->height = height;
    if (av_frame_get_buffer(ctx->combined_frame, 0) < 0) {
        fprintf(stderr, "[rdp_bridge] Failed to allocate combined frame buffer\n");
        goto fail;
    }
    
    /* Create scaler for YUV444 → YUV420 conversion */
    ctx->sws_ctx = sws_getContext(width, height, AV_PIX_FMT_YUV444P,
                                   width, height, AV_PIX_FMT_YUV420P,
                                   SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!ctx->sws_ctx) {
        fprintf(stderr, "[rdp_bridge] Failed to create scaler context\n");
        goto fail;
    }
    
    ctx->transcoder_initialized = true;
    return true;
    
fail:
    /* Cleanup on failure */
    if (ctx->avc_decoder_luma) { avcodec_free_context(&ctx->avc_decoder_luma); }
    if (ctx->avc_decoder_chroma) { avcodec_free_context(&ctx->avc_decoder_chroma); }
    if (ctx->avc_encoder) { avcodec_free_context(&ctx->avc_encoder); }
    if (ctx->decoded_frame_luma) { av_frame_free(&ctx->decoded_frame_luma); }
    if (ctx->decoded_frame_chroma) { av_frame_free(&ctx->decoded_frame_chroma); }
    if (ctx->combined_frame) { av_frame_free(&ctx->combined_frame); }
    if (ctx->output_frame) { av_frame_free(&ctx->output_frame); }
    if (ctx->encode_pkt) { av_packet_free(&ctx->encode_pkt); }
    if (ctx->sws_ctx) { sws_freeContext(ctx->sws_ctx); ctx->sws_ctx = NULL; }
    return false;
}

static void cleanup_transcoder(BridgeContext* ctx)
{
    if (ctx->avc_decoder_luma) { avcodec_free_context(&ctx->avc_decoder_luma); }
    if (ctx->avc_decoder_chroma) { avcodec_free_context(&ctx->avc_decoder_chroma); }
    if (ctx->avc_encoder) { avcodec_free_context(&ctx->avc_encoder); }
    if (ctx->decoded_frame_luma) { av_frame_free(&ctx->decoded_frame_luma); }
    if (ctx->decoded_frame_chroma) { av_frame_free(&ctx->decoded_frame_chroma); }
    if (ctx->combined_frame) { av_frame_free(&ctx->combined_frame); }
    if (ctx->output_frame) { av_frame_free(&ctx->output_frame); }
    if (ctx->encode_pkt) { av_packet_free(&ctx->encode_pkt); }
    if (ctx->sws_ctx) { sws_freeContext(ctx->sws_ctx); ctx->sws_ctx = NULL; }
    ctx->transcoder_initialized = false;
}

/* Transcode AVC444 (luma + chroma streams) to standard 4:2:0 H.264 */
static bool transcode_avc444(BridgeContext* ctx,
                             const uint8_t* luma_data, uint32_t luma_size,
                             const uint8_t* chroma_data, uint32_t chroma_size,
                             uint8_t** out_data, uint32_t* out_size)
{
    if (!ctx->transcoder_initialized) {
        fprintf(stderr, "[rdp_bridge] Transcoder not initialized\n");
        return false;
    }
    
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;
    
    int ret;
    bool got_luma = false;
    bool got_chroma = false;
    
    /* Decode luma stream */
    pkt->data = (uint8_t*)luma_data;
    pkt->size = luma_size;
    
    ret = avcodec_send_packet(ctx->avc_decoder_luma, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        av_packet_free(&pkt);
        return false;
    }
    
    ret = avcodec_receive_frame(ctx->avc_decoder_luma, ctx->decoded_frame_luma);
    if (ret == 0) {
        got_luma = true;
    }
    
    /* Decode chroma stream if present */
    if (chroma_data && chroma_size > 0) {
        pkt->data = (uint8_t*)chroma_data;
        pkt->size = chroma_size;
        
        ret = avcodec_send_packet(ctx->avc_decoder_chroma, pkt);
        if (ret == 0 || ret == AVERROR(EAGAIN)) {
            ret = avcodec_receive_frame(ctx->avc_decoder_chroma, ctx->decoded_frame_chroma);
            if (ret == 0) {
                got_chroma = true;
            }
        }
    }
    
    av_packet_free(&pkt);
    
    if (!got_luma) {
        /* No frame decoded yet (buffering), pass through luma data as-is
         * This happens during initial keyframe build-up */
        *out_data = malloc(luma_size);
        if (*out_data) {
            memcpy(*out_data, luma_data, luma_size);
            *out_size = luma_size;
            return true;
        }
        return false;
    }
    
    /* Combine luma and chroma into YUV444 frame */
    AVFrame* luma = ctx->decoded_frame_luma;
    AVFrame* combined = ctx->combined_frame;
    
    /* Safety check: ensure decoded frame fits in our pre-allocated buffers.
     * If the frame dimensions changed (e.g., after resize), we need to bail out
     * rather than cause a buffer overflow. The transcoder should be reset on resize. */
    if (luma->width > combined->width || luma->height > combined->height) {
        fprintf(stderr, "[rdp_bridge] Transcoder dimension mismatch: decoded=%dx%d, buffer=%dx%d\n",
                luma->width, luma->height, combined->width, combined->height);
        /* Pass through luma data as-is rather than crash */
        *out_data = malloc(luma_size);
        if (*out_data) {
            memcpy(*out_data, luma_data, luma_size);
            *out_size = luma_size;
            return true;
        }
        return false;
    }
    
    /* Copy Y plane from luma frame */
    for (int y = 0; y < luma->height; y++) {
        memcpy(combined->data[0] + y * combined->linesize[0],
               luma->data[0] + y * luma->linesize[0],
               luma->width);
    }
    
    if (got_chroma) {
        /* AVC444: Use decoded chroma for U/V at full resolution */
        AVFrame* chroma = ctx->decoded_frame_chroma;
        
        /* Safety check for chroma dimensions */
        if (chroma->width > combined->width || chroma->height > combined->height) {
            fprintf(stderr, "[rdp_bridge] Chroma dimension mismatch: %dx%d vs %dx%d\n",
                    chroma->width, chroma->height, combined->width, combined->height);
            /* Skip chroma copy, use luma-only fallback below */
            got_chroma = false;
        }
        
        if (got_chroma) {
            /* The chroma stream contains U and V at full resolution */
            for (int y = 0; y < chroma->height; y++) {
                /* Copy U plane */
                memcpy(combined->data[1] + y * combined->linesize[1],
                       chroma->data[1] + y * chroma->linesize[1],
                       chroma->width);
                /* Copy V plane */
                memcpy(combined->data[2] + y * combined->linesize[2],
                       chroma->data[2] + y * chroma->linesize[2],
                       chroma->width);
            }
        }
    }
    
    if (!got_chroma) {
        /* No chroma: upscale 4:2:0 chroma from luma frame to 4:4:4 */
        for (int y = 0; y < luma->height; y++) {
            for (int x = 0; x < luma->width; x++) {
                int src_y = y / 2;
                int src_x = x / 2;
                if (src_y < luma->height / 2 && src_x < luma->width / 2) {
                    combined->data[1][y * combined->linesize[1] + x] =
                        luma->data[1][src_y * luma->linesize[1] + src_x];
                    combined->data[2][y * combined->linesize[2] + x] =
                        luma->data[2][src_y * luma->linesize[2] + src_x];
                }
            }
        }
    }
    
    /* Convert YUV444 → YUV420 */
    av_frame_make_writable(ctx->output_frame);
    sws_scale(ctx->sws_ctx,
              (const uint8_t* const*)combined->data, combined->linesize,
              0, combined->height,
              ctx->output_frame->data, ctx->output_frame->linesize);
    
    ctx->output_frame->pts = luma->pts;
    
    /* Encode to H.264 */
    ret = avcodec_send_frame(ctx->avc_encoder, ctx->output_frame);
    if (ret < 0) {
        fprintf(stderr, "[rdp_bridge] Encode send failed: %d\n", ret);
        return false;
    }
    
    ret = avcodec_receive_packet(ctx->avc_encoder, ctx->encode_pkt);
    if (ret == AVERROR(EAGAIN)) {
        /* Encoder buffering - pass through luma */
        *out_data = malloc(luma_size);
        if (*out_data) {
            memcpy(*out_data, luma_data, luma_size);
            *out_size = luma_size;
            return true;
        }
        return false;
    } else if (ret < 0) {
        fprintf(stderr, "[rdp_bridge] Encode receive failed: %d\n", ret);
        return false;
    }
    
    /* Copy encoded data */
    *out_data = malloc(ctx->encode_pkt->size);
    if (!*out_data) {
        av_packet_unref(ctx->encode_pkt);
        return false;
    }
    memcpy(*out_data, ctx->encode_pkt->data, ctx->encode_pkt->size);
    *out_size = ctx->encode_pkt->size;
    
    av_packet_unref(ctx->encode_pkt);
    return true;
}

/* ============================================================================
 * GFX Pipeline Callbacks (RDPEGFX for H.264/AVC444)
 * ============================================================================ */

static UINT gfx_on_caps_confirm(RdpgfxClientContext* context, const RDPGFX_CAPS_CONFIRM_PDU* caps)
{
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx) return ERROR_INVALID_PARAMETER;
    
    UINT32 version = caps->capsSet->version;
    UINT32 flags = caps->capsSet->flags;
    
    /* Decode version */
    const char* version_str = "Unknown";
    switch (version) {
        case RDPGFX_CAPVERSION_8:    version_str = "8.0"; break;
        case RDPGFX_CAPVERSION_81:   version_str = "8.1"; break;
        case RDPGFX_CAPVERSION_10:   version_str = "10.0"; break;
        case RDPGFX_CAPVERSION_101:  version_str = "10.1"; break;
        case RDPGFX_CAPVERSION_102:  version_str = "10.2"; break;
        case RDPGFX_CAPVERSION_103:  version_str = "10.3"; break;
        case RDPGFX_CAPVERSION_104:  version_str = "10.4"; break;
        case RDPGFX_CAPVERSION_105:  version_str = "10.5"; break;
        case RDPGFX_CAPVERSION_106:  version_str = "10.6"; break;
        case RDPGFX_CAPVERSION_107:  version_str = "10.7"; break;
    }
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_reset_graphics(RdpgfxClientContext* context, const RDPGFX_RESET_GRAPHICS_PDU* reset)
{
    /* PURE GFX MODE: Handle reset ourselves, no GDI chaining */
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx) return ERROR_INVALID_PARAMETER;
    
    /* Reset the AVC444 transcoder on resize.
     * The transcoder's encoder/frames are sized for the original resolution.
     * If we resize to a larger resolution, the new decoded frames will overflow
     * the transcoder's internal buffers, causing a crash.
     * Cleaning up here allows re-initialization with new dimensions. */
    if (bctx->transcoder_initialized) {
        cleanup_transcoder(bctx);
    }
    
    /* NOTE: ClearCodec decoder is session-level and should NOT be reset here.
     * Its internal caches (VBar, ShortVBar, Glyph) build up over the session
     * and the server expects the client to retain them. */
    
    /* Clear all our surface tracking - but NOT the bitmap cache!
     * The GFX bitmap cache and ClearCodec internal state are session-level,
     * they persist across surface resets. Only surfaces are reset. */
    pthread_mutex_lock(&bctx->gfx_mutex);
    for (int i = 0; i < RDP_MAX_GFX_SURFACES; i++) {
        bctx->surfaces[i].active = false;
        if (bctx->surface_buffers[i]) {
            free(bctx->surface_buffers[i]);
            bctx->surface_buffers[i] = NULL;
        }
    }
    bctx->primary_surface_id = 0;
    pthread_mutex_unlock(&bctx->gfx_mutex);
    
    /* NOTE: Do NOT clear gfx_cache here! The bitmap cache persists across
     * surface resets. Server will continue to reference cached entries. */
    
    /* PURE GFX MODE: Do NOT touch gdi->primary_buffer.
     * We only use surface_buffers[] which are managed via CreateSurface/DeleteSurface.
     * The server will send fresh content to the new surface. */
    
    /* Update frame dimensions.
     * NOTE: Do NOT set needs_full_frame here! The buffer is now cleared (black),
     * and the server hasn't sent new content yet. If we set needs_full_frame,
     * Python will send a black frame before real content arrives.
     * The dirty rect overflow mechanism will naturally trigger a full frame
     * when the first big repaint comes in (typically 700+ commands). */
    pthread_mutex_lock(&bctx->rect_mutex);
    bctx->frame_width = reset->width;
    bctx->frame_height = reset->height;
    /* bctx->needs_full_frame = true; -- DON'T do this, causes black frame flash */
    bctx->dirty_rect_count = 0;  /* Clear any stale dirty rects */
    pthread_mutex_unlock(&bctx->rect_mutex);
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_create_surface(RdpgfxClientContext* context, const RDPGFX_CREATE_SURFACE_PDU* create)
{
    /* PURE GFX MODE: Track surfaces ourselves, no GDI chaining */
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx) return ERROR_INVALID_PARAMETER;
    
    pthread_mutex_lock(&bctx->gfx_mutex);
    
    /* Find slot for this surface */
    if (create->surfaceId < RDP_MAX_GFX_SURFACES) {
        bctx->surfaces[create->surfaceId].surface_id = create->surfaceId;
        bctx->surfaces[create->surfaceId].width = create->width;
        bctx->surfaces[create->surfaceId].height = create->height;
        bctx->surfaces[create->surfaceId].pixel_format = create->pixelFormat;
        bctx->surfaces[create->surfaceId].active = true;
        bctx->surfaces[create->surfaceId].mapped_to_output = false;
        bctx->surfaces[create->surfaceId].output_x = 0;
        bctx->surfaces[create->surfaceId].output_y = 0;
        
        /* Allocate per-surface buffer for proper GFX cache workflow.
         * We need this buffer to support SurfaceToCache operations,
         * which read pixels from the surface (not primary_buffer). */
        if (bctx->surface_buffers[create->surfaceId]) {
            free(bctx->surface_buffers[create->surfaceId]);
        }
        size_t buf_size = create->width * create->height * 4;  /* BGRA32 */
        bctx->surface_buffers[create->surfaceId] = (uint8_t*)calloc(1, buf_size);
        if (!bctx->surface_buffers[create->surfaceId]) {
            fprintf(stderr, "[rdp_bridge] Warning: Failed to allocate surface buffer (%zu bytes)\n", buf_size);
        }
        
        /* Create progressive surface context for this surface */
        if (bctx->progressive_decoder) {
            progressive_create_surface_context(bctx->progressive_decoder, 
                create->surfaceId, create->width, create->height);
        }
    }
    
    pthread_mutex_unlock(&bctx->gfx_mutex);
    
    /* Queue CREATE_SURFACE event for Python wire format streaming */
    RdpGfxEvent event = {0};
    event.type = RDP_GFX_EVENT_CREATE_SURFACE;
    event.surface_id = create->surfaceId;
    event.width = create->width;
    event.height = create->height;
    event.pixel_format = create->pixelFormat;
    gfx_queue_event(bctx, &event);
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_delete_surface(RdpgfxClientContext* context, const RDPGFX_DELETE_SURFACE_PDU* del)
{
    /* PURE GFX MODE: Track surfaces ourselves, no GDI chaining */
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx) return ERROR_INVALID_PARAMETER;
    
    /* NOTE: ClearCodec decoder is session-level and should NOT be reset on surface delete. */
    
    pthread_mutex_lock(&bctx->gfx_mutex);
    
    if (del->surfaceId < RDP_MAX_GFX_SURFACES) {
        bctx->surfaces[del->surfaceId].active = false;
        if (bctx->primary_surface_id == del->surfaceId) {
            bctx->primary_surface_id = 0;
            /* Issue 12l: Primary surface deleted - clear stale dirty rects */
            pthread_mutex_lock(&bctx->rect_mutex);
            bctx->dirty_rect_count = 0;
            pthread_mutex_unlock(&bctx->rect_mutex);
        }
        if (bctx->surface_buffers[del->surfaceId]) {
            free(bctx->surface_buffers[del->surfaceId]);
            bctx->surface_buffers[del->surfaceId] = NULL;
        }
        /* Delete progressive surface context */
        if (bctx->progressive_decoder) {
            progressive_delete_surface_context(bctx->progressive_decoder, del->surfaceId);
        }
    }
    
    pthread_mutex_unlock(&bctx->gfx_mutex);
    
    /* Queue DELETE_SURFACE event for Python wire format streaming */
    RdpGfxEvent event = {0};
    event.type = RDP_GFX_EVENT_DELETE_SURFACE;
    event.surface_id = del->surfaceId;
    gfx_queue_event(bctx, &event);
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_map_surface(RdpgfxClientContext* context, const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU* map)
{
    /* PURE GFX MODE: Track surface mapping ourselves, no GDI chaining */
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx) return ERROR_INVALID_PARAMETER;
    
    pthread_mutex_lock(&bctx->gfx_mutex);
    
    if (map->surfaceId < RDP_MAX_GFX_SURFACES && bctx->surfaces[map->surfaceId].active) {
        bctx->surfaces[map->surfaceId].mapped_to_output = true;
        bctx->surfaces[map->surfaceId].output_x = map->outputOriginX;
        bctx->surfaces[map->surfaceId].output_y = map->outputOriginY;
        bctx->primary_surface_id = map->surfaceId;
    }
    
    pthread_mutex_unlock(&bctx->gfx_mutex);
    return CHANNEL_RC_OK;
}

/* Additional GFX callbacks - no-op implementations for protocol compliance */

static UINT gfx_on_map_surface_scaled(RdpgfxClientContext* context, 
                                       const RDPGFX_MAP_SURFACE_TO_SCALED_OUTPUT_PDU* map)
{
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx) return ERROR_INVALID_PARAMETER;
    
    pthread_mutex_lock(&bctx->gfx_mutex);
    if (map->surfaceId < RDP_MAX_GFX_SURFACES && bctx->surfaces[map->surfaceId].active) {
        bctx->surfaces[map->surfaceId].mapped_to_output = true;
        bctx->surfaces[map->surfaceId].output_x = map->outputOriginX;
        bctx->surfaces[map->surfaceId].output_y = map->outputOriginY;
        bctx->primary_surface_id = map->surfaceId;
    }
    pthread_mutex_unlock(&bctx->gfx_mutex);
    return CHANNEL_RC_OK;
}

static UINT gfx_on_map_surface_window(RdpgfxClientContext* context,
                                       const RDPGFX_MAP_SURFACE_TO_WINDOW_PDU* map)
{
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx) return ERROR_INVALID_PARAMETER;
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_map_surface_scaled_window(RdpgfxClientContext* context,
                                              const RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU* map)
{
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx) return ERROR_INVALID_PARAMETER;
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_solid_fill(RdpgfxClientContext* context, const RDPGFX_SOLID_FILL_PDU* fill)
{
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx || !fill) return ERROR_INVALID_PARAMETER;
    
    /* Track commands in this frame */
    bctx->frame_cmd_count++;
    
    /* PURE GFX MODE: Only write to surface_buffers[], never gdi->primary_buffer */
    
    /* Validate surface */
    if (fill->surfaceId >= RDP_MAX_GFX_SURFACES || !bctx->surfaces[fill->surfaceId].active) {
        return CHANNEL_RC_OK;
    }
    
    pthread_mutex_lock(&bctx->gfx_mutex);
    uint8_t* surfBuf = bctx->surface_buffers[fill->surfaceId];
    UINT32 surfW = bctx->surfaces[fill->surfaceId].width;
    UINT32 surfH = bctx->surfaces[fill->surfaceId].height;
    int32_t offsetX = bctx->surfaces[fill->surfaceId].output_x;
    int32_t offsetY = bctx->surfaces[fill->surfaceId].output_y;
    pthread_mutex_unlock(&bctx->gfx_mutex);
    
    if (!surfBuf || surfW == 0 || surfH == 0) {
        return CHANNEL_RC_OK;
    }
    
    /* Build BGRA32 color */
    uint32_t color = fill->fillPixel.B | 
                     (fill->fillPixel.G << 8) | 
                     (fill->fillPixel.R << 16) | 
                     (fill->fillPixel.XA << 24);
    
    UINT32 surfStride = surfW * 4;
    
    /* Fill each rectangle in surface buffer */
    for (UINT16 i = 0; i < fill->fillRectCount; i++) {
        UINT32 surfLeft = fill->fillRects[i].left;
        UINT32 surfTop = fill->fillRects[i].top;
        UINT32 surfRight = fill->fillRects[i].right;
        UINT32 surfBottom = fill->fillRects[i].bottom;
        
        /* Clamp to surface bounds */
        if (surfRight > surfW) surfRight = surfW;
        if (surfBottom > surfH) surfBottom = surfH;
        if (surfLeft >= surfRight || surfTop >= surfBottom) continue;
        
        /* Fill surface buffer */
        for (UINT32 y = surfTop; y < surfBottom; y++) {
            uint32_t* row = (uint32_t*)(surfBuf + y * surfStride + surfLeft * 4);
            for (UINT32 x = 0; x < (surfRight - surfLeft); x++) {
                row[x] = color;
            }
        }
        
        /* Mark dirty rect (with output offset for screen coords) */
        pthread_mutex_lock(&bctx->rect_mutex);
        if (bctx->dirty_rect_count < RDP_MAX_DIRTY_RECTS) {
            bctx->dirty_rects[bctx->dirty_rect_count].x = surfLeft + offsetX;
            bctx->dirty_rects[bctx->dirty_rect_count].y = surfTop + offsetY;
            bctx->dirty_rects[bctx->dirty_rect_count].width = surfRight - surfLeft;
            bctx->dirty_rects[bctx->dirty_rect_count].height = surfBottom - surfTop;
            bctx->dirty_rect_count++;
        } else {
            bctx->needs_full_frame = true;
        }
        pthread_mutex_unlock(&bctx->rect_mutex);
    }
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_surface_to_surface(RdpgfxClientContext* context, 
                                       const RDPGFX_SURFACE_TO_SURFACE_PDU* copy)
{
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx || !copy) return ERROR_INVALID_PARAMETER;
    
    /* Track commands in this frame */
    bctx->frame_cmd_count++;
    
    /* PURE GFX MODE: Only work with surface_buffers[], never gdi->primary_buffer */
    
    pthread_mutex_lock(&bctx->gfx_mutex);
    
    /* Get source surface info */
    uint8_t* srcSurfBuf = NULL;
    UINT32 srcSurfW = 0, srcSurfH = 0;
    
    if (copy->surfaceIdSrc < RDP_MAX_GFX_SURFACES && bctx->surfaces[copy->surfaceIdSrc].active) {
        srcSurfBuf = bctx->surface_buffers[copy->surfaceIdSrc];
        srcSurfW = bctx->surfaces[copy->surfaceIdSrc].width;
        srcSurfH = bctx->surfaces[copy->surfaceIdSrc].height;
    }
    
    /* Get dest surface info */
    uint8_t* dstSurfBuf = NULL;
    UINT32 dstSurfW = 0, dstSurfH = 0;
    INT32 dstOutX = 0, dstOutY = 0;
    
    if (copy->surfaceIdDest < RDP_MAX_GFX_SURFACES && bctx->surfaces[copy->surfaceIdDest].active) {
        dstSurfBuf = bctx->surface_buffers[copy->surfaceIdDest];
        dstSurfW = bctx->surfaces[copy->surfaceIdDest].width;
        dstSurfH = bctx->surfaces[copy->surfaceIdDest].height;
        dstOutX = bctx->surfaces[copy->surfaceIdDest].output_x;
        dstOutY = bctx->surfaces[copy->surfaceIdDest].output_y;
    }
    
    pthread_mutex_unlock(&bctx->gfx_mutex);
    
    /* Source rectangle (surface-local coords) */
    INT32 srcLeft = copy->rectSrc.left;
    INT32 srcTop = copy->rectSrc.top;
    INT32 width = copy->rectSrc.right - copy->rectSrc.left;
    INT32 height = copy->rectSrc.bottom - copy->rectSrc.top;
    
    if (width <= 0 || height <= 0 || !srcSurfBuf || !dstSurfBuf) return CHANNEL_RC_OK;
    
    /* For overlapping copies within same surface, need temp buffer */
    uint8_t* tempBuf = NULL;
    bool needTemp = (copy->surfaceIdSrc == copy->surfaceIdDest);
    
    if (needTemp && srcSurfW > 0) {
        size_t tempSize = width * height * 4;
        tempBuf = (uint8_t*)malloc(tempSize);
        if (tempBuf) {
            UINT32 srcStride = srcSurfW * 4;
            for (INT32 y = 0; y < height; y++) {
                INT32 sy = srcTop + y;
                if (sy < 0 || sy >= (INT32)srcSurfH) continue;
                uint8_t* src = srcSurfBuf + sy * srcStride + srcLeft * 4;
                uint8_t* dst = tempBuf + y * width * 4;
                INT32 copyW = width;
                if (srcLeft < 0) { copyW += srcLeft; } 
                if (srcLeft + width > (INT32)srcSurfW) copyW = srcSurfW - srcLeft;
                if (copyW > 0 && srcLeft >= 0) memcpy(dst, src, copyW * 4);
            }
        }
    }
    
    UINT32 dstStride = dstSurfW * 4;
    
    /* Copy to each destination point in dest surface buffer */
    for (UINT16 i = 0; i < copy->destPtsCount; i++) {
        INT32 dstLeft = copy->destPts[i].x;
        INT32 dstTop = copy->destPts[i].y;
        
        for (INT32 y = 0; y < height; y++) {
            INT32 sy = srcTop + y;
            INT32 dy = dstTop + y;
            if (dy < 0 || dy >= (INT32)dstSurfH) continue;
            if (!tempBuf && (sy < 0 || sy >= (INT32)srcSurfH)) continue;
            
            uint8_t* src;
            if (tempBuf) {
                src = tempBuf + y * width * 4;
            } else {
                src = srcSurfBuf + sy * (srcSurfW * 4) + srcLeft * 4;
            }
            
            INT32 copyW = width;
            INT32 dstX = dstLeft;
            if (dstX < 0) { src -= dstX * 4; copyW += dstX; dstX = 0; }
            if (dstX + copyW > (INT32)dstSurfW) copyW = dstSurfW - dstX;
            if (copyW > 0) {
                uint8_t* dst = dstSurfBuf + dy * dstStride + dstX * 4;
                memcpy(dst, src, copyW * 4);
            }
        }
        
        /* Mark dirty rect (with output offset for screen coords) */
        pthread_mutex_lock(&bctx->rect_mutex);
        if (bctx->dirty_rect_count < RDP_MAX_DIRTY_RECTS) {
            INT32 dirtyX = dstLeft + dstOutX;
            INT32 dirtyY = dstTop + dstOutY;
            if (dirtyX < 0) dirtyX = 0;
            if (dirtyY < 0) dirtyY = 0;
            bctx->dirty_rects[bctx->dirty_rect_count].x = dirtyX;
            bctx->dirty_rects[bctx->dirty_rect_count].y = dirtyY;
            bctx->dirty_rects[bctx->dirty_rect_count].width = width;
            bctx->dirty_rects[bctx->dirty_rect_count].height = height;
            bctx->dirty_rect_count++;
        } else {
            bctx->needs_full_frame = true;
        }
        pthread_mutex_unlock(&bctx->rect_mutex);
    }
    
    if (tempBuf) free(tempBuf);
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_surface_to_cache(RdpgfxClientContext* context,
                                     const RDPGFX_SURFACE_TO_CACHE_PDU* cache)
{
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx || !cache) return ERROR_INVALID_PARAMETER;
    
    if (!bctx->gfx_cache) {
        return CHANNEL_RC_OK;  /* No cache allocated */
    }
    
    /* Get surface info and buffer */
    if (cache->surfaceId >= RDP_MAX_GFX_SURFACES || !bctx->surfaces[cache->surfaceId].active) {
        return CHANNEL_RC_OK;
    }
    
    uint8_t* surfBuf = bctx->surface_buffers[cache->surfaceId];
    UINT32 surfWidth = bctx->surfaces[cache->surfaceId].width;
    UINT32 surfHeight = bctx->surfaces[cache->surfaceId].height;
    
    if (!surfBuf) {
        static int no_buf_logs = 0;
        if (no_buf_logs < 10) {
            fprintf(stderr, "[rdp_bridge] BLACK_TILE: SurfaceToCache no surface buffer for surface=%u slot=%u\n",
                    cache->surfaceId, cache->cacheSlot);
            no_buf_logs++;
        }
        return CHANNEL_RC_OK;  /* No surface buffer */
    }
    
    /* Calculate source rectangle (no offset - using surface-local coords) */
    INT32 left = cache->rectSrc.left;
    INT32 top = cache->rectSrc.top;
    UINT32 width = cache->rectSrc.right - cache->rectSrc.left;
    UINT32 height = cache->rectSrc.bottom - cache->rectSrc.top;
    
    /* Validate cache slot */
    if (cache->cacheSlot >= (UINT64)bctx->gfx_cache_size) {
        fprintf(stderr, "[rdp_bridge] Warning: cache slot %u exceeds max %d\n",
                cache->cacheSlot, bctx->gfx_cache_size);
        return CHANNEL_RC_OK;
    }
    
    /* Bounds check against surface dimensions */
    if (left < 0 || top < 0 || left + (INT32)width > (INT32)surfWidth || 
        top + (INT32)height > (INT32)surfHeight) {
        return CHANNEL_RC_OK;
    }
    
    /* Allocate/reallocate cache entry */
    GfxCacheEntry* entry = &bctx->gfx_cache[cache->cacheSlot];
    size_t dataSize = width * height * 4;
    
    if (entry->data && entry->data_size < dataSize) {
        free(entry->data);
        entry->data = NULL;
    }
    if (!entry->data) {
        entry->data = (uint8_t*)malloc(dataSize);
        if (!entry->data) return ERROR_OUTOFMEMORY;
        entry->data_size = dataSize;
    }
    
    /* Copy pixels from surface buffer to cache */
    UINT32 surfStride = surfWidth * 4;
    for (UINT32 y = 0; y < height; y++) {
        uint8_t* src = surfBuf + (top + y) * surfStride + left * 4;
        uint8_t* dst = entry->data + y * width * 4;
        memcpy(dst, src, width * 4);
    }
    
    entry->width = width;
    entry->height = height;
    entry->valid = true;
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_cache_to_surface(RdpgfxClientContext* context,
                                     const RDPGFX_CACHE_TO_SURFACE_PDU* cache)
{
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx || !cache) return ERROR_INVALID_PARAMETER;
    
    /* Track commands in this frame */
    bctx->frame_cmd_count++;
    
    /* PURE GFX MODE: Only write to surface_buffers[], never gdi->primary_buffer */
    
    if (!bctx->gfx_cache) {
        return CHANNEL_RC_OK;
    }
    
    /* Validate cache slot */
    if (cache->cacheSlot >= (UINT64)bctx->gfx_cache_size) {
        return CHANNEL_RC_OK;
    }
    
    GfxCacheEntry* entry = &bctx->gfx_cache[cache->cacheSlot];
    if (!entry->valid || !entry->data) {
        static int miss_logs = 0;
        if (miss_logs < 20) {
            fprintf(stderr, "[rdp_bridge] BLACK_TILE: CacheToSurface cache miss slot=%u (valid=%d, data=%p)\n",
                    cache->cacheSlot, entry->valid, (void*)entry->data);
            miss_logs++;
        }
        pthread_mutex_lock(&bctx->rect_mutex);
        bctx->needs_full_frame = true;
        pthread_mutex_unlock(&bctx->rect_mutex);
        return CHANNEL_RC_OK;
    }
    
    /* Get surface offset */
    int32_t offsetX = 0, offsetY = 0;
    if (cache->surfaceId < RDP_MAX_GFX_SURFACES && bctx->surfaces[cache->surfaceId].active) {
        offsetX = bctx->surfaces[cache->surfaceId].output_x;
        offsetY = bctx->surfaces[cache->surfaceId].output_y;
    }
    
    /* Copy cached bitmap to each destination point in surface buffer */
    for (UINT16 i = 0; i < cache->destPtsCount; i++) {
        /* Destination in surface-local coordinates */
        INT32 surfDstX = cache->destPts[i].x;
        INT32 surfDstY = cache->destPts[i].y;
        
        /* Get surface buffer info */
        uint8_t* surfBuf = NULL;
        UINT32 surfW = 0, surfH = 0;
        if (cache->surfaceId < RDP_MAX_GFX_SURFACES && bctx->surfaces[cache->surfaceId].active) {
            surfBuf = bctx->surface_buffers[cache->surfaceId];
            surfW = bctx->surfaces[cache->surfaceId].width;
            surfH = bctx->surfaces[cache->surfaceId].height;
        }
        
        if (!surfBuf || surfW == 0 || surfH == 0) continue;
        
        /* PURE GFX MODE: Only write to surface buffer */
        INT32 clampX = surfDstX;
        INT32 clampY = surfDstY;
        UINT32 copyW = entry->width;
        UINT32 copyH = entry->height;
        UINT32 srcOffX = 0, srcOffY = 0;
        
        if (clampX < 0) { srcOffX = -clampX; copyW += clampX; clampX = 0; }
        if (clampY < 0) { srcOffY = -clampY; copyH += clampY; clampY = 0; }
        if (clampX + (INT32)copyW > (INT32)surfW) copyW = surfW - clampX;
        if (clampY + (INT32)copyH > (INT32)surfH) copyH = surfH - clampY;
        
        if (copyW > 0 && copyH > 0) {
            UINT32 surfStride = surfW * 4;
            for (UINT32 y = 0; y < copyH; y++) {
                uint8_t* src = entry->data + (srcOffY + y) * entry->width * 4 + srcOffX * 4;
                uint8_t* dst = surfBuf + (clampY + y) * surfStride + clampX * 4;
                memcpy(dst, src, copyW * 4);
            }
            
            /* Mark dirty rect (with output offset for screen coords) */
            pthread_mutex_lock(&bctx->rect_mutex);
            if (bctx->dirty_rect_count < RDP_MAX_DIRTY_RECTS) {
                bctx->dirty_rects[bctx->dirty_rect_count].x = clampX + offsetX;
                bctx->dirty_rects[bctx->dirty_rect_count].y = clampY + offsetY;
                bctx->dirty_rects[bctx->dirty_rect_count].width = copyW;
                bctx->dirty_rects[bctx->dirty_rect_count].height = copyH;
                bctx->dirty_rect_count++;
            } else {
                bctx->needs_full_frame = true;
            }
            pthread_mutex_unlock(&bctx->rect_mutex);
        }
    }
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_evict_cache(RdpgfxClientContext* context,
                                const RDPGFX_EVICT_CACHE_ENTRY_PDU* evict)
{
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx || !evict) return ERROR_INVALID_PARAMETER;
    
    /* Mark cache slot as invalid */
    if (bctx->gfx_cache && evict->cacheSlot < (UINT64)bctx->gfx_cache_size) {
        bctx->gfx_cache[evict->cacheSlot].valid = false;
        /* Keep the buffer allocated for reuse */
    }
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_delete_encoding_context(RdpgfxClientContext* context,
                                            const RDPGFX_DELETE_ENCODING_CONTEXT_PDU* del)
{
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx || !del) return ERROR_INVALID_PARAMETER;
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_cache_import_reply(RdpgfxClientContext* context,
                                       const RDPGFX_CACHE_IMPORT_REPLY_PDU* reply)
{
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx || !reply) return ERROR_INVALID_PARAMETER;
    
    return CHANNEL_RC_OK;
}

/* Helper: Detect frame type from H.264 NAL unit */
static RdpH264FrameType detect_h264_frame_type(const uint8_t* data, size_t len)
{
    if (!data || len < 4) return RDP_H264_FRAME_TYPE_P;
    
    /* Look for NAL unit type in Annex-B stream */
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            uint8_t nal_type = data[i+3] & 0x1F;
            if (nal_type == 5) return RDP_H264_FRAME_TYPE_IDR;  /* IDR picture */
            if (nal_type == 1) return RDP_H264_FRAME_TYPE_P;    /* Non-IDR */
        } else if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            if (i + 4 < len) {
                uint8_t nal_type = data[i+4] & 0x1F;
                if (nal_type == 5) return RDP_H264_FRAME_TYPE_IDR;
                if (nal_type == 1) return RDP_H264_FRAME_TYPE_P;
            }
        }
    }
    return RDP_H264_FRAME_TYPE_P;
}

/* Queue an H.264 frame for browser consumption
 * For AVC444: transcode dual streams (4:4:4) to single 4:2:0 stream */
static bool queue_h264_frame(BridgeContext* bctx, uint32_t frame_id, uint16_t surface_id,
                             RdpGfxCodecId codec_id, const RdpRect* rect,
                             const uint8_t* nal_data, uint32_t nal_size,
                             const uint8_t* chroma_data, uint32_t chroma_size)
{
    if (!bctx || !nal_data || nal_size == 0) return false;
    
    const uint8_t* output_nal = nal_data;
    uint32_t output_nal_size = nal_size;
    uint8_t* transcoded_data = NULL;
    
    /* For AVC444: transcode to 4:2:0 for browser compatibility */
    if ((codec_id == RDP_GFX_CODEC_AVC444 || codec_id == RDP_GFX_CODEC_AVC444v2) 
        && chroma_data && chroma_size > 0) {
        
        /* Initialize transcoder on first frame */
        if (!bctx->transcoder_initialized) {
            int width = rect->width > 0 ? rect->width : bctx->frame_width;
            int height = rect->height > 0 ? rect->height : bctx->frame_height;
            if (width <= 0 || height <= 0) {
                width = 1920;
                height = 1080;
            }
            if (!init_transcoder(bctx, width, height)) {
                fprintf(stderr, "[rdp_bridge] Transcoder init failed, passing through luma only\n");
            }
        }
        
        /* Transcode AVC444 → AVC420 */
        if (bctx->transcoder_initialized) {
            uint32_t new_size = 0;
            if (transcode_avc444(bctx, nal_data, nal_size, chroma_data, chroma_size,
                                 &transcoded_data, &new_size)) {
                output_nal = transcoded_data;
                output_nal_size = new_size;
                codec_id = RDP_GFX_CODEC_AVC420;  /* Now it's 4:2:0 */
            }
        }
    }
    
    pthread_mutex_lock(&bctx->h264_mutex);
    
    /* Check if queue is full */
    if (bctx->h264_count >= RDP_MAX_H264_FRAMES) {
        /* Drop oldest frame */
        H264FrameEntry* oldest = &bctx->h264_frames[bctx->h264_read_idx];
        if (oldest->nal_data) free(oldest->nal_data);
        if (oldest->chroma_nal_data) free(oldest->chroma_nal_data);
        oldest->valid = false;
        bctx->h264_read_idx = (bctx->h264_read_idx + 1) % RDP_MAX_H264_FRAMES;
        bctx->h264_count--;
        fprintf(stderr, "[rdp_bridge] Warning: H.264 queue full, dropped frame\n");
    }
    
    H264FrameEntry* entry = &bctx->h264_frames[bctx->h264_write_idx];
    
    /* Free any existing data */
    if (entry->nal_data) free(entry->nal_data);
    if (entry->chroma_nal_data) free(entry->chroma_nal_data);
    
    /* Copy NAL data (either original or transcoded) */
    entry->nal_data = (uint8_t*)malloc(output_nal_size);
    if (!entry->nal_data) {
        pthread_mutex_unlock(&bctx->h264_mutex);
        if (transcoded_data) free(transcoded_data);
        return false;
    }
    memcpy(entry->nal_data, output_nal, output_nal_size);
    entry->nal_size = output_nal_size;
    
    /* No chroma data after transcoding (merged into 4:2:0) */
    entry->chroma_nal_data = NULL;
    entry->chroma_nal_size = 0;
    
    entry->frame_id = frame_id;
    entry->surface_id = surface_id;
    entry->codec_id = codec_id;
    entry->frame_type = detect_h264_frame_type(output_nal, output_nal_size);
    entry->dest_rect = *rect;
    entry->timestamp = (uint64_t)(clock() * 1000000.0 / CLOCKS_PER_SEC);
    entry->needs_ack = true;
    entry->valid = true;
    
    bctx->h264_write_idx = (bctx->h264_write_idx + 1) % RDP_MAX_H264_FRAMES;
    bctx->h264_count++;
      
    /* Track negotiated codec (report original, not transcoded) */
    if (codec_id == RDP_GFX_CODEC_AVC420 && chroma_data) {
        /* Was AVC444, transcoded to AVC420 */
        bctx->gfx_codec = RDP_GFX_CODEC_AVC444;
    } else {
        bctx->gfx_codec = codec_id;
    }
    
    /* Mark that H.264 was queued in this frame - frontend will handle rendering */
    bctx->h264_queued_this_frame = true;
    
    pthread_mutex_unlock(&bctx->h264_mutex);
    
    if (transcoded_data) free(transcoded_data);
    return true;
}

static UINT gfx_on_surface_command(RdpgfxClientContext* context, const RDPGFX_SURFACE_COMMAND* cmd)
{
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx) return ERROR_INVALID_PARAMETER;
    
    /* Track commands in this frame */
    bctx->frame_cmd_count++;
    
    /* Check if disconnecting - don't process frames during teardown */
    pthread_mutex_lock(&bctx->gfx_mutex);
    bool disconnecting = bctx->gfx_disconnecting;
    pthread_mutex_unlock(&bctx->gfx_mutex);
    
    if (disconnecting) {
        return CHANNEL_RC_OK;
    }
    
    RdpRect rect = {
        .x = cmd->left,
        .y = cmd->top,
        .width = cmd->right - cmd->left,
        .height = cmd->bottom - cmd->top
    };
    
    switch (cmd->codecId) {
        case RDPGFX_CODECID_AVC420: {
            /* AVC420: Single H.264 stream in YUV 4:2:0 */
            const RDPGFX_AVC420_BITMAP_STREAM* avc420 = cmd->extra;
            if (avc420 && avc420->data && avc420->length > 0) {
                queue_h264_frame(bctx, bctx->current_frame_id, cmd->surfaceId,
                                RDP_GFX_CODEC_AVC420, &rect,
                                avc420->data, avc420->length, NULL, 0);
            }
            break;
        }
        
        case RDPGFX_CODECID_AVC444:
        case RDPGFX_CODECID_AVC444v2: {
            /* AVC444: Dual H.264 streams (luma + chroma) for YUV 4:4:4 
             * Per MS-RDPEGFX, the RFX_AVC444_BITMAP_STREAM contains:
             * - LC field indicating stream configuration
             * - First AVC420 stream (typically luma)
             * - Second AVC420 stream (typically chroma) */
            const RDPGFX_AVC444_BITMAP_STREAM* avc444 = cmd->extra;
            if (avc444) {
                const uint8_t* luma_data = NULL;
                uint32_t luma_size = 0;
                const uint8_t* chroma_data = NULL;
                uint32_t chroma_size = 0;
                
                /* First stream (usually luma/main) */
                if (avc444->bitstream[0].data && avc444->bitstream[0].length > 0) {
                    luma_data = avc444->bitstream[0].data;
                    luma_size = avc444->bitstream[0].length;
                }
                
                /* Second stream (usually chroma for 4:4:4) */
                if (avc444->bitstream[1].data && avc444->bitstream[1].length > 0) {
                    chroma_data = avc444->bitstream[1].data;
                    chroma_size = avc444->bitstream[1].length;
                }
                
                if (luma_data) {
                    RdpGfxCodecId codec = (cmd->codecId == RDPGFX_CODECID_AVC444v2) 
                                          ? RDP_GFX_CODEC_AVC444v2 : RDP_GFX_CODEC_AVC444;
                    queue_h264_frame(bctx, bctx->current_frame_id, cmd->surfaceId,
                                    codec, &rect, luma_data, luma_size,
                                    chroma_data, chroma_size);
                }
            }
            break;
        }
        
        /* CLEARCODEC: Decode using FreeRDP's standalone clear_decompress API */
        case RDPGFX_CODECID_CLEARCODEC: {
            if (!bctx->clear_decoder || !cmd->data || cmd->length == 0) {
                static int clear_skip = 0;
                if (clear_skip < 3) {
                    fprintf(stderr, "[rdp_bridge] ClearCodec: no decoder or data - marking refresh\n");
                    clear_skip++;
                }
                pthread_mutex_lock(&bctx->rect_mutex);
                bctx->needs_full_frame = true;
                pthread_mutex_unlock(&bctx->rect_mutex);
                break;
            }
            
            /* Get surface info */
            if (cmd->surfaceId >= RDP_MAX_GFX_SURFACES || !bctx->surfaces[cmd->surfaceId].active) {
                break;
            }
            
            uint8_t* surfBuf = bctx->surface_buffers[cmd->surfaceId];
            UINT32 surfWidth = bctx->surfaces[cmd->surfaceId].width;
            UINT32 surfHeight = bctx->surfaces[cmd->surfaceId].height;
            UINT32 surfStride = surfWidth * 4;
            
            /* Command rect is in surface-local coordinates */
            UINT32 nWidth = cmd->right - cmd->left;
            UINT32 nHeight = cmd->bottom - cmd->top;
            UINT32 surfX = cmd->left;
            UINT32 surfY = cmd->top;
            
            /* Bounds check against surface */
            if (surfX + nWidth > surfWidth || surfY + nHeight > surfHeight) {
                static int bounds_err = 0;
                if (bounds_err < 3) {
                    fprintf(stderr, "[rdp_bridge] ClearCodec: out of surface bounds\n");
                    bounds_err++;
                }
                break;
            }
            
            /* Decode ClearCodec to surface buffer if available */
            INT32 result = -1;
            if (surfBuf) {
                /* Note: nDstWidth/nDstHeight must be the FULL destination buffer size,
                 * not the update region size, for proper bounds checking */
                result = clear_decompress(bctx->clear_decoder,
                                         cmd->data, cmd->length,
                                         nWidth, nHeight,
                                         surfBuf, PIXEL_FORMAT_BGRA32,
                                         surfStride, surfX, surfY,
                                         surfWidth, surfHeight,
                                         NULL);
            } else {
                static int no_surfbuf = 0;
                if (no_surfbuf < 10) {
                    fprintf(stderr, "[rdp_bridge] BLACK_TILE: ClearCodec no surface buffer for surf=%u\n",
                            cmd->surfaceId);
                    no_surfbuf++;
                }
            }
            
            if (result < 0) {
                static int clear_err = 0;
                if (clear_err < 5) {
                    fprintf(stderr, "[rdp_bridge] ClearCodec decode failed: %d - marking refresh\n", result);
                    clear_err++;
                }
                pthread_mutex_lock(&bctx->rect_mutex);
                bctx->needs_full_frame = true;
                pthread_mutex_unlock(&bctx->rect_mutex);
                break;
            }
            
            /* PURE GFX MODE: Mark dirty rect (data is already in surface buffer) */
            INT32 outX = bctx->surfaces[cmd->surfaceId].output_x;
            INT32 outY = bctx->surfaces[cmd->surfaceId].output_y;
            
            pthread_mutex_lock(&bctx->rect_mutex);
            if (bctx->dirty_rect_count < RDP_MAX_DIRTY_RECTS) {
                bctx->dirty_rects[bctx->dirty_rect_count].x = surfX + outX;
                bctx->dirty_rects[bctx->dirty_rect_count].y = surfY + outY;
                bctx->dirty_rects[bctx->dirty_rect_count].width = nWidth;
                bctx->dirty_rects[bctx->dirty_rect_count].height = nHeight;
                bctx->dirty_rect_count++;
            } else {
                bctx->needs_full_frame = true;
            }
            pthread_mutex_unlock(&bctx->rect_mutex);
            break;
        }
        
        /* UNCOMPRESSED: Raw BGRA pixels - copy to surface buffer */
        case RDPGFX_CODECID_UNCOMPRESSED: {
            /* PURE GFX MODE: Only write to surface_buffers[] */
            
            if (!cmd->data) {
                pthread_mutex_lock(&bctx->rect_mutex);
                bctx->needs_full_frame = true;
                pthread_mutex_unlock(&bctx->rect_mutex);
                break;
            }
            
            UINT32 surfId = cmd->surfaceId;
            UINT32 surfX = cmd->left;
            UINT32 surfY = cmd->top;
            UINT32 nWidth = cmd->right - cmd->left;
            UINT32 nHeight = cmd->bottom - cmd->top;
            
            /* Get surface info */
            if (surfId >= RDP_MAX_GFX_SURFACES || !bctx->surfaces[surfId].active) {
                break;
            }
            
            uint8_t* surfBuf = bctx->surface_buffers[surfId];
            UINT32 surfW = bctx->surfaces[surfId].width;
            UINT32 surfH = bctx->surfaces[surfId].height;
            INT32 outX = bctx->surfaces[surfId].output_x;
            INT32 outY = bctx->surfaces[surfId].output_y;
            
            /* Copy to surface buffer */
            if (surfBuf && surfW > 0 && surfH > 0 &&
                surfX + nWidth <= surfW && surfY + nHeight <= surfH) {
                UINT32 srcStride = nWidth * 4;
                UINT32 surfStride = surfW * 4;
                for (UINT32 y = 0; y < nHeight; y++) {
                    BYTE* src = cmd->data + y * srcStride;
                    BYTE* dst = surfBuf + (surfY + y) * surfStride + surfX * 4;
                    memcpy(dst, src, nWidth * 4);
                }
                
                /* Mark dirty rect */
                pthread_mutex_lock(&bctx->rect_mutex);
                if (bctx->dirty_rect_count < RDP_MAX_DIRTY_RECTS) {
                    bctx->dirty_rects[bctx->dirty_rect_count].x = surfX + outX;
                    bctx->dirty_rects[bctx->dirty_rect_count].y = surfY + outY;
                    bctx->dirty_rects[bctx->dirty_rect_count].width = nWidth;
                    bctx->dirty_rects[bctx->dirty_rect_count].height = nHeight;
                    bctx->dirty_rect_count++;
                } else {
                    bctx->needs_full_frame = true;
                }
                pthread_mutex_unlock(&bctx->rect_mutex);
            }
            
            break;
        }
        
        /* Progressive codec - decode using FreeRDP's progressive decoder */
        case RDPGFX_CODECID_CAPROGRESSIVE:
        case RDPGFX_CODECID_CAPROGRESSIVE_V2: {
            if (!bctx->progressive_decoder) {
                fprintf(stderr, "[rdp_bridge] Progressive decoder not initialized\n");
                pthread_mutex_lock(&bctx->rect_mutex);
                bctx->needs_full_frame = true;
                pthread_mutex_unlock(&bctx->rect_mutex);
                break;
            }
            
            /* PURE GFX MODE: Only write to surface_buffers[] */
            
            UINT32 surfId = cmd->surfaceId;
            
            /* Get surface info */
            if (surfId >= RDP_MAX_GFX_SURFACES || !bctx->surfaces[surfId].active) {
                break;
            }
            
            uint8_t* surfBuf = bctx->surface_buffers[surfId];
            UINT32 surfW = bctx->surfaces[surfId].width;
            UINT32 surfH = bctx->surfaces[surfId].height;
            INT32 outX = bctx->surfaces[surfId].output_x;
            INT32 outY = bctx->surfaces[surfId].output_y;
            
            if (!surfBuf || surfW == 0 || surfH == 0) {
                break;
            }
            
            /* Progressive codec uses Wire-To-Surface-2 PDU format:
             * - rect coordinates are 0,0,0,0 
             * - actual update region is determined during decompression */
            REGION16 invalidRegion;
            region16_init(&invalidRegion);
            
            UINT32 surfStride = surfW * 4;
            INT32 rc = progressive_decompress(bctx->progressive_decoder,
                cmd->data, cmd->length,
                surfBuf, PIXEL_FORMAT_BGRA32,
                surfStride,
                0, 0,  /* nXDst, nYDst - decode to full buffer */
                &invalidRegion,
                cmd->surfaceId, bctx->current_frame_id);
            
            if (rc < 0) {
                static int prog_err = 0;
                if (prog_err < 5) {
                    fprintf(stderr, "[rdp_bridge] Progressive decode failed: %d\n", rc);
                    prog_err++;
                }
                pthread_mutex_lock(&bctx->rect_mutex);
                bctx->needs_full_frame = true;
                pthread_mutex_unlock(&bctx->rect_mutex);
            } else {
                /* Mark invalidated regions as dirty (adjusted for output offset) */
                UINT32 numRects = 0;
                const RECTANGLE_16* rects = region16_rects(&invalidRegion, &numRects);
                
                pthread_mutex_lock(&bctx->rect_mutex);
                for (UINT32 i = 0; i < numRects && bctx->dirty_rect_count < RDP_MAX_DIRTY_RECTS; i++) {
                    bctx->dirty_rects[bctx->dirty_rect_count].x = outX + rects[i].left;
                    bctx->dirty_rects[bctx->dirty_rect_count].y = outY + rects[i].top;
                    bctx->dirty_rects[bctx->dirty_rect_count].width = rects[i].right - rects[i].left;
                    bctx->dirty_rects[bctx->dirty_rect_count].height = rects[i].bottom - rects[i].top;
                    bctx->dirty_rect_count++;
                }
                if (numRects > 0 && bctx->dirty_rect_count >= RDP_MAX_DIRTY_RECTS) {
                    bctx->needs_full_frame = true;
                }
                if (numRects == 0) {
                    bctx->needs_full_frame = true;
                }
                pthread_mutex_unlock(&bctx->rect_mutex);
            }
            
            region16_uninit(&invalidRegion);
            break;
        }
        
        /* Planar codec - decode using FreeRDP's planar decoder */
        case RDPGFX_CODECID_PLANAR: {
            if (!bctx->planar_decoder) {
                pthread_mutex_lock(&bctx->rect_mutex);
                bctx->needs_full_frame = true;
                pthread_mutex_unlock(&bctx->rect_mutex);
                break;
            }
            
            /* PURE GFX MODE: Only write to surface_buffers[] */
            
            UINT32 surfId = cmd->surfaceId;
            UINT32 surfX = cmd->left;
            UINT32 surfY = cmd->top;
            UINT32 nWidth = cmd->right - cmd->left;
            UINT32 nHeight = cmd->bottom - cmd->top;
            
            /* Get surface info */
            if (surfId >= RDP_MAX_GFX_SURFACES || !bctx->surfaces[surfId].active) {
                break;
            }
            
            uint8_t* surfBuf = bctx->surface_buffers[surfId];
            UINT32 surfW = bctx->surfaces[surfId].width;
            UINT32 surfH = bctx->surfaces[surfId].height;
            INT32 outX = bctx->surfaces[surfId].output_x;
            INT32 outY = bctx->surfaces[surfId].output_y;
            
            bool decoded = false;
            
            /* Decode to surface buffer */
            if (surfBuf && surfW > 0 && surfH > 0 &&
                surfX + nWidth <= surfW && surfY + nHeight <= surfH) {
                UINT32 surfStride = surfW * 4;
                if (freerdp_bitmap_decompress_planar(bctx->planar_decoder,
                        cmd->data, cmd->length,
                        nWidth, nHeight,
                        surfBuf, PIXEL_FORMAT_BGRA32,
                        surfStride,
                        surfX, surfY,
                        nWidth, nHeight, FALSE)) {
                    decoded = true;
                    
                    /* Mark dirty rect */
                    pthread_mutex_lock(&bctx->rect_mutex);
                    if (bctx->dirty_rect_count < RDP_MAX_DIRTY_RECTS) {
                        bctx->dirty_rects[bctx->dirty_rect_count].x = surfX + outX;
                        bctx->dirty_rects[bctx->dirty_rect_count].y = surfY + outY;
                        bctx->dirty_rects[bctx->dirty_rect_count].width = nWidth;
                        bctx->dirty_rects[bctx->dirty_rect_count].height = nHeight;
                        bctx->dirty_rect_count++;
                    } else {
                        bctx->needs_full_frame = true;
                    }
                    pthread_mutex_unlock(&bctx->rect_mutex);
                }
            }
            
            if (!decoded) {
                static int planar_err = 0;
                if (planar_err < 5) {
                    fprintf(stderr, "[rdp_bridge] Planar decode failed\n");
                    planar_err++;
                }
                pthread_mutex_lock(&bctx->rect_mutex);
                bctx->needs_full_frame = true;
                pthread_mutex_unlock(&bctx->rect_mutex);
            }
            break;
        }
        
        /* Alpha codec and other unknown codecs */
        case RDPGFX_CODECID_ALPHA:
        default: {
            static int other_codec = 0;
            if (other_codec < 10) {
                fprintf(stderr, "[rdp_bridge] Unsupported codec 0x%04X at (%d,%d)-(%d,%d) - marking refresh\n",
                        cmd->codecId, cmd->left, cmd->top, cmd->right, cmd->bottom);
                other_codec++;
            }
            pthread_mutex_lock(&bctx->rect_mutex);
            bctx->needs_full_frame = true;
            pthread_mutex_unlock(&bctx->rect_mutex);
            break;
        }
    }
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_start_frame(RdpgfxClientContext* context, const RDPGFX_START_FRAME_PDU* start)
{
    /* PURE GFX MODE: Just track frame ID, no GDI chaining needed */
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx || !start) return ERROR_INVALID_PARAMETER;
    
    /* Mark frame as in progress - Python should not send frames while this is true */
    pthread_mutex_lock(&bctx->rect_mutex);
    bctx->gfx_frame_in_progress = true;
    pthread_mutex_unlock(&bctx->rect_mutex);
    
    bctx->current_frame_id = start->frameId;
    bctx->frame_cmd_count = 0;  /* Reset command count for this frame */
    bctx->h264_queued_this_frame = false;  /* Reset H.264 flag for this frame */
    
    /* Queue START_FRAME event for Python wire format streaming */
    RdpGfxEvent event = {0};
    event.type = RDP_GFX_EVENT_START_FRAME;
    event.frame_id = start->frameId;
    gfx_queue_event(bctx, &event);
    
    return CHANNEL_RC_OK;
}

static UINT gfx_on_end_frame(RdpgfxClientContext* context, const RDPGFX_END_FRAME_PDU* end)
{
    /* PURE GFX MODE: FreeRDP's internal rdpgfx_recv_end_frame_pdu() will send 
     * FrameAcknowledge after we return. We just need to track decoded frames.
     * 
     * Note: FreeRDP increments gfx->TotalDecodedFrames and sends the ack internally
     * when gfx->sendFrameAcks is TRUE (which it is by default).
     */
    BridgeContext* bctx = (BridgeContext*)context->custom;
    if (!bctx || !end) return ERROR_INVALID_PARAMETER;
    
    pthread_mutex_lock(&bctx->rect_mutex);
    /* Frame is now complete - Python can safely read buffer and send frames */
    bctx->gfx_frame_in_progress = false;
    /* Update last_completed_frame_id so Python knows a frame finished */
    bctx->last_completed_frame_id = end->frameId;
    pthread_mutex_unlock(&bctx->rect_mutex);
    
    /* Queue END_FRAME event for Python wire format streaming */
    RdpGfxEvent event = {0};
    event.type = RDP_GFX_EVENT_END_FRAME;
    event.frame_id = end->frameId;
    gfx_queue_event(bctx, &event);
    
    /* DON'T set needs_full_frame here! 
     * - If H.264 was sent, the frontend handles it via the H.264 pipeline
     * - If dirty_rects were added (SolidFill, CopyRect, etc), they'll be sent as delta WebP
     * - Only set needs_full_frame as a fallback in error cases within each codec handler
     */
    
    return CHANNEL_RC_OK;
}

/* ============================================================================
 * GFX/H.264 API Implementation
 * ============================================================================ */

bool rdp_gfx_is_active(RdpSession* session)
{
    if (!session) return false;
    BridgeContext* ctx = (BridgeContext*)session;
    
    pthread_mutex_lock(&ctx->gfx_mutex);
    bool active = ctx->gfx_active && ctx->gfx != NULL;
    pthread_mutex_unlock(&ctx->gfx_mutex);
    
    return active;
}

RdpGfxCodecId rdp_gfx_get_codec(RdpSession* session)
{
    if (!session) return RDP_GFX_CODEC_UNCOMPRESSED;
    BridgeContext* ctx = (BridgeContext*)session;
    
    pthread_mutex_lock(&ctx->gfx_mutex);
    RdpGfxCodecId codec = ctx->gfx_codec;
    pthread_mutex_unlock(&ctx->gfx_mutex);
    
    return codec;
}

int rdp_has_h264_frames(RdpSession* session)
{
    if (!session) return 0;
    BridgeContext* ctx = (BridgeContext*)session;
    
    pthread_mutex_lock(&ctx->h264_mutex);
    int count = ctx->h264_count;
    pthread_mutex_unlock(&ctx->h264_mutex);
    
    return count;
}

int rdp_get_h264_frame(RdpSession* session, RdpH264Frame* frame)
{
    if (!session || !frame) return -2;
    BridgeContext* ctx = (BridgeContext*)session;
    
    pthread_mutex_lock(&ctx->h264_mutex);
    
    if (ctx->h264_count == 0) {
        pthread_mutex_unlock(&ctx->h264_mutex);
        return -1;  /* No frames */
    }
    
    H264FrameEntry* entry = &ctx->h264_frames[ctx->h264_read_idx];
    if (!entry->valid) {
        pthread_mutex_unlock(&ctx->h264_mutex);
        return -1;
    }
    
    /* Copy to output (caller must not free nal_data) */
    frame->frame_id = entry->frame_id;
    frame->surface_id = entry->surface_id;
    frame->codec_id = entry->codec_id;
    frame->frame_type = entry->frame_type;
    frame->dest_rect = entry->dest_rect;
    frame->nal_size = entry->nal_size;
    frame->nal_data = entry->nal_data;
    frame->chroma_nal_size = entry->chroma_nal_size;
    frame->chroma_nal_data = entry->chroma_nal_data;
    frame->timestamp = entry->timestamp;
    frame->needs_ack = entry->needs_ack;
    
    /* Mark as read but don't free yet - caller needs the data */
    entry->valid = false;
    ctx->h264_read_idx = (ctx->h264_read_idx + 1) % RDP_MAX_H264_FRAMES;
    ctx->h264_count--;
    
    pthread_mutex_unlock(&ctx->h264_mutex);
    return 0;
}

int rdp_ack_h264_frame(RdpSession* session, uint32_t frame_id)
{
    if (!session) return -1;
    BridgeContext* ctx = (BridgeContext*)session;
    
    pthread_mutex_lock(&ctx->gfx_mutex);
    
    if (!ctx->gfx || !ctx->gfx_active) {
        pthread_mutex_unlock(&ctx->gfx_mutex);
        return -1;
    }
    
    RDPGFX_FRAME_ACKNOWLEDGE_PDU ack = {
        .frameId = frame_id,
        .queueDepth = 1
    };
    
    UINT status = CHANNEL_RC_OK;
    if (ctx->gfx->FrameAcknowledge) {
        status = ctx->gfx->FrameAcknowledge(ctx->gfx, &ack);
    }
    
    pthread_mutex_unlock(&ctx->gfx_mutex);
    
    return (status == CHANNEL_RC_OK) ? 0 : -1;
}

int rdp_gfx_get_surface(RdpSession* session, uint16_t surface_id, RdpGfxSurface* surface)
{
    if (!session || !surface) return -1;
    BridgeContext* ctx = (BridgeContext*)session;
    
    pthread_mutex_lock(&ctx->gfx_mutex);
    
    for (int i = 0; i < RDP_MAX_GFX_SURFACES; i++) {
        if (ctx->surfaces[i].active && ctx->surfaces[i].surface_id == surface_id) {
            *surface = ctx->surfaces[i];
            pthread_mutex_unlock(&ctx->gfx_mutex);
            return 0;
        }
    }
    
    pthread_mutex_unlock(&ctx->gfx_mutex);
    return -1;
}

uint16_t rdp_gfx_get_primary_surface(RdpSession* session)
{
    if (!session) return 0;
    BridgeContext* ctx = (BridgeContext*)session;
    
    pthread_mutex_lock(&ctx->gfx_mutex);
    uint16_t id = ctx->primary_surface_id;
    pthread_mutex_unlock(&ctx->gfx_mutex);
    
    return id;
}

/* ============================================================================
 * GFX Event Queue API (for wire format streaming)
 * ============================================================================ */

/* Internal helper: queue a GFX event (caller must NOT hold gfx_event_mutex) */
static void gfx_queue_event(BridgeContext* ctx, const RdpGfxEvent* event)
{
    if (!ctx || !event) return;
    
    pthread_mutex_lock(&ctx->gfx_event_mutex);
    
    if (ctx->gfx_event_count >= RDP_MAX_GFX_EVENTS) {
        /* Queue full - drop oldest event */
        ctx->gfx_event_read_idx = (ctx->gfx_event_read_idx + 1) % RDP_MAX_GFX_EVENTS;
        ctx->gfx_event_count--;
    }
    
    ctx->gfx_events[ctx->gfx_event_write_idx] = *event;
    ctx->gfx_event_write_idx = (ctx->gfx_event_write_idx + 1) % RDP_MAX_GFX_EVENTS;
    ctx->gfx_event_count++;
    
    pthread_mutex_unlock(&ctx->gfx_event_mutex);
}

int rdp_gfx_has_events(RdpSession* session)
{
    if (!session) return 0;
    BridgeContext* ctx = (BridgeContext*)session;
    
    pthread_mutex_lock(&ctx->gfx_event_mutex);
    int count = ctx->gfx_event_count;
    pthread_mutex_unlock(&ctx->gfx_event_mutex);
    
    return count;
}

int rdp_gfx_get_event(RdpSession* session, RdpGfxEvent* event)
{
    if (!session || !event) return -1;
    BridgeContext* ctx = (BridgeContext*)session;
    
    pthread_mutex_lock(&ctx->gfx_event_mutex);
    
    if (ctx->gfx_event_count == 0) {
        pthread_mutex_unlock(&ctx->gfx_event_mutex);
        return -1;
    }
    
    *event = ctx->gfx_events[ctx->gfx_event_read_idx];
    ctx->gfx_event_read_idx = (ctx->gfx_event_read_idx + 1) % RDP_MAX_GFX_EVENTS;
    ctx->gfx_event_count--;
    
    pthread_mutex_unlock(&ctx->gfx_event_mutex);
    return 0;
}

void rdp_gfx_clear_events(RdpSession* session)
{
    if (!session) return;
    BridgeContext* ctx = (BridgeContext*)session;
    
    pthread_mutex_lock(&ctx->gfx_event_mutex);
    ctx->gfx_event_write_idx = 0;
    ctx->gfx_event_read_idx = 0;
    ctx->gfx_event_count = 0;
    pthread_mutex_unlock(&ctx->gfx_event_mutex);
}

int rdp_gfx_is_progressive_enabled(RdpSession* session)
{
    if (!session) return 0;
    
    rdpContext* context = (rdpContext*)session;
    rdpSettings* settings = context->settings;
    
    if (!settings) return 0;
    
    return freerdp_settings_get_bool(settings, FreeRDP_GfxProgressive) ? 1 : 0;
}

/* ============================================================================
 * Audio API
 * ============================================================================ */

bool rdp_has_audio_data(RdpSession* session)
{
    if (!session) return false;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (!ctx->audio_initialized || !ctx->audio_buffer) {
        return false;
    }
    
    pthread_mutex_lock(&ctx->audio_mutex);
    bool has_data = ctx->audio_buffer_pos > ctx->audio_read_pos;
    pthread_mutex_unlock(&ctx->audio_mutex);
    
    return has_data;
}

int rdp_get_audio_format(RdpSession* session, int* sample_rate, int* channels, int* bits)
{
    if (!session) return -1;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (!ctx->audio_initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&ctx->audio_mutex);
    if (sample_rate) *sample_rate = ctx->audio_sample_rate;
    if (channels) *channels = ctx->audio_channels;
    if (bits) *bits = ctx->audio_bits;
    pthread_mutex_unlock(&ctx->audio_mutex);
    
    return 0;
}

int rdp_get_audio_data(RdpSession* session, uint8_t* buffer, int max_size)
{
    if (!session || !buffer || max_size <= 0) return -1;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (!ctx->audio_initialized || !ctx->audio_buffer) {
        return 0;
    }
    
    pthread_mutex_lock(&ctx->audio_mutex);
    
    size_t available = ctx->audio_buffer_pos - ctx->audio_read_pos;
    if (available == 0) {
        pthread_mutex_unlock(&ctx->audio_mutex);
        return 0;
    }
    
    size_t to_copy = (available < (size_t)max_size) ? available : (size_t)max_size;
    memcpy(buffer, ctx->audio_buffer + ctx->audio_read_pos, to_copy);
    ctx->audio_read_pos += to_copy;
    
    /* Reset buffer positions if fully consumed */
    if (ctx->audio_read_pos >= ctx->audio_buffer_pos) {
        ctx->audio_buffer_pos = 0;
        ctx->audio_read_pos = 0;
    }
    
    pthread_mutex_unlock(&ctx->audio_mutex);
    
    return (int)to_copy;
}

void rdp_write_audio_data(RdpSession* session, const uint8_t* data, size_t size,
                          int sample_rate, int channels, int bits)
{
    if (!session || !data || size == 0) return;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    pthread_mutex_lock(&ctx->audio_mutex);
    
    /* Update format info */
    ctx->audio_sample_rate = sample_rate;
    ctx->audio_channels = channels;
    ctx->audio_bits = bits;
    
    /* Check if we need to resize buffer or if buffer is full */
    if (ctx->audio_buffer_pos + size > ctx->audio_buffer_size) {
        /* Buffer overflow - reset and accept data loss */
        ctx->audio_buffer_pos = 0;
        ctx->audio_read_pos = 0;
    }
    
    if (ctx->audio_buffer && ctx->audio_buffer_pos + size <= ctx->audio_buffer_size) {
        memcpy(ctx->audio_buffer + ctx->audio_buffer_pos, data, size);
        ctx->audio_buffer_pos += size;
    }
    
    pthread_mutex_unlock(&ctx->audio_mutex);
}

/* ============================================================================
 * Opus Audio API (for native audio streaming without PulseAudio)
 * ============================================================================ */

void rdp_set_audio_context(RdpSession* session)
{
    if (!session) return;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    /* Point to our BridgeContext's Opus buffer using POINTERS for mutable state */
    g_audio_ctx.opus_buffer = ctx->opus_buffer;
    g_audio_ctx.opus_buffer_size = ctx->opus_buffer_size;
    g_audio_ctx.opus_write_pos = &ctx->opus_write_pos;
    g_audio_ctx.opus_read_pos = &ctx->opus_read_pos;
    g_audio_ctx.opus_mutex = &ctx->opus_mutex;
    g_audio_ctx.sample_rate = ctx->opus_sample_rate;
    g_audio_ctx.channels = ctx->opus_channels;
    g_audio_ctx.initialized = &ctx->opus_initialized;
    
    /* Try to find and call the plugin's context setter using dlsym.
     * The plugin is loaded dynamically by FreeRDP during connect,
     * so we use RTLD_DEFAULT to search all loaded libraries. */
    typedef void (*set_context_fn)(void*);
    set_context_fn set_ctx = (set_context_fn)dlsym(RTLD_DEFAULT, "rdpsnd_bridge_set_context");
    if (set_ctx) {
        set_ctx(&g_audio_ctx);
        fprintf(stderr, "[rdp_bridge] Audio context passed to rdpsnd plugin\n");
    } else {
        fprintf(stderr, "[rdp_bridge] rdpsnd_bridge_set_context not found (plugin not loaded yet)\n");
    }
}

/* Exported function for the plugin to get the current audio context.
 * The plugin can call this via dlsym(RTLD_DEFAULT, "rdp_get_current_audio_context")
 * Note: This is a legacy interface. For multi-session, use rdp_lookup_session_by_rdpcontext */
__attribute__((visibility("default")))
void* rdp_get_current_audio_context(void)
{
    return &g_audio_ctx;
}

/* Get audio buffer debug statistics for diagnostics */
int rdp_get_audio_stats(RdpSession* session, int* initialized, size_t* write_pos, 
                        size_t* read_pos, size_t* buffer_size)
{
    if (!session) return -1;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (initialized) *initialized = ctx->opus_initialized;
    if (buffer_size) *buffer_size = ctx->opus_buffer_size;
    
    if (!ctx->opus_buffer) {
        if (write_pos) *write_pos = 0;
        if (read_pos) *read_pos = 0;
        return -2;  /* Buffer not allocated */
    }
    
    pthread_mutex_lock(&ctx->opus_mutex);
    if (write_pos) *write_pos = ctx->opus_write_pos;
    if (read_pos) *read_pos = ctx->opus_read_pos;
    pthread_mutex_unlock(&ctx->opus_mutex);
    
    return 0;
}

bool rdp_has_opus_data(RdpSession* session)
{
    if (!session) return false;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (!ctx->opus_initialized || !ctx->opus_buffer) {
        return false;
    }
    
    pthread_mutex_lock(&ctx->opus_mutex);
    bool has_data = ctx->opus_write_pos > ctx->opus_read_pos;
    pthread_mutex_unlock(&ctx->opus_mutex);
    
    return has_data;
}

int rdp_get_opus_format(RdpSession* session, int* sample_rate, int* channels)
{
    if (!session) return -1;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (!ctx->opus_initialized) {
        return -1;
    }
    
    pthread_mutex_lock(&ctx->opus_mutex);
    if (sample_rate) *sample_rate = ctx->opus_sample_rate;
    if (channels) *channels = ctx->opus_channels;
    pthread_mutex_unlock(&ctx->opus_mutex);
    
    return 0;
}

int rdp_get_opus_frame(RdpSession* session, uint8_t* buffer, int max_size)
{
    if (!session || !buffer || max_size <= 0) return -1;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
    
    if (!ctx->opus_initialized || !ctx->opus_buffer) {
        return 0;
    }
    
    pthread_mutex_lock(&ctx->opus_mutex);
    
    /* Check if we have any data */
    if (ctx->opus_write_pos <= ctx->opus_read_pos) {
        pthread_mutex_unlock(&ctx->opus_mutex);
        return 0;
    }
    
    /* Read frame header (2 bytes: little-endian size) */
    size_t read_pos = ctx->opus_read_pos % ctx->opus_buffer_size;
    uint16_t frame_size = ctx->opus_buffer[read_pos];
    read_pos = (read_pos + 1) % ctx->opus_buffer_size;
    frame_size |= (uint16_t)ctx->opus_buffer[read_pos] << 8;
    read_pos = (read_pos + 1) % ctx->opus_buffer_size;
    
    if (frame_size == 0 || frame_size > 4000) {
        /* Invalid frame - reset buffer */
        ctx->opus_write_pos = 0;
        ctx->opus_read_pos = 0;
        pthread_mutex_unlock(&ctx->opus_mutex);
        return 0;
    }
    
    if (frame_size > max_size) {
        /* Buffer too small - skip this frame */
        ctx->opus_read_pos += 2 + frame_size;
        pthread_mutex_unlock(&ctx->opus_mutex);
        return -2;  /* Buffer too small error */
    }
    
    /* Read Opus frame data (handle wrap-around) */
    size_t first_chunk = ctx->opus_buffer_size - read_pos;
    if (first_chunk >= frame_size) {
        memcpy(buffer, ctx->opus_buffer + read_pos, frame_size);
    } else {
        memcpy(buffer, ctx->opus_buffer + read_pos, first_chunk);
        memcpy(buffer + first_chunk, ctx->opus_buffer, frame_size - first_chunk);
    }
    
    ctx->opus_read_pos += 2 + frame_size;
    
    /* Reset positions if buffer is empty */
    if (ctx->opus_read_pos >= ctx->opus_write_pos) {
        ctx->opus_write_pos = 0;
        ctx->opus_read_pos = 0;
    }
    
    pthread_mutex_unlock(&ctx->opus_mutex);
    
    return frame_size;
}

/* ============================================================================
 * Version
 * ============================================================================ */

const char* rdp_version(void)
{
    return RDP_BRIDGE_VERSION;
}

/* ============================================================================
 * Codec Constants - Exported for Python binding to avoid value drift
 * These values come directly from FreeRDP's rdpgfx.h header
 * ============================================================================ */

uint16_t rdp_gfx_codec_uncompressed(void) { return RDPGFX_CODECID_UNCOMPRESSED; }
uint16_t rdp_gfx_codec_cavideo(void) { return RDPGFX_CODECID_CAVIDEO; }
uint16_t rdp_gfx_codec_clearcodec(void) { return RDPGFX_CODECID_CLEARCODEC; }
uint16_t rdp_gfx_codec_planar(void) { return RDPGFX_CODECID_PLANAR; }
uint16_t rdp_gfx_codec_avc420(void) { return RDPGFX_CODECID_AVC420; }
uint16_t rdp_gfx_codec_alpha(void) { return RDPGFX_CODECID_ALPHA; }
uint16_t rdp_gfx_codec_avc444(void) { return RDPGFX_CODECID_AVC444; }
uint16_t rdp_gfx_codec_avc444v2(void) { return RDPGFX_CODECID_AVC444v2; }
uint16_t rdp_gfx_codec_progressive(void) { return RDPGFX_CODECID_CAPROGRESSIVE; }
uint16_t rdp_gfx_codec_progressive_v2(void) { return RDPGFX_CODECID_CAPROGRESSIVE_V2; }
