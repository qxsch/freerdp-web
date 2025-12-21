/**
 * RDP Bridge Native Library Implementation
 * 
 * Uses FreeRDP3 libfreerdp for direct RDP connection with:
 * - GDI software rendering to memory buffer
 * - Dirty rectangle tracking for delta updates
 * - Direct input injection (no X11/xdotool)
 */

#include "rdp_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

/* FreeRDP3 headers */
#include <freerdp/freerdp.h>
#include <freerdp/client.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/rdpsnd.h>
#include <freerdp/client/disp.h>
#include <freerdp/client/rdpsnd.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/event.h>
#include <opus/opus.h>
#include <winpr/crt.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/collections.h>

#define RDP_BRIDGE_VERSION "1.0.0"
#define MAX_ERROR_LEN 512

/* Extended client context */
typedef struct {
    rdpClientContext common;        /* Must be first */
    
    /* Our custom fields */
    RdpState state;
    char error_msg[MAX_ERROR_LEN];
    
    /* Frame buffer */
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
    ctx->audio_initialized = false;
    ctx->audio_buffer = NULL;
    ctx->audio_buffer_size = 0;
    ctx->opus_encoder = NULL;
    
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
    
    /* Use legacy GDI mode for stable headless rendering.
     * GFX pipeline is disabled because gdi_graphics_pipeline_init() causes
     * threading issues when called from channel callbacks in FreeRDP3. */
    if (!freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, FALSE)) goto fail;
    
    /* Audio playback - uses PulseAudio null sink in container */
    if (!freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_AudioCapture, FALSE)) goto fail;
    
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
    
    /* Certificate handling - ignore for simplicity */
    if (!freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_AutoAcceptCertificate, TRUE)) goto fail;
    
    /* Clipboard (enabled) */
    if (!freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE)) goto fail;
    
    /* Dynamic resolution updates */
    if (!freerdp_settings_set_bool(settings, FreeRDP_SupportDisplayControl, TRUE)) goto fail;
    if (!freerdp_settings_set_bool(settings, FreeRDP_DynamicResolutionUpdate, TRUE)) goto fail;
    
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
    
    if (!freerdp_connect(instance)) {
        UINT32 error = freerdp_get_last_error(context);
        snprintf(ctx->error_msg, MAX_ERROR_LEN, 
                 "Connection failed: 0x%08X", error);
        ctx->state = RDP_STATE_ERROR;
        return -1;
    }
    
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
    
    rdp_disconnect(session);
    
    pthread_mutex_destroy(&ctx->rect_mutex);
    pthread_mutex_destroy(&ctx->audio_mutex);
    
    /* Free audio resources */
    if (ctx->opus_encoder) {
        opus_encoder_destroy(ctx->opus_encoder);
        ctx->opus_encoder = NULL;
    }
    if (ctx->audio_buffer) {
        free(ctx->audio_buffer);
        ctx->audio_buffer = NULL;
    }
    
    freerdp_client_context_free(context);
}

/* ============================================================================
 * Event Processing & Frame Capture
 * ============================================================================ */

int rdp_poll(RdpSession* session, int timeout_ms)
{
    if (!session) return -1;
    
    rdpContext* context = (rdpContext*)session;
    freerdp* instance = context->instance;
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
    
    /* Handle pending resize - use display control channel if available */
    if (ctx->resize_pending) {
        ctx->resize_pending = false;
        
        uint32_t new_width = ctx->pending_width;
        uint32_t new_height = ctx->pending_height;
        
        /* Try to use Display Control channel for dynamic resize */
        if (ctx->disp && ctx->disp->SendMonitorLayout) {
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
            
            ctx->disp->SendMonitorLayout(ctx->disp, 1, &layout);
        }
        
        /* Mark for full frame after resize */
        pthread_mutex_lock(&ctx->rect_mutex);
        ctx->needs_full_frame = true;
        pthread_mutex_unlock(&ctx->rect_mutex);
    }
    
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
            return -1;
        }
    }
    
    /* Check if we have dirty rects (frame update available) */
    pthread_mutex_lock(&ctx->rect_mutex);
    int has_update = (ctx->dirty_rect_count > 0 || ctx->needs_full_frame) ? 1 : 0;
    pthread_mutex_unlock(&ctx->rect_mutex);
    
    return has_update;
}

uint8_t* rdp_get_frame_buffer(RdpSession* session, int* width, int* height, int* stride)
{
    if (!session) return NULL;
    
    rdpContext* context = (rdpContext*)session;
    BridgeContext* ctx = (BridgeContext*)context;
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
    
    int count = ctx->dirty_rect_count;
    if (count > max_rects) count = max_rects;
    
    for (int i = 0; i < count; i++) {
        rects[i] = ctx->dirty_rects[i];
    }
    
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
    bool needs = ctx->needs_full_frame;
    ctx->needs_full_frame = false;
    pthread_mutex_unlock(&ctx->rect_mutex);
    
    return needs;
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
    rdpSettings* settings = context->settings;
    
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
    }
    else if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
        /* GFX pipeline disabled - just store reference */
        bctx->gfx = (RdpgfxClientContext*)e->pInterface;
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
    }
    else if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
        bctx->gfx = NULL;
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
    freerdp* instance = context->instance;
    rdpSettings* settings = context->settings;
    rdpGdi* gdi = context->gdi;
    
    /* Free old GDI resources */
    gdi_free(instance);
    
    /* Reinitialize GDI with new size */
    if (!gdi_init(instance, PIXEL_FORMAT_BGRA32)) {
        snprintf(ctx->error_msg, MAX_ERROR_LEN, "GDI resize failed");
        return FALSE;
    }
    
    gdi = context->gdi;
    
    /* Update stored dimensions */
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
 * Version
 * ============================================================================ */

const char* rdp_version(void)
{
    return RDP_BRIDGE_VERSION;
}
