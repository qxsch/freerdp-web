/**
 * RDP Web Client
 * Vanilla JS implementation for browser-based RDP via WebSocket proxy
 */

(function() {
    'use strict';

    // Configuration
    const config = {
        wsUrl: 'ws://localhost:8765',
        reconnectDelay: 3000,
        mouseThrottleMs: 16, // ~60fps for mouse moves
        resizeDebounceMs: 2000, // Wait 2 seconds before resize renegotiation
    };

    // State
    let ws = null;
    let isConnected = false;
    let canvas = null;
    let ctx = null;
    let lastMouseSend = 0;
    let pingStart = 0;
    let resizeTimeout = null;
    let pendingResize = false;
    let lastRequestedWidth = 0;  // Track last requested dimensions to avoid duplicates
    let lastRequestedHeight = 0;
    
    // Audio state
    let audioContext = null;
    let audioQueue = [];
    let isAudioPlaying = false;
    let isMuted = false;
    let audioGainNode = null;
    let audioNextPlayTime = 0;  // Track when next buffer should start (prevents gaps)
    
    // Opus decoder state
    let opusDecoder = null;
    let opusInitialized = false;
    let opusSampleRate = 48000;
    let opusChannels = 2;
    let opusDecodeErrors = 0;        // Track consecutive decode errors
    let opusLastSuccessTime = 0;     // Timestamp of last successful decode
    let opusRecoveryPending = false; // Prevent multiple simultaneous recovery attempts
    let opusFramesReceived = 0;      // Total frames received from server
    let opusFramesDecoded = 0;       // Total frames successfully decoded (output callback fired)
    let opusFramesQueued = 0;        // Total frames queued for playback

    // H.264 video decoder state (WebCodecs)
    let videoDecoder = null;
    let h264Initialized = false;
    let h264FrameCount = 0;
    let lastH264FrameId = 0;
    let pendingH264Frames = [];  // Queue for out-of-order frames
    let gfxSurfaces = new Map(); // surface_id -> {width, height, canvas}
    let h264DecodeQueue = [];    // Metadata queue: {destX, destY, destW, destH} per pending decode
    let h264DecoderError = false; // Set on decode error, cleared on IDR
    let h264ConfiguredWidth = 0;  // Currently configured decoder width
    let h264ConfiguredHeight = 0; // Currently configured decoder height
    let h264ActiveRegion = null;  // Track H.264 active region to prevent WebP overpainting
    let h264Active = false;       // True once we've received any H.264 frame

    // Audio frame magic headers
    const AUDIO_MAGIC = [0x41, 0x55, 0x44, 0x49];  // "AUDI" (PCM - legacy)
    const OPUS_MAGIC = [0x4F, 0x50, 0x55, 0x53];   // "OPUS" (Opus encoded)
    
    // H.264 frame magic header
    const H264_MAGIC = [0x48, 0x32, 0x36, 0x34];   // "H264"
    
    // Frame types received by client:
    // - WebP/JPEG: Raw image data (full frame or GDI-decoded content)
    // - DELT: Delta frame with dirty rectangles as WebP tiles  
    // - H264: H.264 NAL units for hardware decoding
    // - OPUS: Opus-encoded audio
    // 
    // Note: The server handles all RDP GFX codec translation.
    // ClearCodec, Planar, etc. are decoded server-side and sent as WebP.
    // H.264 codecs (AVC420/AVC444) are forwarded as H264 frames.

    // DOM Elements
    const elements = {
        vmScreen: document.getElementById('vmScreen'),
        canvas: document.getElementById('rdpCanvas'),
        loading: document.getElementById('loadingIndicator'),
        statusDot: document.getElementById('statusDot'),
        statusText: document.getElementById('statusText'),
        connectBtn: document.getElementById('connectBtn'),
        disconnectBtn: document.getElementById('disconnectBtn'),
        muteBtn: document.getElementById('muteBtn'),
        fullscreenBtn: document.getElementById('fullscreenBtn'),
        connectModal: document.getElementById('connectModal'),
        modalConnectBtn: document.getElementById('modalConnectBtn'),
        modalCancelBtn: document.getElementById('modalCancelBtn'),
        vmHost: document.getElementById('vmHost'),
        vmPort: document.getElementById('vmPort'),
        vmUser: document.getElementById('vmUser'),
        vmPass: document.getElementById('vmPass'),
        resolution: document.getElementById('resolution'),
        latency: document.getElementById('latency'),
    };

    /**
     * Initialize the RDP client
     */
    function init() {
        canvas = elements.canvas;
        ctx = canvas.getContext('2d');

        setupEventListeners();
        console.log('[RDP Client] Initialized');
    }

    /**
     * Setup all event listeners
     */
    function setupEventListeners() {
        // UI Controls
        elements.connectBtn.addEventListener('click', showConnectModal);
        elements.disconnectBtn.addEventListener('click', disconnect);
        elements.muteBtn.addEventListener('click', toggleMute);
        elements.fullscreenBtn.addEventListener('click', toggleFullscreen);
        elements.modalConnectBtn.addEventListener('click', handleConnect);
        elements.modalCancelBtn.addEventListener('click', hideConnectModal);

        // Close modal on outside click
        elements.connectModal.addEventListener('click', (e) => {
            if (e.target === elements.connectModal) hideConnectModal();
        });

        // Keyboard events (global when connected)
        document.addEventListener('keydown', handleKeyDown);
        document.addEventListener('keyup', handleKeyUp);

        // Mouse events on canvas
        canvas.addEventListener('mousemove', handleMouseMove);
        canvas.addEventListener('mousedown', handleMouseDown);
        canvas.addEventListener('mouseup', handleMouseUp);
        canvas.addEventListener('wheel', handleMouseWheel);
        canvas.addEventListener('contextmenu', (e) => e.preventDefault());

        // Prevent default for some keys when focused
        canvas.addEventListener('keydown', (e) => {
            if (['Tab', 'F1', 'F5', 'F11', 'F12'].includes(e.key)) {
                e.preventDefault();
            }
        });

        // Focus canvas on click
        canvas.setAttribute('tabindex', '0');
        canvas.addEventListener('click', () => canvas.focus());

        // Window resize event with debounce
        window.addEventListener('resize', handleWindowResize);

        // Also observe the vmScreen container for size changes
        if (typeof ResizeObserver !== 'undefined') {
            const resizeObserver = new ResizeObserver(handleWindowResize);
            resizeObserver.observe(elements.vmScreen);
        }
    }

    /**
     * Show connection modal
     */
    function showConnectModal() {
        elements.connectModal.classList.add('active');
        elements.vmHost.focus();
    }

    /**
     * Hide connection modal
     */
    function hideConnectModal() {
        elements.connectModal.classList.remove('active');
    }

    /**
     * Handle connect button in modal
     */
    function handleConnect() {
        const host = elements.vmHost.value.trim();
        const port = parseInt(elements.vmPort.value) || 3389;
        const user = elements.vmUser.value.trim();
        const pass = elements.vmPass.value;

        if (!host || !user) {
            alert('Please enter host and username');
            return;
        }

        hideConnectModal();
        connect({ host, port, user, pass });
    }

    /**
     * Connect to the WebSocket server and initiate RDP session
     */
    function connect(credentials) {
        if (ws && ws.readyState === WebSocket.OPEN) {
            console.warn('[RDP Client] Already connected');
            return;
        }

        updateStatus('connecting', 'Connecting...');
        elements.loading.querySelector('p').textContent = 'Connecting to server...';

        ws = new WebSocket(config.wsUrl);
        ws.binaryType = 'arraybuffer';

        ws.onopen = () => {
            console.log('[RDP Client] WebSocket connected');
            updateStatus('connecting', 'Authenticating...');
            elements.loading.querySelector('p').textContent = 'Starting RDP session...';

            // Calculate available space for initial connection
            const { width, height } = getAvailableDimensions();
            console.log('[RDP Client] Initial dimensions:', width, 'x', height);
            
            // Track the dimensions we're requesting and pre-set canvas to avoid redundant resize
            lastRequestedWidth = width;
            lastRequestedHeight = height;
            canvas.width = width;
            canvas.height = height;

            // Send connection request with calculated dimensions
            sendMessage({
                type: 'connect',
                host: credentials.host,
                port: credentials.port,
                username: credentials.user,
                password: credentials.pass,
                width: width,
                height: height
            });
        };

        ws.onmessage = handleMessage;

        ws.onerror = (error) => {
            console.error('[RDP Client] WebSocket error:', error);
            updateStatus('disconnected', 'Connection error');
        };

        ws.onclose = (event) => {
            console.log('[RDP Client] WebSocket closed:', event.code, event.reason);
            handleDisconnect();
        };
    }

    /**
     * Disconnect from the server
     */
    function disconnect() {
        if (ws) {
            sendMessage({ type: 'disconnect' });
            ws.close();
        }
        handleDisconnect();
    }

    /**
     * Handle disconnect cleanup
     */
    function handleDisconnect() {
        isConnected = false;
        ws = null;
        lastRequestedWidth = 0;
        lastRequestedHeight = 0;
        updateStatus('disconnected', 'Disconnected');
        elements.canvas.style.display = 'none';
        elements.loading.style.display = 'block';
        elements.loading.querySelector('p').textContent = 'Click Connect to start';
        elements.connectBtn.disabled = false;
        elements.disconnectBtn.disabled = true;
        elements.muteBtn.disabled = true;
        
        // Cleanup audio
        cleanupAudio();
        
        // Cleanup H.264 decoder
        cleanupH264Decoder();
        
        // Reset H.264 region tracking
        h264Active = false;
        h264ActiveRegion = null;
    }

    /**
     * Handle incoming WebSocket messages
     */
    function handleMessage(event) {
        // Binary data = frame update or audio
        if (event.data instanceof ArrayBuffer) {
            const bytes = new Uint8Array(event.data);
            
            // Check for Opus audio frame magic header: "OPUS" (0x4F, 0x50, 0x55, 0x53)
            if (bytes.length > 12 &&
                bytes[0] === OPUS_MAGIC[0] && bytes[1] === OPUS_MAGIC[1] &&
                bytes[2] === OPUS_MAGIC[2] && bytes[3] === OPUS_MAGIC[3]) {
                handleOpusFrame(bytes);
                return;
            }
            
            // Check for legacy PCM audio frame magic header: "AUDI" (0x41, 0x55, 0x44, 0x49)
            if (bytes.length > 12 &&
                bytes[0] === AUDIO_MAGIC[0] && bytes[1] === AUDIO_MAGIC[1] &&
                bytes[2] === AUDIO_MAGIC[2] && bytes[3] === AUDIO_MAGIC[3]) {
                handleAudioFrame(bytes);
                return;
            }
            
            handleFrameUpdate(event.data);
            return;
        }

        // JSON message
        try {
            const msg = JSON.parse(event.data);
            
            switch (msg.type) {
                case 'connected':
                    handleConnected(msg);
                    break;
                case 'frame':
                    // Base64 encoded frame (fallback)
                    handleBase64Frame(msg.data);
                    break;
                case 'resize':
                    handleResize(msg.width, msg.height);
                    break;
                case 'pong':
                    handlePong();
                    break;
                case 'error':
                    handleError(msg.message);
                    break;
                case 'disconnected':
                    handleDisconnect();
                    break;
                default:
                    console.log('[RDP Client] Unknown message type:', msg.type);
            }
        } catch (e) {
            console.error('[RDP Client] Failed to parse message:', e);
        }
    }

    /**
     * Handle successful connection
     */
    function handleConnected(msg) {
        isConnected = true;
        updateStatus('connected', 'Connected');
        elements.canvas.style.display = 'block';
        elements.loading.style.display = 'none';
        elements.connectBtn.disabled = true;
        elements.disconnectBtn.disabled = false;
        elements.muteBtn.disabled = false;
        canvas.focus();
        
        // Initialize audio on first user interaction (required by browsers)
        initAudio();

        if (msg.width && msg.height) {
            handleResize(msg.width, msg.height);
        }

        // Trigger a resize check after a short delay to ensure we're using full available space
        // (container may have slightly different size now that loading indicator is hidden)
        setTimeout(() => {
            if (isConnected) {
                const { width, height } = getAvailableDimensions();
                // Only resize if dimensions changed AND differ from what we already requested
                if ((width !== canvas.width || height !== canvas.height) &&
                    (width !== lastRequestedWidth || height !== lastRequestedHeight)) {
                    console.log('[RDP Client] Post-connect resize to:', width, 'x', height);
                    lastRequestedWidth = width;
                    lastRequestedHeight = height;
                    sendMessage({ type: 'resize', width, height });
                }
            }
        }, 500);

        // Start ping for latency measurement
        setInterval(sendPing, 5000);
        
        console.log('[RDP Client] RDP session established');
    }

    /**
     * Handle binary frame update (WebP full frame, delta frame, or H.264)
     */
    function handleFrameUpdate(data) {
        const bytes = new Uint8Array(data);
        
        // Check for H.264 frame magic header: "H264" (0x48, 0x32, 0x36, 0x34)
        if (bytes.length > 25 &&
            bytes[0] === H264_MAGIC[0] && bytes[1] === H264_MAGIC[1] &&
            bytes[2] === H264_MAGIC[2] && bytes[3] === H264_MAGIC[3]) {
            handleH264Frame(bytes);
            return;
        }
        
        // Check for delta frame magic header: "DELT" (0x44, 0x45, 0x4C, 0x54)
        if (bytes.length > 8 &&
            bytes[0] === 0x44 && bytes[1] === 0x45 &&
            bytes[2] === 0x4C && bytes[3] === 0x54) {
            handleDeltaFrame(bytes);
            return;
        }
        
        // Full frame (WebP or JPEG)
        // Detect format by magic bytes
        let mimeType = 'image/webp';
        
        // JPEG magic: FF D8 FF
        if (bytes[0] === 0xFF && bytes[1] === 0xD8 && bytes[2] === 0xFF) {
            mimeType = 'image/jpeg';
        }
        // RIFF....WEBP (WebP magic)
        // bytes 0-3: RIFF, bytes 8-11: WEBP
        
        const blob = new Blob([data], { type: mimeType });
        const url = URL.createObjectURL(blob);
        const img = new Image();
        
        img.onload = () => {
            // If H.264 is active, clip to draw only outside the H.264 region
            if (h264ActiveRegion) {
                ctx.save();
                clipOutsideRect(ctx, h264ActiveRegion.x, h264ActiveRegion.y, 
                                h264ActiveRegion.w, h264ActiveRegion.h);
                ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
                ctx.restore();
            } else {
                ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
            }
            URL.revokeObjectURL(url);
        };
        
        img.onerror = () => {
            URL.revokeObjectURL(url);
            console.error('[RDP Client] Failed to decode frame');
        };
        
        img.src = url;
    }

    /**
     * Handle delta frame with dirty rectangles
     * Format: [DELT (4)] [JSON length (4, little-endian)] [JSON] [tile data...]
     * JSON: {"rects": [{"x": 0, "y": 0, "w": 100, "h": 100, "size": 1234}, ...]}
     */
    function handleDeltaFrame(bytes) {
        try {
            // Read JSON length (little-endian uint32 at offset 4)
            const jsonLength = bytes[4] | (bytes[5] << 8) | (bytes[6] << 16) | (bytes[7] << 24);
            
            if (jsonLength <= 0 || jsonLength > bytes.length - 8) {
                console.error('[RDP Client] Invalid delta frame JSON length:', jsonLength);
                return;
            }
            
            // Parse JSON metadata
            const jsonBytes = bytes.slice(8, 8 + jsonLength);
            const jsonStr = new TextDecoder().decode(jsonBytes);
            const metadata = JSON.parse(jsonStr);
            
            if (!metadata.rects || !Array.isArray(metadata.rects)) {
                console.error('[RDP Client] Invalid delta frame metadata');
                return;
            }
            
            // Process each tile
            let offset = 8 + jsonLength;
            let tilesLoaded = 0;
            const totalTiles = metadata.rects.length;
            
            for (const rect of metadata.rects) {
                if (offset + rect.size > bytes.length) {
                    console.error('[RDP Client] Delta frame tile overflow');
                    break;
                }
                
                const tileData = bytes.slice(offset, offset + rect.size);
                offset += rect.size;
                
                // Decode and draw tile
                const blob = new Blob([tileData], { type: 'image/webp' });
                const url = URL.createObjectURL(blob);
                const img = new Image();
                
                // Capture rect values in closure
                const tileX = rect.x;
                const tileY = rect.y;
                const tileW = rect.w;
                const tileH = rect.h;
                
                img.onload = () => {
                    // If tile overlaps with H.264 region, clip to draw only outside parts
                    if (h264ActiveRegion && rectsOverlap(
                        tileX, tileY, tileW, tileH,
                        h264ActiveRegion.x, h264ActiveRegion.y, 
                        h264ActiveRegion.w, h264ActiveRegion.h)) {
                        ctx.save();
                        clipOutsideRect(ctx, h264ActiveRegion.x, h264ActiveRegion.y, 
                                        h264ActiveRegion.w, h264ActiveRegion.h);
                        ctx.drawImage(img, tileX, tileY);
                        ctx.restore();
                    } else {
                        ctx.drawImage(img, tileX, tileY);
                    }
                    URL.revokeObjectURL(url);
                    tilesLoaded++;
                };
                
                img.onerror = () => {
                    URL.revokeObjectURL(url);
                    console.error('[RDP Client] Failed to decode delta tile at', tileX, tileY);
                };
                
                img.src = url;
            }
            
        } catch (e) {
            console.error('[RDP Client] Delta frame parsing error:', e);
        }
    }

    /**
     * Handle H.264 frame from GFX pipeline
     * Format: [H264 (4)] [frame_id (4)] [surface_id (2)] [codec_id (2)]
     *         [frame_type (1)] [x (2)] [y (2)] [w (2)] [h (2)]
     *         [nal_size (4)] [chroma_nal_size (4)]
     *         [nal_data...] [chroma_nal_data...]
     */
    async function handleH264Frame(bytes) {
        try {
            // Parse header (25 bytes)
            const view = new DataView(bytes.buffer, bytes.byteOffset);
            let offset = 4; // Skip magic
            
            const frameId = view.getUint32(offset, true); offset += 4;
            const surfaceId = view.getUint16(offset, true); offset += 2;
            const codecId = view.getUint16(offset, true); offset += 2;
            const frameType = bytes[offset]; offset += 1;
            const destX = view.getInt16(offset, true); offset += 2;
            const destY = view.getInt16(offset, true); offset += 2;
            const destW = view.getUint16(offset, true); offset += 2;
            const destH = view.getUint16(offset, true); offset += 2;
            const nalSize = view.getUint32(offset, true); offset += 4;
            const chromaNalSize = view.getUint32(offset, true); offset += 4;
            
            // Validate sizes
            if (offset + nalSize + chromaNalSize > bytes.length) {
                console.error('[RDP Client] H.264 frame data overflow');
                return;
            }
            
            // Extract NAL data
            const nalData = bytes.slice(offset, offset + nalSize);
            offset += nalSize;
            
            // For AVC444, we also have chroma NAL data
            let chromaData = null;
            if (chromaNalSize > 0) {
                chromaData = bytes.slice(offset, offset + chromaNalSize);
            }
            
            // Log first few frames (codec ID is from server's RDP GFX pipeline)
            h264FrameCount++;
            if (h264FrameCount <= 5) {
                const typeName = ['IDR', 'P', 'B'][frameType] || '?';
                console.log(`[RDP Client] H.264 #${h264FrameCount}: codec=0x${codecId.toString(16).padStart(4,'0')} ${typeName} ` +
                           `${destW}x${destH} NAL:${nalSize}b chroma:${chromaNalSize}b`);
            }
            
            // Initialize decoder if needed
            if (!h264Initialized) {
                const success = await initH264Decoder(destW, destH);
                if (!success) {
                    // Fallback: request WebP frames instead
                    console.warn('[RDP Client] H.264 decoder unavailable, frames will be dropped');
                    return;
                }
            }
            
            // Decode the frame
            await decodeH264Frame(nalData, frameType, destX, destY, destW, destH, surfaceId, chromaData);
            
            // Note: Frame acknowledgment is handled automatically by FreeRDP's EndFrame handler
            // in the C code. No need to send acks from JavaScript.
            
            lastH264FrameId = frameId;
            
            // Mark H.264 as active and track the region
            h264Active = true;
            h264ActiveRegion = { x: destX, y: destY, w: destW, h: destH };
            
        } catch (e) {
            console.error('[RDP Client] H.264 frame error:', e);
        }
    }
    
    /**
     * Check if two rectangles overlap
     */
    function rectsOverlap(x1, y1, w1, h1, x2, y2, w2, h2) {
        return !(x1 + w1 <= x2 || x2 + w2 <= x1 || y1 + h1 <= y2 || y2 + h2 <= y1);
    }
    
    /**
     * Set up a clipping region that excludes a rectangle (draw around it)
     * Uses the "evenodd" fill rule: outer rect + inner rect = only outer area
     */
    function clipOutsideRect(context, rx, ry, rw, rh) {
        context.beginPath();
        // Outer rectangle (full canvas)
        context.rect(0, 0, canvas.width, canvas.height);
        // Inner rectangle (the hole to exclude) - wound opposite direction
        context.moveTo(rx, ry);
        context.lineTo(rx, ry + rh);
        context.lineTo(rx + rw, ry + rh);
        context.lineTo(rx + rw, ry);
        context.closePath();
        context.clip('evenodd');
    }

    /**
     * Initialize H.264 decoder using WebCodecs API
     */
    async function initH264Decoder(width, height) {
        // Check WebCodecs support
        if (typeof VideoDecoder === 'undefined') {
            console.warn('[RDP Client] WebCodecs VideoDecoder not supported');
            return false;
        }
        
        try {
            // Check if H.264 is supported
            const config = {
                codec: 'avc1.64001f', // H.264 High Profile Level 3.1
                codedWidth: width,
                codedHeight: height,
                optimizeForLatency: true,
                hardwareAcceleration: 'prefer-hardware',
            };
            
            const support = await VideoDecoder.isConfigSupported(config);
            if (!support.supported) {
                console.warn('[RDP Client] H.264 codec not supported');
                return false;
            }
            
            // Create decoder
            videoDecoder = new VideoDecoder({
                output: (frame) => {
                    // Get destination rect from queue (FIFO order matches decode order)
                    const meta = h264DecodeQueue.shift();
                    
                    // Canvas backing store dimensions (this is the actual RDP content size)
                    const canvasW = canvas.width;
                    const canvasH = canvas.height;
                    
                    // H.264 pads to 16x16 macroblock boundaries:
                    // coded dimensions >= canvas dimensions
                    // The RDP content occupies the top-left (canvasW x canvasH) of the H.264 frame.
                    // The rest is macroblock padding (green/black pixels).
                    
                    // Log first few frames for debugging
                    if (h264FrameCount <= 10) {
                        console.log(`[RDP Client] H.264 decoded:`,
                            `coded=${frame.codedWidth}x${frame.codedHeight}`,
                            `canvas=${canvasW}x${canvasH}`,
                            `padding=(${frame.codedWidth - canvasW},${frame.codedHeight - canvasH})`,
                            `dest=(${meta?.destX},${meta?.destY},${meta?.destW},${meta?.destH})`);
                    }
                    
                    if (meta) {
                        // destRect is in canvas coordinate space.
                        // The H.264 frame content is 1:1 with canvas coordinates - no scaling.
                        // Just use destRect directly as source coordinates.
                        const sx = meta.destX;
                        const sy = meta.destY;
                        const sWidth = meta.destW;
                        const sHeight = meta.destH;
                        
                        // Destination rect: same coordinates
                        const dx = meta.destX;
                        const dy = meta.destY;
                        const dWidth = meta.destW;
                        const dHeight = meta.destH;
                        
                        ctx.drawImage(frame, sx, sy, sWidth, sHeight, dx, dy, dWidth, dHeight);
                    }
                    frame.close();
                    h264DecoderError = false; // Clear error flag on successful decode
                },
                error: (e) => {
                    console.error('[RDP Client] H.264 decode error:', e);
                    h264DecoderError = true;
                    // Clear the metadata queue on error to stay in sync
                    h264DecodeQueue = [];
                }
            });
            
            await videoDecoder.configure(config);
            h264Initialized = true;
            h264ConfiguredWidth = width;
            h264ConfiguredHeight = height;
            console.log('[RDP Client] H.264 decoder initialized:', width, 'x', height);
            return true;
            
        } catch (e) {
            console.error('[RDP Client] H.264 decoder init failed:', e);
            return false;
        }
    }

    /**
     * Decode H.264 frame and draw to canvas
     * Note: AVC444 streams are transcoded to 4:2:0 on the server for browser compatibility
     */
    async function decodeH264Frame(nalData, frameType, destX, destY, destW, destH, surfaceId, chromaData) {
        // Determine if this is a keyframe (IDR)
        const isKeyframe = (frameType === 0);
        
        // For H.264 in RDP GFX, the stream encodes the FULL surface, not tiles.
        // We should initialize decoder with canvas/screen dimensions, not destRect.
        // Use canvas dimensions as the surface size (or we could track per-surface)
        const surfaceW = canvas.width;
        const surfaceH = canvas.height;
        
        // Check if we need to reconfigure decoder for different dimensions
        const needsReconfigure = h264Initialized && isKeyframe && 
            (surfaceW !== h264ConfiguredWidth || surfaceH !== h264ConfiguredHeight);
        
        // Reset decoder on IDR if there was a previous error, decoder is closed, or dimensions changed
        if (isKeyframe && (h264DecoderError || !videoDecoder || videoDecoder.state === 'closed' || needsReconfigure)) {
            if (needsReconfigure) {
                console.log(`[RDP Client] Reconfiguring H.264 decoder: ${h264ConfiguredWidth}x${h264ConfiguredHeight} -> ${surfaceW}x${surfaceH}`);
            } else {
                console.log('[RDP Client] Initializing H.264 decoder on IDR frame');
            }
            if (videoDecoder && videoDecoder.state !== 'closed') {
                try { videoDecoder.close(); } catch (e) { /* ignore */ }
            }
            h264Initialized = false;
            h264DecoderError = false;
            h264DecodeQueue = [];
            
            // Initialize with SURFACE dimensions (not tile dimensions)
            const success = await initH264Decoder(surfaceW, surfaceH);
            if (!success) {
                console.warn('[RDP Client] H.264 decoder init failed');
                return;
            }
        }
        
        if (!videoDecoder || videoDecoder.state === 'closed') {
            return;
        }
        
        // Skip P/B frames if decoder had errors (wait for next IDR)
        if (h264DecoderError && !isKeyframe) {
            return;
        }
        
        try {
            // Server transcodes AVC444 (4:4:4 dual-stream) to AVC420 (4:2:0)
            // so we receive a single, browser-compatible H.264 stream
            
            // Push metadata to queue BEFORE decode (maintains FIFO order)
            h264DecodeQueue.push({ destX, destY, destW, destH, surfaceId });
            
            // Create EncodedVideoChunk
            const chunk = new EncodedVideoChunk({
                type: isKeyframe ? 'key' : 'delta',
                timestamp: performance.now() * 1000, // microseconds
                data: nalData
            });
            
            // Queue for decoding
            videoDecoder.decode(chunk);
            
        } catch (e) {
            console.error('[RDP Client] H.264 decode failed:', e);
            // Pop the metadata we just pushed since decode failed
            h264DecodeQueue.pop();
            h264DecoderError = true;
        }
    }

    /**
     * Handle base64 encoded frame (fallback)
     */
    function handleBase64Frame(base64Data) {
        const img = new Image();
        img.onload = () => {
            ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
        };
        img.src = 'data:image/jpeg;base64,' + base64Data;
    }

    /**
     * Initialize audio context (must be called after user interaction)
     */
    function initAudio() {
        if (audioContext) return;
        
        try {
            audioContext = new (window.AudioContext || window.webkitAudioContext)();
            audioGainNode = audioContext.createGain();
            audioGainNode.connect(audioContext.destination);
            audioGainNode.gain.value = isMuted ? 0 : 1;
            console.log('[RDP Client] Audio context initialized:', audioContext.sampleRate, 'Hz');
        } catch (e) {
            console.error('[RDP Client] Failed to initialize audio:', e);
        }
    }

    /**
     * Initialize Opus decoder using WebCodecs API
     */
    async function initOpusDecoder(sampleRate, channels, forceReinit = false) {
        // Check WebCodecs support
        if (typeof AudioDecoder === 'undefined') {
            console.warn('[RDP Client] WebCodecs AudioDecoder not supported, Opus audio disabled');
            return false;
        }
        
        // Close existing decoder if format changed or force reinit requested
        if (opusDecoder && (forceReinit || opusSampleRate !== sampleRate || opusChannels !== channels)) {
            try {
                opusDecoder.close();
            } catch (e) {
                // Ignore close errors
            }
            opusDecoder = null;
            opusInitialized = false;
        }
        
        if (opusDecoder && !forceReinit) return true;
        
        try {
            opusSampleRate = sampleRate;
            opusChannels = channels;
            opusDecodeErrors = 0;  // Reset error count on new decoder
            opusFramesDecoded = 0; // Reset decoded count on new decoder
            opusFramesQueued = 0;  // Reset queued count on new decoder
            
            opusDecoder = new AudioDecoder({
                output: (audioData) => {
                    // Reset error tracking on successful decode
                    opusDecodeErrors = 0;
                    opusLastSuccessTime = performance.now();
                    opusFramesDecoded++;
                    
                    // Log first decoded frame and occasional milestones
                    if (opusFramesDecoded === 1) {
                        console.log('[RDP Client] Opus audio active: ' + 
                                    audioData.sampleRate + 'Hz, format=' + audioData.format);
                    }
                    
                    // Convert AudioData to AudioBuffer and queue for playback
                    handleDecodedOpusAudio(audioData);
                },
                error: (e) => {
                    opusDecodeErrors++;
                    console.error('[RDP Client] Opus decode error (#' + opusDecodeErrors + '):', e.message || e);
                    
                    // Trigger recovery after multiple consecutive errors
                    if (opusDecodeErrors >= 5 && !opusRecoveryPending) {
                        console.warn('[RDP Client] Too many Opus decode errors, scheduling recovery...');
                        scheduleOpusRecovery();
                    }
                }
            });
            
            await opusDecoder.configure({
                codec: 'opus',
                sampleRate: sampleRate,
                numberOfChannels: channels,
            });
            
            opusInitialized = true;
            opusRecoveryPending = false;
            console.log('[RDP Client] Opus decoder initialized:', sampleRate, 'Hz,', channels, 'ch');
            return true;
            
        } catch (e) {
            console.error('[RDP Client] Failed to initialize Opus decoder:', e);
            opusDecoder = null;
            return false;
        }
    }

    /**
     * Schedule Opus decoder recovery after errors
     */
    function scheduleOpusRecovery() {
        if (opusRecoveryPending) return;
        opusRecoveryPending = true;
        
        // Small delay to prevent rapid recovery loops
        setTimeout(async () => {
            console.log('[RDP Client] Attempting Opus decoder recovery...');
            
            // Close existing decoder
            if (opusDecoder) {
                try {
                    opusDecoder.close();
                } catch (e) {
                    // Ignore
                }
                opusDecoder = null;
            }
            opusInitialized = false;
            
            // Clear audio queue to prevent stale audio
            audioQueue = [];
            
            // Reinitialize will happen on next frame
            opusRecoveryPending = false;
            console.log('[RDP Client] Opus decoder recovery complete, will reinit on next frame');
        }, 100);
    }

    /**
     * Handle decoded Opus audio data
     */
    function handleDecodedOpusAudio(audioData) {
        if (!audioContext) {
            console.warn('[RDP Client] No audio context for decoded audio');
            audioData.close();
            return;
        }
        if (isMuted) {
            audioData.close();
            return;
        }
        
        try {
            // Resume audio context if suspended (browser autoplay policy)
            if (audioContext.state === 'suspended') {
                console.log('[RDP Client] Resuming suspended AudioContext');
                audioContext.resume();
            }
            
            // Create AudioBuffer from AudioData
            const numFrames = audioData.numberOfFrames;
            const numChannels = audioData.numberOfChannels;
            const sampleRate = audioData.sampleRate;
            
            const audioBuffer = audioContext.createBuffer(numChannels, numFrames, sampleRate);
            
            // AudioData.copyTo expects format and planeIndex options
            // The format depends on how the decoder outputs data
            const format = audioData.format; // e.g., 'f32-planar', 'f32', 's16', etc.
            
            if (format && format.includes('planar')) {
                // Planar format - copy each channel separately
                for (let ch = 0; ch < numChannels; ch++) {
                    const channelData = audioBuffer.getChannelData(ch);
                    audioData.copyTo(channelData, { planeIndex: ch });
                }
            } else {
                // Interleaved format - need to copy all data and de-interleave
                // Create a buffer large enough for all samples
                const totalSamples = numFrames * numChannels;
                let tempBuffer;
                
                // Determine the format and create appropriate buffer
                if (format === 'f32' || format === 'f32-planar') {
                    tempBuffer = new Float32Array(totalSamples);
                } else if (format === 's16' || format === 's16-planar') {
                    tempBuffer = new Int16Array(totalSamples);
                } else if (format === 's32' || format === 's32-planar') {
                    tempBuffer = new Int32Array(totalSamples);
                } else {
                    // Default to float32
                    tempBuffer = new Float32Array(totalSamples);
                }
                
                try {
                    audioData.copyTo(tempBuffer, { planeIndex: 0 });
                    
                    // De-interleave if needed
                    for (let ch = 0; ch < numChannels; ch++) {
                        const channelData = audioBuffer.getChannelData(ch);
                        for (let i = 0; i < numFrames; i++) {
                            let sample = tempBuffer[i * numChannels + ch];
                            // Convert to float if needed
                            if (format === 's16' || format === 's16-planar') {
                                sample = sample / 32768.0;
                            } else if (format === 's32' || format === 's32-planar') {
                                sample = sample / 2147483648.0;
                            }
                            channelData[i] = sample;
                        }
                    }
                } catch (copyError) {
                    // If copyTo fails, try alternative approach - copy per channel
                    for (let ch = 0; ch < numChannels; ch++) {
                        const channelData = audioBuffer.getChannelData(ch);
                        try {
                            audioData.copyTo(channelData, { planeIndex: ch, format: 'f32-planar' });
                        } catch (e2) {
                            // Last resort: try to copy without options
                            console.warn('[RDP Client] Audio copy fallback for channel', ch);
                        }
                    }
                }
            }
            
            // Queue for playback
            queueAudioBuffer(audioBuffer);
            
        } catch (e) {
            console.error('[RDP Client] Decoded audio error:', e);
        } finally {
            audioData.close();
        }
    }

    /**
     * Handle Opus audio frame from server
     * Format: [OPUS magic (4)] [sample_rate (4)] [channels (2)] [frame_size (2)] [Opus data...]
     */
    async function handleOpusFrame(bytes) {
        opusFramesReceived++;
        
        // Log first frame only
        if (opusFramesReceived === 1) {
            console.log('[RDP Client] Opus audio stream started');
        }
        
        if (!audioContext) {
            initAudio();
        }
        
        if (isMuted) return;
        
        // Skip if recovery is pending
        if (opusRecoveryPending) return;
        
        try {
            // Resume audio context if suspended
            if (audioContext && audioContext.state === 'suspended') {
                audioContext.resume();
            }
            
            // Parse header
            const dataView = new DataView(bytes.buffer, bytes.byteOffset);
            const sampleRate = dataView.getUint32(4, true);  // little-endian
            const channels = dataView.getUint16(8, true);
            const frameSize = dataView.getUint16(10, true);
            
            // Validate frame size to prevent decoding garbage
            if (frameSize === 0 || frameSize > 4000) {
                console.warn('[RDP Client] Invalid Opus frame size:', frameSize);
                return;
            }
            
            // Initialize decoder if needed or if format changed
            if (!opusInitialized || !opusDecoder || 
                opusSampleRate !== sampleRate || opusChannels !== channels) {
                const success = await initOpusDecoder(sampleRate, channels);
                if (!success) return;
            }
            
            // Check decoder state before decoding
            if (opusDecoder.state === 'closed') {
                console.warn('[RDP Client] Opus decoder is closed, reinitializing...');
                opusInitialized = false;
                const success = await initOpusDecoder(sampleRate, channels, true);
                if (!success) return;
            }
            
            // Extract Opus frame data
            const opusData = bytes.slice(12, 12 + frameSize);
            
            if (opusData.length === 0) return;
            
            // Decode Opus frame
            opusDecoder.decode(new EncodedAudioChunk({
                type: 'key',
                timestamp: performance.now() * 1000,  // microseconds
                data: opusData
            }));
            
        } catch (e) {
            console.error('[RDP Client] Opus frame error:', e);
            opusDecodeErrors++;
            
            // Schedule recovery if we're seeing repeated frame handling errors
            if (opusDecodeErrors >= 3 && !opusRecoveryPending) {
                scheduleOpusRecovery();
            }
        }
    }

    /**
     * Handle audio frame from server
     * Format: [AUDI magic (4)] [sample_rate (4)] [channels (2)] [bits (2)] [PCM data...]
     */
    function handleAudioFrame(bytes) {
        if (!audioContext || isMuted) return;
        
        try {
            // Resume audio context if suspended (browser autoplay policy)
            if (audioContext.state === 'suspended') {
                audioContext.resume();
            }
            
            // Parse header
            const dataView = new DataView(bytes.buffer, bytes.byteOffset);
            const sampleRate = dataView.getUint32(4, true);  // little-endian
            const channels = dataView.getUint16(8, true);
            const bits = dataView.getUint16(10, true);
            
            // Extract PCM data
            const pcmData = bytes.slice(12);
            
            if (pcmData.length === 0) return;
            
            // Convert PCM to AudioBuffer
            const bytesPerSample = bits / 8;
            const numSamples = Math.floor(pcmData.length / (channels * bytesPerSample));
            
            if (numSamples === 0) return;
            
            // Create audio buffer at source sample rate
            const audioBuffer = audioContext.createBuffer(channels, numSamples, sampleRate);
            
            // Convert PCM bytes to float samples
            for (let ch = 0; ch < channels; ch++) {
                const channelData = audioBuffer.getChannelData(ch);
                
                for (let i = 0; i < numSamples; i++) {
                    const sampleIndex = i * channels + ch;
                    const byteOffset = sampleIndex * bytesPerSample;
                    
                    let sample;
                    if (bits === 16) {
                        // 16-bit signed PCM (little-endian)
                        const int16 = pcmData[byteOffset] | (pcmData[byteOffset + 1] << 8);
                        sample = int16 > 32767 ? (int16 - 65536) / 32768 : int16 / 32768;
                    } else if (bits === 8) {
                        // 8-bit unsigned PCM
                        sample = (pcmData[byteOffset] - 128) / 128;
                    } else {
                        sample = 0;
                    }
                    
                    channelData[i] = sample;
                }
            }
            
            // Queue audio for playback
            queueAudioBuffer(audioBuffer);
            
        } catch (e) {
            console.error('[RDP Client] Audio frame error:', e);
        }
    }

    /**
     * Queue audio buffer for playback with jitter buffering
     * 
     * Key insight: We track audioNextPlayTime separately from the queue.
     * This prevents gaps when the queue empties (all buffers finished playing)
     * because we still know when the last buffer ended.
     */
    function queueAudioBuffer(audioBuffer) {
        opusFramesQueued++;
        
        const source = audioContext.createBufferSource();
        source.buffer = audioBuffer;
        source.connect(audioGainNode);
        
        const currentTime = audioContext.currentTime;
        const bufferDuration = audioBuffer.duration;
        
        // Clean up finished buffers from queue (for tracking purposes)
        audioQueue = audioQueue.filter(item => item.endTime > currentTime);
        
        // Determine start time for this buffer
        let startTime;
        
        if (audioNextPlayTime <= currentTime) {
            // No audio scheduled or we've fallen behind - need to resync
            // Add small initial buffer (100ms) for jitter absorption
            startTime = currentTime + 0.1;
            
            // Log initial start only
            if (opusFramesQueued === 1) {
                console.log('[RDP Client] Audio playback started');
            }
        } else {
            // Schedule immediately after the previous buffer
            startTime = audioNextPlayTime;
        }
        
        // Limit latency: if we're too far ahead, skip forward
        // Max latency = 500ms (25 buffers at 20ms each)
        const maxLatency = 0.5;
        if (startTime > currentTime + maxLatency) {
            // Too much latency - drop some time
            startTime = currentTime + 0.1;
            // Latency reset - don't log to avoid spam during bursts
        }
        
        // Schedule the buffer
        source.start(startTime);
        
        // Update tracking
        audioNextPlayTime = startTime + bufferDuration;
        audioQueue.push({ endTime: audioNextPlayTime, source: source });
        
        // Limit queue size (for memory, not scheduling)
        while (audioQueue.length > 50) {
            audioQueue.shift();
        }
    }

    /**
     * Toggle audio mute
     */
    function toggleMute() {
        isMuted = !isMuted;
        
        if (audioGainNode) {
            audioGainNode.gain.value = isMuted ? 0 : 1;
        }
        
        elements.muteBtn.textContent = isMuted ? '🔇' : '🔊';
        console.log('[RDP Client] Audio', isMuted ? 'muted' : 'unmuted');
    }

    /**
     * Cleanup audio resources
     */
    function cleanupAudio() {
        audioQueue = [];
        audioNextPlayTime = 0;
        
        // Clean up Opus decoder
        if (opusDecoder) {
            try {
                opusDecoder.close();
            } catch (e) {
                // Ignore errors during cleanup
            }
            opusDecoder = null;
            opusInitialized = false;
        }
        
        // Reset audio error tracking and counters
        opusDecodeErrors = 0;
        opusLastSuccessTime = 0;
        opusRecoveryPending = false;
        opusFramesReceived = 0;
        opusFramesDecoded = 0;
        opusFramesQueued = 0;
        
        if (audioContext) {
            audioContext.close();
            audioContext = null;
            audioGainNode = null;
        }
    }

    /**
     * Cleanup H.264 decoder resources
     */
    function cleanupH264Decoder() {
        if (videoDecoder) {
            try {
                videoDecoder.close();
            } catch (e) {
                // Ignore errors during cleanup
            }
            videoDecoder = null;
            h264Initialized = false;
        }
        
        h264FrameCount = 0;
        lastH264FrameId = 0;
        pendingH264Frames = [];
        gfxSurfaces.clear();
    }

    /**
     * Calculate available dimensions from the container
     */
    function getAvailableDimensions() {
        const container = elements.vmScreen;
        const containerRect = container.getBoundingClientRect();

        // Calculate available space (subtract some padding for borders)
        let width = Math.floor(containerRect.width - 4);
        let height = Math.floor(containerRect.height - 4);

        // Enforce minimum and maximum bounds
        width = Math.max(640, Math.min(width, 4096));
        height = Math.max(480, Math.min(height, 2160));

        // Round to even numbers (some encoders require this)
        width = Math.floor(width / 2) * 2;
        height = Math.floor(height / 2) * 2;

        return { width, height };
    }

    /**
     * Handle window/container resize with debounce
     */
    function handleWindowResize() {
        if (!isConnected) return;

        // Clear any pending resize timeout
        if (resizeTimeout) {
            clearTimeout(resizeTimeout);
        }

        pendingResize = true;

        // Debounce: wait 2 seconds after last resize event before renegotiating
        resizeTimeout = setTimeout(() => {
            if (!isConnected || !pendingResize) return;

            const { width: newWidth, height: newHeight } = getAvailableDimensions();

            // Only request resize if dimensions actually changed from both canvas AND last request
            if ((newWidth !== canvas.width || newHeight !== canvas.height) &&
                (newWidth !== lastRequestedWidth || newHeight !== lastRequestedHeight)) {
                console.log('[RDP Client] Requesting resize to:', newWidth, 'x', newHeight);
                lastRequestedWidth = newWidth;
                lastRequestedHeight = newHeight;
                sendMessage({
                    type: 'resize',
                    width: newWidth,
                    height: newHeight
                });
            } else {
                console.log('[RDP Client] Skipping redundant resize request');
            }

            pendingResize = false;
        }, config.resizeDebounceMs);
    }

    /**
     * Handle resize confirmation from server
     */
    function handleResize(width, height) {
        // Skip if dimensions haven't changed
        if (canvas.width === width && canvas.height === height) {
            return;
        }
        canvas.width = width;
        canvas.height = height;
        elements.resolution.textContent = `Resolution: ${width}x${height}`;
        console.log('[RDP Client] Resized to:', width, 'x', height);
    }

    /**
     * Handle error message
     */
    function handleError(message) {
        console.error('[RDP Client] Server error:', message);
        updateStatus('error', 'Error: ' + message);
        alert('RDP Error: ' + message);
    }

    /**
     * Send ping for latency measurement
     */
    function sendPing() {
        if (isConnected) {
            pingStart = performance.now();
            sendMessage({ type: 'ping' });
        }
    }

    /**
     * Handle pong response
     */
    function handlePong() {
        const latency = Math.round(performance.now() - pingStart);
        elements.latency.textContent = `Latency: ${latency}ms`;
    }

    /**
     * Update connection status UI
     */
    function updateStatus(status, text) {
        elements.statusDot.className = 'status-dot';
        if (status === 'connected') {
            elements.statusDot.classList.add('connected');
        }
        elements.statusText.textContent = text;
    }

    /**
     * Toggle fullscreen mode
     */
    function toggleFullscreen() {
        if (document.fullscreenElement) {
            document.exitFullscreen();
        } else {
            elements.vmScreen.requestFullscreen();
        }
    }

    /**
     * Get mouse position relative to canvas
     */
    function getMousePos(e) {
        const rect = canvas.getBoundingClientRect();
        const scaleX = canvas.width / rect.width;
        const scaleY = canvas.height / rect.height;
        return {
            x: Math.round((e.clientX - rect.left) * scaleX),
            y: Math.round((e.clientY - rect.top) * scaleY)
        };
    }

    /**
     * Handle mouse move
     */
    function handleMouseMove(e) {
        if (!isConnected) return;

        const now = Date.now();
        if (now - lastMouseSend < config.mouseThrottleMs) return;
        lastMouseSend = now;

        const pos = getMousePos(e);
        sendMessage({
            type: 'mouse',
            action: 'move',
            x: pos.x,
            y: pos.y
        });
    }

    /**
     * Handle mouse down
     */
    function handleMouseDown(e) {
        if (!isConnected) return;
        e.preventDefault();
        canvas.focus();

        const pos = getMousePos(e);
        sendMessage({
            type: 'mouse',
            action: 'down',
            button: e.button, // 0=left, 1=middle, 2=right
            x: pos.x,
            y: pos.y
        });
    }

    /**
     * Handle mouse up
     */
    function handleMouseUp(e) {
        if (!isConnected) return;
        e.preventDefault();

        const pos = getMousePos(e);
        sendMessage({
            type: 'mouse',
            action: 'up',
            button: e.button,
            x: pos.x,
            y: pos.y
        });
    }

    /**
     * Handle mouse wheel
     */
    function handleMouseWheel(e) {
        if (!isConnected) return;
        e.preventDefault();

        const pos = getMousePos(e);
        sendMessage({
            type: 'mouse',
            action: 'wheel',
            deltaX: e.deltaX,
            deltaY: e.deltaY,
            x: pos.x,
            y: pos.y
        });
    }

    /**
     * Handle key down
     */
    function handleKeyDown(e) {
        if (!isConnected) return;
        if (document.activeElement !== canvas) return;
        
        e.preventDefault();
        sendMessage({
            type: 'key',
            action: 'down',
            key: e.key,
            code: e.code,
            keyCode: e.keyCode,
            ctrlKey: e.ctrlKey,
            shiftKey: e.shiftKey,
            altKey: e.altKey,
            metaKey: e.metaKey
        });
    }

    /**
     * Handle key up
     */
    function handleKeyUp(e) {
        if (!isConnected) return;
        if (document.activeElement !== canvas) return;

        e.preventDefault();
        sendMessage({
            type: 'key',
            action: 'up',
            key: e.key,
            code: e.code,
            keyCode: e.keyCode,
            ctrlKey: e.ctrlKey,
            shiftKey: e.shiftKey,
            altKey: e.altKey,
            metaKey: e.metaKey
        });
    }

    /**
     * Send message to server
     */
    function sendMessage(msg) {
        if (ws && ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify(msg));
        }
    }

    // ========================================
    // Public API
    // ========================================

    /**
     * Send keys to the VM programmatically
     * @param {string|string[]} keys - Key(s) to send. Can be a string like "Hello" or array of key codes
     * @param {object} options - Options like { ctrl: true, alt: false, shift: false }
     * @returns {Promise<void>}
     */
    function sendKeys(keys, options = {}) {
        return new Promise((resolve, reject) => {
            if (!isConnected) {
                reject(new Error('Not connected to VM'));
                return;
            }

            const keyArray = Array.isArray(keys) ? keys : keys.split('');
            const delay = options.delay || 50;

            let index = 0;

            function sendNextKey() {
                if (index >= keyArray.length) {
                    resolve();
                    return;
                }

                const key = keyArray[index];
                
                // Send key down
                sendMessage({
                    type: 'key',
                    action: 'down',
                    key: key,
                    code: `Key${key.toUpperCase()}`,
                    ctrlKey: options.ctrl || false,
                    shiftKey: options.shift || false,
                    altKey: options.alt || false,
                    metaKey: options.meta || false
                });

                // Send key up after small delay
                setTimeout(() => {
                    sendMessage({
                        type: 'key',
                        action: 'up',
                        key: key,
                        code: `Key${key.toUpperCase()}`,
                        ctrlKey: options.ctrl || false,
                        shiftKey: options.shift || false,
                        altKey: options.alt || false,
                        metaKey: options.meta || false
                    });

                    index++;
                    setTimeout(sendNextKey, delay);
                }, 20);
            }

            sendNextKey();
        });
    }

    /**
     * Send a special key combination
     * @param {string} combo - Key combination like "Ctrl+Alt+Delete"
     */
    function sendKeyCombo(combo) {
        if (!isConnected) {
            throw new Error('Not connected to VM');
        }

        sendMessage({
            type: 'keycombo',
            combo: combo
        });
    }

    /**
     * Send Ctrl+Alt+Delete
     */
    function sendCtrlAltDel() {
        sendKeyCombo('Ctrl+Alt+Delete');
    }

    /**
     * Get connection status
     */
    function getStatus() {
        return {
            connected: isConnected,
            wsState: ws ? ws.readyState : null
        };
    }

    // Expose public API
    window.vmClient = {
        connect,
        disconnect,
        sendKeys,
        sendKeyCombo,
        sendCtrlAltDel,
        getStatus,
        config
    };

    // Initialize on DOM ready
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }

})();
