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
    
    // Audio state
    let audioContext = null;
    let audioQueue = [];
    let isAudioPlaying = false;
    let isMuted = false;
    let audioGainNode = null;
    
    // Opus decoder state
    let opusDecoder = null;
    let opusInitialized = false;
    let opusSampleRate = 48000;
    let opusChannels = 2;

    // Audio frame magic headers
    const AUDIO_MAGIC = [0x41, 0x55, 0x44, 0x49];  // "AUDI" (PCM - legacy)
    const OPUS_MAGIC = [0x4F, 0x50, 0x55, 0x53];   // "OPUS" (Opus encoded)

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
        updateStatus('disconnected', 'Disconnected');
        elements.canvas.style.display = 'none';
        elements.loading.style.display = 'block';
        elements.loading.querySelector('p').textContent = 'Click Connect to start';
        elements.connectBtn.disabled = false;
        elements.disconnectBtn.disabled = true;
        elements.muteBtn.disabled = true;
        
        // Cleanup audio
        cleanupAudio();
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
                if (width !== canvas.width || height !== canvas.height) {
                    console.log('[RDP Client] Post-connect resize to:', width, 'x', height);
                    sendMessage({ type: 'resize', width, height });
                }
            }
        }, 500);

        // Start ping for latency measurement
        setInterval(sendPing, 5000);
        console.log('[RDP Client] RDP session established');
    }

    /**
     * Handle binary frame update (WebP full frame or delta frame)
     */
    function handleFrameUpdate(data) {
        const bytes = new Uint8Array(data);
        
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
            ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
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
                
                img.onload = () => {
                    ctx.drawImage(img, tileX, tileY);
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
    async function initOpusDecoder(sampleRate, channels) {
        // Check WebCodecs support
        if (typeof AudioDecoder === 'undefined') {
            console.warn('[RDP Client] WebCodecs AudioDecoder not supported, Opus audio disabled');
            return false;
        }
        
        // Close existing decoder if format changed
        if (opusDecoder && (opusSampleRate !== sampleRate || opusChannels !== channels)) {
            opusDecoder.close();
            opusDecoder = null;
            opusInitialized = false;
        }
        
        if (opusDecoder) return true;
        
        try {
            opusSampleRate = sampleRate;
            opusChannels = channels;
            
            opusDecoder = new AudioDecoder({
                output: (audioData) => {
                    // Convert AudioData to AudioBuffer and queue for playback
                    handleDecodedOpusAudio(audioData);
                },
                error: (e) => {
                    console.error('[RDP Client] Opus decode error:', e);
                }
            });
            
            await opusDecoder.configure({
                codec: 'opus',
                sampleRate: sampleRate,
                numberOfChannels: channels,
            });
            
            opusInitialized = true;
            console.log('[RDP Client] Opus decoder initialized:', sampleRate, 'Hz,', channels, 'ch');
            return true;
            
        } catch (e) {
            console.error('[RDP Client] Failed to initialize Opus decoder:', e);
            opusDecoder = null;
            return false;
        }
    }

    /**
     * Handle decoded Opus audio data
     */
    function handleDecodedOpusAudio(audioData) {
        if (!audioContext || isMuted) {
            audioData.close();
            return;
        }
        
        try {
            // Resume audio context if suspended (browser autoplay policy)
            if (audioContext.state === 'suspended') {
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
        if (!audioContext) {
            initAudio();
        }
        
        if (isMuted) return;
        
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
            
            // Initialize decoder if needed
            if (!opusInitialized) {
                const success = await initOpusDecoder(sampleRate, channels);
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
     */
    function queueAudioBuffer(audioBuffer) {
        const source = audioContext.createBufferSource();
        source.buffer = audioBuffer;
        source.connect(audioGainNode);
        
        // Calculate when to start this buffer
        const currentTime = audioContext.currentTime;
        const bufferDuration = audioBuffer.duration;
        
        // Clean up finished buffers first
        audioQueue = audioQueue.filter(item => item.endTime > currentTime);
        
        // Calculate start time for new buffer
        const lastEndTime = audioQueue.length > 0 
            ? Math.max(...audioQueue.map(item => item.endTime))
            : currentTime;
        
        // Jitter buffer: add initial delay to absorb network jitter
        // Use 150ms initial buffer, then schedule seamlessly
        const initialBufferDelay = audioQueue.length === 0 ? 0.15 : 0;
        
        // Limit queue size to prevent excessive latency buildup
        // With 20ms Opus frames, 25 buffers = 500ms max latency
        const maxQueueSize = 25;
        
        if (audioQueue.length < maxQueueSize) {
            // Schedule buffer to play after previous one ends
            const startTime = Math.max(currentTime + initialBufferDelay, lastEndTime);
            source.start(startTime);
            audioQueue.push({ endTime: startTime + bufferDuration, source: source });
        } else {
            // Queue full - drop oldest buffer to reduce latency
            // This shouldn't happen often if encoding/decoding keeps up
            const oldest = audioQueue.shift();
            if (oldest && oldest.source) {
                try { oldest.source.stop(); } catch(e) {}
            }
            const startTime = Math.max(currentTime, lastEndTime);
            source.start(startTime);
            audioQueue.push({ endTime: startTime + bufferDuration, source: source });
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
        
        if (audioContext) {
            audioContext.close();
            audioContext = null;
            audioGainNode = null;
        }
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

            // Only request resize if dimensions actually changed
            if (newWidth !== canvas.width || newHeight !== canvas.height) {
                console.log('[RDP Client] Requesting resize to:', newWidth, 'x', newHeight);
                sendMessage({
                    type: 'resize',
                    width: newWidth,
                    height: newHeight
                });
            }

            pendingResize = false;
        }, config.resizeDebounceMs);
    }

    /**
     * Handle resize confirmation from server
     */
    function handleResize(width, height) {
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
