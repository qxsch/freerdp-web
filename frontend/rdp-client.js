/**
 * RDP Web Client - Reusable ES Module
 * Shadow DOM isolated component for browser-based RDP via WebSocket
 * 
 * @example
 * import { RDPClient } from './rdp-client.js';
 * 
 * const client = new RDPClient(document.getElementById('container'), {
 *   wsUrl: 'ws://localhost:8765',
 *   showTopBar: true,
 *   showBottomBar: true
 * });
 * 
 * await client.connect({ host: '192.168.1.100', user: 'admin', pass: 'secret' });
 */

// ============================================================
// STYLES - Shadow DOM isolated styles
// ============================================================
const STYLES = `
:host {
    display: block;
    width: 100%;
    height: 100%;
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    --rdp-bg: #1a1a2e;
    --rdp-header-bg: #16213e;
    --rdp-border: #0f3460;
    --rdp-text: #eee;
    --rdp-text-muted: #888;
    --rdp-accent: #51cf66;
    --rdp-error: #ff6b6b;
    --rdp-btn-bg: #0f3460;
    --rdp-btn-hover: #1a4a7a;
}

* {
    margin: 0;
    padding: 0;
    box-sizing: border-box;
}

.rdp-container {
    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
    background: var(--rdp-bg);
    color: var(--rdp-text);
    overflow: hidden;
}

/* Top Bar */
.rdp-topbar {
    background: var(--rdp-header-bg);
    padding: 8px 16px;
    display: flex;
    align-items: center;
    gap: 16px;
    border-bottom: 1px solid var(--rdp-border);
    flex-shrink: 0;
}

.rdp-topbar.hidden { display: none; }

.rdp-status {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 0.85rem;
}

.rdp-status-dot {
    width: 10px;
    height: 10px;
    border-radius: 50%;
    background: var(--rdp-error);
    transition: background 0.3s;
}

.rdp-status-dot.connected { background: var(--rdp-accent); }

.rdp-controls {
    margin-left: auto;
    display: flex;
    gap: 8px;
}

.rdp-btn {
    background: var(--rdp-btn-bg);
    border: none;
    color: var(--rdp-text);
    padding: 6px 14px;
    border-radius: 4px;
    cursor: pointer;
    font-size: 0.85rem;
    transition: background 0.2s;
}

.rdp-btn:hover { background: var(--rdp-btn-hover); }
.rdp-btn:disabled { opacity: 0.5; cursor: not-allowed; }

/* Screen Area */
.rdp-screen-wrapper {
    flex: 1;
    display: flex;
    justify-content: center;
    align-items: stretch;
    padding: 8px;
    min-height: 0;
    overflow: hidden;
}

.rdp-screen {
    background: #000;
    border: 2px solid var(--rdp-border);
    border-radius: 4px;
    overflow: hidden;
    position: relative;
    flex: 1;
    min-height: 200px;
    display: flex;
    justify-content: center;
    align-items: center;
}

.rdp-screen canvas {
    display: block;
    max-width: 100%;
    max-height: 100%;
}

.rdp-loading {
    position: absolute;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    text-align: center;
}

.rdp-spinner {
    width: 36px;
    height: 36px;
    border: 3px solid #333;
    border-top-color: var(--rdp-accent);
    border-radius: 50%;
    animation: rdp-spin 1s linear infinite;
    margin: 0 auto 12px;
}

@keyframes rdp-spin {
    to { transform: rotate(360deg); }
}

/* Bottom Bar */
.rdp-bottombar {
    background: var(--rdp-header-bg);
    padding: 6px 16px;
    font-size: 0.8rem;
    color: var(--rdp-text-muted);
    display: flex;
    justify-content: space-between;
    border-top: 1px solid var(--rdp-border);
    flex-shrink: 0;
}

.rdp-bottombar.hidden { display: none; }

/* Modal */
.rdp-modal {
    display: none;
    position: absolute;
    top: 0;
    left: 0;
    width: 100%;
    height: 100%;
    background: rgba(0, 0, 0, 0.7);
    justify-content: center;
    align-items: center;
    z-index: 1000;
}

.rdp-modal.active { display: flex; }

.rdp-modal-content {
    background: var(--rdp-header-bg);
    padding: 24px;
    border-radius: 8px;
    max-width: 360px;
    width: 90%;
}

.rdp-modal-content h2 {
    margin-bottom: 16px;
    font-size: 1.1rem;
}

.rdp-form-group {
    margin-bottom: 12px;
}

.rdp-form-group label {
    display: block;
    margin-bottom: 4px;
    font-size: 0.85rem;
}

.rdp-form-group input {
    width: 100%;
    padding: 8px;
    border: 1px solid var(--rdp-border);
    border-radius: 4px;
    background: var(--rdp-bg);
    color: var(--rdp-text);
    font-size: 0.9rem;
}

.rdp-form-group input:focus {
    outline: none;
    border-color: var(--rdp-accent);
}

.rdp-modal-buttons {
    display: flex;
    gap: 8px;
    margin-top: 16px;
}

.rdp-modal-buttons .rdp-btn { flex: 1; }
.rdp-modal-buttons .rdp-btn-primary { background: var(--rdp-accent); color: #000; }
`;

// ============================================================
// HTML TEMPLATE
// ============================================================
const TEMPLATE = `
<div class="rdp-container">
    <div class="rdp-topbar">
        <div class="rdp-status">
            <div class="rdp-status-dot"></div>
            <span class="rdp-status-text">Disconnected</span>
        </div>
        <div class="rdp-controls">
            <button class="rdp-btn rdp-btn-connect">Connect</button>
            <button class="rdp-btn rdp-btn-disconnect" disabled>Disconnect</button>
            <button class="rdp-btn rdp-btn-mute" disabled title="Toggle Audio">🔊</button>
            <button class="rdp-btn rdp-btn-fullscreen">⛶</button>
        </div>
    </div>

    <div class="rdp-screen-wrapper">
        <div class="rdp-screen">
            <div class="rdp-loading">
                <div class="rdp-spinner"></div>
                <p>Click Connect to start</p>
            </div>
            <canvas class="rdp-canvas" width="1280" height="720" style="display: none;"></canvas>
        </div>
    </div>

    <div class="rdp-bottombar">
        <span class="rdp-resolution">Resolution: --</span>
        <span class="rdp-latency">Latency: --</span>
    </div>

    <div class="rdp-modal">
        <div class="rdp-modal-content">
            <h2>Connect to Remote Desktop</h2>
            <div class="rdp-form-group">
                <label>Host</label>
                <input type="text" class="rdp-input-host" placeholder="192.168.1.100">
            </div>
            <div class="rdp-form-group">
                <label>Port</label>
                <input type="number" class="rdp-input-port" value="3389">
            </div>
            <div class="rdp-form-group">
                <label>Username</label>
                <input type="text" class="rdp-input-user" placeholder="Administrator">
            </div>
            <div class="rdp-form-group">
                <label>Password</label>
                <input type="password" class="rdp-input-pass" placeholder="Password">
            </div>
            <div class="rdp-modal-buttons">
                <button class="rdp-btn rdp-btn-primary rdp-modal-connect">Connect</button>
                <button class="rdp-btn rdp-modal-cancel">Cancel</button>
            </div>
        </div>
    </div>
</div>
`;

// ============================================================
// CONSTANTS
// ============================================================
const OPUS_MAGIC = [0x4F, 0x50, 0x55, 0x53];  // "OPUS"
const AUDIO_MAGIC = [0x41, 0x55, 0x44, 0x49]; // "AUDI"
const H264_MAGIC = [0x48, 0x32, 0x36, 0x34];  // "H264"

// New GFX event stream magic codes (4 bytes each)
const SURF_MAGIC = [0x53, 0x55, 0x52, 0x46];  // "SURF" - createSurface
const DELS_MAGIC = [0x44, 0x45, 0x4C, 0x53];  // "DELS" - deleteSurface
const STFR_MAGIC = [0x53, 0x54, 0x46, 0x52];  // "STFR" - startFrame
const ENFR_MAGIC = [0x45, 0x4E, 0x46, 0x52];  // "ENFR" - endFrame
const PROG_MAGIC = [0x50, 0x52, 0x4F, 0x47];  // "PROG" - progressive tile
const WEBP_MAGIC = [0x57, 0x45, 0x42, 0x50];  // "WEBP" - WebP tile
const TILE_MAGIC = [0x54, 0x49, 0x4C, 0x45];  // "TILE" - raw RGBA tile
const SFIL_MAGIC = [0x53, 0x46, 0x49, 0x4C];  // "SFIL" - solidFill
const S2SF_MAGIC = [0x53, 0x32, 0x53, 0x46];  // "S2SF" - surfaceToSurface
const C2SF_MAGIC = [0x43, 0x32, 0x53, 0x46];  // "C2SF" - cacheToSurface

// ============================================================
// RDP CLIENT CLASS
// ============================================================
export class RDPClient {
    /**
     * Create an RDP Client instance
     * @param {HTMLElement} container - Container element to attach to
     * @param {Object} options - Configuration options
     * @param {string} [options.wsUrl='ws://localhost:8765'] - WebSocket server URL
     * @param {boolean} [options.showTopBar=true] - Show top toolbar
     * @param {boolean} [options.showBottomBar=true] - Show bottom status bar
     * @param {number} [options.reconnectDelay=3000] - Reconnection delay in ms
     */
    constructor(container, options = {}) {
        this.options = {
            wsUrl: 'ws://localhost:8765',
            showTopBar: true,
            showBottomBar: true,
            reconnectDelay: 3000,
            mouseThrottleMs: 16,
            resizeDebounceMs: 2000,
            ...options
        };

        this._container = container;
        this._initShadowDOM();
        this._initState();
        this._bindElements();
        this._setupEventListeners();
        
        console.log('[RDPClient] Initialized');
    }

    // --------------------------------------------------
    // INITIALIZATION
    // --------------------------------------------------
    
    _initShadowDOM() {
        this._shadow = this._container.attachShadow({ mode: 'open' });
        
        const style = document.createElement('style');
        style.textContent = STYLES;
        
        const template = document.createElement('template');
        template.innerHTML = TEMPLATE;
        
        this._shadow.appendChild(style);
        this._shadow.appendChild(template.content.cloneNode(true));
        
        // Apply visibility options
        if (!this.options.showTopBar) {
            this._shadow.querySelector('.rdp-topbar').classList.add('hidden');
        }
        if (!this.options.showBottomBar) {
            this._shadow.querySelector('.rdp-bottombar').classList.add('hidden');
        }
    }

    _initState() {
        this._ws = null;
        this._isConnected = false;
        this._canvas = null;
        this._ctx = null;
        this._lastMouseSend = 0;
        this._pingStart = 0;
        this._resizeTimeout = null;
        this._lastRequestedWidth = 0;
        this._lastRequestedHeight = 0;
        
        // Audio state
        this._audioContext = null;
        this._audioQueue = [];
        this._isMuted = false;
        this._audioGainNode = null;
        this._audioNextPlayTime = 0;
        
        // Opus decoder
        this._opusDecoder = null;
        this._opusInitialized = false;
        this._opusSampleRate = 48000;
        this._opusChannels = 2;
        
        // GFX Worker for progressive/tile rendering
        this._gfxWorker = null;
        this._gfxWorkerReady = false;
        this._useOffscreenCanvas = typeof OffscreenCanvas !== 'undefined';
        this._pendingGfxMessages = [];
        
        // Event callbacks
        this._eventHandlers = {};
    }

    _bindElements() {
        const $ = sel => this._shadow.querySelector(sel);
        
        this._el = {
            container: $('.rdp-container'),
            screen: $('.rdp-screen'),
            canvas: $('.rdp-canvas'),
            loading: $('.rdp-loading'),
            statusDot: $('.rdp-status-dot'),
            statusText: $('.rdp-status-text'),
            btnConnect: $('.rdp-btn-connect'),
            btnDisconnect: $('.rdp-btn-disconnect'),
            btnMute: $('.rdp-btn-mute'),
            btnFullscreen: $('.rdp-btn-fullscreen'),
            modal: $('.rdp-modal'),
            modalConnect: $('.rdp-modal-connect'),
            modalCancel: $('.rdp-modal-cancel'),
            inputHost: $('.rdp-input-host'),
            inputPort: $('.rdp-input-port'),
            inputUser: $('.rdp-input-user'),
            inputPass: $('.rdp-input-pass'),
            resolution: $('.rdp-resolution'),
            latency: $('.rdp-latency'),
        };
        
        this._canvas = this._el.canvas;
        
        // Initialize GFX worker - OffscreenCanvas is required
        this._initGfxWorker();
    }
    
    /**
     * Initialize the GFX compositor worker
     * OffscreenCanvas is REQUIRED - will fail if not supported
     */
    _initGfxWorker() {
        if (!this._useOffscreenCanvas) {
            const msg = 'OffscreenCanvas is required but not supported by this browser';
            console.error('[RDPClient]', msg);
            this._handleError(msg);
            return;
        }
        
        try {
            this._gfxWorker = new Worker('./gfx-worker.js', { type: 'module' });
            
            this._gfxWorker.onmessage = (event) => {
                this._handleGfxWorkerMessage(event.data);
            };
            
            this._gfxWorker.onerror = (err) => {
                console.error('[RDPClient] GFX Worker error:', err);
                this._gfxWorker = null;
                this._gfxWorkerReady = false;
                this._handleError('GFX Worker failed to initialize');
            };
            
            console.log('[RDPClient] GFX Worker created');
        } catch (err) {
            console.error('[RDPClient] Failed to create GFX Worker:', err);
            this._gfxWorker = null;
            this._handleError('Failed to create GFX Worker: ' + err.message);
        }
    }
    
    /**
     * Handle messages from GFX worker
     */
    _handleGfxWorkerMessage(msg) {
        switch (msg.type) {
            case 'loaded':
                console.log('[RDPClient] GFX Worker loaded');
                break;
                
            case 'ready':
                this._gfxWorkerReady = true;
                console.log('[RDPClient] GFX Worker ready, WASM:', msg.wasmReady);
                // Flush pending messages
                for (const pending of this._pendingGfxMessages) {
                    this._gfxWorker.postMessage(pending.msg, pending.transfer);
                }
                this._pendingGfxMessages = [];
                break;
                
            case 'frameAck':
                // Send frame acknowledgment back to server
                if (this._ws && this._ws.readyState === WebSocket.OPEN && msg.data) {
                    this._ws.send(msg.data);
                }
                break;
                
            case 'backpressure':
                // Send backpressure signal to server
                if (this._ws && this._ws.readyState === WebSocket.OPEN && msg.data) {
                    this._ws.send(msg.data);
                }
                break;
                
            case 'unhandled':
                // Unhandled message from worker - process on main thread
                if (msg.data) {
                    this._handleFrameUpdate(msg.data);
                }
                break;
        }
    }
    
    /**
     * Initialize GFX worker canvas when connected
     */
    _initGfxWorkerCanvas(width, height) {
        if (!this._gfxWorker || !this._useOffscreenCanvas) {
            this._handleError('GFX Worker not available');
            this.disconnect();
            return;
        }
        
        try {
            // Transfer canvas control to worker
            const offscreen = this._canvas.transferControlToOffscreen();
            
            this._gfxWorker.postMessage({
                type: 'init',
                data: { canvas: offscreen, width, height }
            }, [offscreen]);
            
            console.log('[RDPClient] Canvas transferred to GFX Worker');
        } catch (err) {
            console.error('[RDPClient] Failed to transfer canvas:', err);
            this._handleError('Failed to transfer canvas to worker: ' + err.message);
            this.disconnect();
        }
    }
    
    /**
     * Send message to GFX worker
     */
    _sendToGfxWorker(data) {
        if (!this._gfxWorker) return false;
        
        const msg = { type: 'binary', data };
        const transfer = data instanceof ArrayBuffer ? [data] : [];
        
        if (this._gfxWorkerReady) {
            this._gfxWorker.postMessage(msg, transfer);
        } else {
            this._pendingGfxMessages.push({ msg, transfer });
        }
        
        return true;
    }

    _setupEventListeners() {
        // UI buttons
        this._el.btnConnect.addEventListener('click', () => this._showModal());
        this._el.btnDisconnect.addEventListener('click', () => this.disconnect());
        this._el.btnMute.addEventListener('click', () => this._toggleMute());
        this._el.btnFullscreen.addEventListener('click', () => this._toggleFullscreen());
        
        // Modal
        this._el.modalConnect.addEventListener('click', () => this._handleModalConnect());
        this._el.modalCancel.addEventListener('click', () => this._hideModal());
        this._el.modal.addEventListener('click', (e) => {
            if (e.target === this._el.modal) this._hideModal();
        });

        // Canvas interactions
        this._canvas.setAttribute('tabindex', '0');
        this._canvas.addEventListener('click', () => this._canvas.focus());
        this._canvas.addEventListener('mousemove', (e) => this._handleMouseMove(e));
        this._canvas.addEventListener('mousedown', (e) => this._handleMouseDown(e));
        this._canvas.addEventListener('mouseup', (e) => this._handleMouseUp(e));
        this._canvas.addEventListener('wheel', (e) => this._handleMouseWheel(e));
        this._canvas.addEventListener('contextmenu', (e) => e.preventDefault());

        // Keyboard - scoped to shadow root
        this._shadow.addEventListener('keydown', (e) => this._handleKeyDown(e));
        this._shadow.addEventListener('keyup', (e) => this._handleKeyUp(e));

        // Resize observer
        if (typeof ResizeObserver !== 'undefined') {
            this._resizeObserver = new ResizeObserver(() => this._handleResize());
            this._resizeObserver.observe(this._el.screen);
        }
    }

    // --------------------------------------------------
    // PUBLIC API
    // --------------------------------------------------

    /**
     * Connect to an RDP server
     * @param {Object} credentials
     * @param {string} credentials.host - RDP server hostname/IP
     * @param {number} [credentials.port=3389] - RDP port
     * @param {string} credentials.user - Username
     * @param {string} credentials.pass - Password
     * @returns {Promise<void>}
     */
    connect(credentials) {
        return new Promise((resolve, reject) => {
            if (this._ws && this._ws.readyState === WebSocket.OPEN) {
                reject(new Error('Already connected'));
                return;
            }

            this._pendingConnect = { resolve, reject };
            this._updateStatus('connecting', 'Connecting...');
            this._el.loading.querySelector('p').textContent = 'Connecting...';

            this._ws = new WebSocket(this.options.wsUrl);
            this._ws.binaryType = 'arraybuffer';

            this._ws.onopen = () => {
                const { width, height } = this._getAvailableDimensions();
                this._lastRequestedWidth = width;
                this._lastRequestedHeight = height;
                this._canvas.width = width;
                this._canvas.height = height;

                this._sendMessage({
                    type: 'connect',
                    host: credentials.host,
                    port: credentials.port || 3389,
                    username: credentials.user,
                    password: credentials.pass,
                    width,
                    height
                });
            };

            this._ws.onmessage = (e) => this._handleMessage(e);
            this._ws.onerror = (e) => {
                this._updateStatus('error', 'Connection error');
                if (this._pendingConnect) {
                    this._pendingConnect.reject(new Error('WebSocket error'));
                    this._pendingConnect = null;
                }
            };
            this._ws.onclose = () => this._handleDisconnect();
        });
    }

    /**
     * Disconnect from the RDP server
     */
    disconnect() {
        if (this._ws) {
            this._sendMessage({ type: 'disconnect' });
            this._ws.close();
        }
        this._handleDisconnect();
    }

    /**
     * Send keystrokes to the remote desktop
     * @param {string|string[]} keys - Keys to send
     * @param {Object} [options] - Modifier options
     * @returns {Promise<void>}
     */
    sendKeys(keys, options = {}) {
        return new Promise((resolve, reject) => {
            if (!this._isConnected) {
                reject(new Error('Not connected'));
                return;
            }

            const keyArray = Array.isArray(keys) ? keys : keys.split('');
            const delay = options.delay || 50;
            let index = 0;

            const sendNext = () => {
                if (index >= keyArray.length) {
                    resolve();
                    return;
                }

                const key = keyArray[index];
                this._sendMessage({
                    type: 'key', action: 'down', key,
                    code: `Key${key.toUpperCase()}`,
                    ctrlKey: options.ctrl || false,
                    shiftKey: options.shift || false,
                    altKey: options.alt || false,
                    metaKey: options.meta || false
                });

                setTimeout(() => {
                    this._sendMessage({
                        type: 'key', action: 'up', key,
                        code: `Key${key.toUpperCase()}`,
                        ctrlKey: options.ctrl || false,
                        shiftKey: options.shift || false,
                        altKey: options.alt || false,
                        metaKey: options.meta || false
                    });
                    index++;
                    setTimeout(sendNext, delay);
                }, 20);
            };

            sendNext();
        });
    }

    /**
     * Send a key combination (e.g., "Ctrl+Alt+Delete")
     * @param {string} combo - Key combination
     */
    sendKeyCombo(combo) {
        if (!this._isConnected) throw new Error('Not connected');
        this._sendMessage({ type: 'keycombo', combo });
    }

    /**
     * Send Ctrl+Alt+Delete
     */
    sendCtrlAltDel() {
        this.sendKeyCombo('Ctrl+Alt+Delete');
    }

    /**
     * Get current connection status
     * @returns {Object} Status object
     */
    getStatus() {
        return {
            connected: this._isConnected,
            resolution: this._isConnected ? 
                { width: this._canvas.width, height: this._canvas.height } : null,
            muted: this._isMuted
        };
    }

    /**
     * Set mute state
     * @param {boolean} muted
     */
    setMuted(muted) {
        this._isMuted = muted;
        if (this._audioGainNode) {
            this._audioGainNode.gain.value = muted ? 0 : 1;
        }
        this._el.btnMute.textContent = muted ? '🔇' : '🔊';
    }

    /**
     * Get current mute state
     * @returns {boolean} True if muted
     */
    getMuted() {
        return this._isMuted;
    }

    /**
     * Get current resolution
     * @returns {Object|null} Resolution object { width, height } or null if not connected
     */
    getResolution() {
        if (!this._isConnected) return null;
        return { width: this._canvas.width, height: this._canvas.height };
    }

    /**
     * Register an event handler
     * @param {string} event - Event name ('connected', 'disconnected', 'error')
     * @param {Function} handler - Callback function
     */
    on(event, handler) {
        if (!this._eventHandlers[event]) {
            this._eventHandlers[event] = [];
        }
        this._eventHandlers[event].push(handler);
    }

    /**
     * Remove an event handler
     * @param {string} event
     * @param {Function} handler
     */
    off(event, handler) {
        if (this._eventHandlers[event]) {
            this._eventHandlers[event] = this._eventHandlers[event]
                .filter(h => h !== handler);
        }
    }

    /**
     * Destroy the client and clean up resources
     */
    destroy() {
        this.disconnect();
        if (this._gfxWorker) {
            this._gfxWorker.terminate();
            this._gfxWorker = null;
        }
        if (this._resizeObserver) {
            this._resizeObserver.disconnect();
        }
        this._shadow.innerHTML = '';
    }

    // --------------------------------------------------
    // PRIVATE: UI
    // --------------------------------------------------

    _emit(event, data) {
        if (this._eventHandlers[event]) {
            this._eventHandlers[event].forEach(h => h(data));
        }
    }

    _showModal() {
        this._el.modal.classList.add('active');
        this._el.inputHost.focus();
    }

    _hideModal() {
        this._el.modal.classList.remove('active');
    }

    _handleModalConnect() {
        const host = this._el.inputHost.value.trim();
        const port = parseInt(this._el.inputPort.value) || 3389;
        const user = this._el.inputUser.value.trim();
        const pass = this._el.inputPass.value;

        if (!host || !user) {
            alert('Please enter host and username');
            return;
        }

        this._hideModal();
        this.connect({ host, port, user, pass });
    }

    _updateStatus(state, text) {
        this._el.statusDot.classList.toggle('connected', state === 'connected');
        this._el.statusText.textContent = text;
    }

    _toggleMute() {
        this.setMuted(!this._isMuted);
    }

    _toggleFullscreen() {
        if (document.fullscreenElement) {
            document.exitFullscreen();
        } else {
            this._container.requestFullscreen();
        }
    }

    // --------------------------------------------------
    // PRIVATE: WEBSOCKET
    // --------------------------------------------------

    _sendMessage(msg) {
        if (this._ws && this._ws.readyState === WebSocket.OPEN) {
            this._ws.send(JSON.stringify(msg));
        }
    }

    _handleMessage(event) {
        if (event.data instanceof ArrayBuffer) {
            const bytes = new Uint8Array(event.data);
            
            // Audio - always handle on main thread
            if (this._matchMagic(bytes, OPUS_MAGIC)) {
                this._handleOpusFrame(bytes);
                return;
            }
            if (this._matchMagic(bytes, AUDIO_MAGIC)) {
                this._handleAudioFrame(bytes);
                return;
            }
            
            // When using GFX worker, route ALL non-audio frames to worker
            // (canvas is transferred, so main thread cannot draw)
            if (this._gfxWorkerReady && this._sendToGfxWorker(event.data)) {
                return;
            }
            
            // If worker exists but not ready yet, queue frames
            // (canvas is transferred, but worker hasn't confirmed ready)
            if (this._gfxWorker && this._useOffscreenCanvas && !this._gfxWorkerReady) {
                this._pendingGfxMessages.push({
                    msg: { type: 'binary', data: event.data },
                    transfer: [event.data]
                });
                return;
            }
            
            // Main thread fallback (when worker not available)
            this._handleFrameUpdate(event.data);
            return;
        }

        try {
            const msg = JSON.parse(event.data);
            switch (msg.type) {
                case 'connected':
                    this._handleConnected(msg);
                    break;
                case 'resize':
                    this._handleServerResize(msg.width, msg.height);
                    break;
                case 'pong':
                    this._handlePong();
                    break;
                case 'error':
                    this._handleError(msg.message);
                    break;
                case 'disconnected':
                    this._handleDisconnect();
                    break;
            }
        } catch (e) {
            console.error('[RDPClient] Parse error:', e);
        }
    }

    /**
     * Check if message is a new GFX event stream format
     */
    _isGfxEventMessage(bytes) {
        if (bytes.length < 4) return false;
        
        return this._matchMagic(bytes, SURF_MAGIC) ||
               this._matchMagic(bytes, DELS_MAGIC) ||
               this._matchMagic(bytes, STFR_MAGIC) ||
               this._matchMagic(bytes, ENFR_MAGIC) ||
               this._matchMagic(bytes, PROG_MAGIC) ||
               this._matchMagic(bytes, WEBP_MAGIC) ||
               this._matchMagic(bytes, TILE_MAGIC) ||
               this._matchMagic(bytes, SFIL_MAGIC) ||
               this._matchMagic(bytes, S2SF_MAGIC) ||
               this._matchMagic(bytes, C2SF_MAGIC);
    }

    _matchMagic(bytes, magic) {
        return bytes.length > 12 &&
            bytes[0] === magic[0] && bytes[1] === magic[1] &&
            bytes[2] === magic[2] && bytes[3] === magic[3];
    }

    _handleConnected(msg) {
        this._isConnected = true;
        this._updateStatus('connected', 'Connected');
        this._el.canvas.style.display = 'block';
        this._el.loading.style.display = 'none';
        this._el.btnConnect.disabled = true;
        this._el.btnDisconnect.disabled = false;
        this._el.btnMute.disabled = false;
        this._canvas.focus();
        
        this._initAudio();

        if (msg.width && msg.height) {
            this._handleServerResize(msg.width, msg.height);
        }
        
        // Initialize GFX worker with canvas
        // Note: We only transfer canvas control on connect, not earlier,
        // because the canvas dimensions need to match the session size
        this._initGfxWorkerCanvas(msg.width || this._canvas.width, 
                                  msg.height || this._canvas.height);

        setInterval(() => this._sendPing(), 5000);
        
        this._emit('connected', { width: msg.width, height: msg.height });
        
        if (this._pendingConnect) {
            this._pendingConnect.resolve();
            this._pendingConnect = null;
        }
    }

    _handleDisconnect() {
        this._isConnected = false;
        this._ws = null;
        this._lastRequestedWidth = 0;
        this._lastRequestedHeight = 0;
        this._updateStatus('disconnected', 'Disconnected');
        this._el.canvas.style.display = 'none';
        this._el.loading.style.display = 'block';
        this._el.loading.querySelector('p').textContent = 'Click Connect to start';
        this._el.btnConnect.disabled = false;
        this._el.btnDisconnect.disabled = true;
        this._el.btnMute.disabled = true;
        
        this._cleanupAudio();
        this._cleanupGfxWorker();
        
        this._emit('disconnected');
    }
    
    /**
     * Clean up GFX worker
     */
    _cleanupGfxWorker() {
        if (this._gfxWorker) {
            this._gfxWorker.terminate();
            this._gfxWorker = null;
        }
        this._gfxWorkerReady = false;
        this._pendingGfxMessages = [];
        
        // Re-acquire canvas context for next connection
        // (transferControlToOffscreen is one-way, so we need to recreate canvas)
        if (this._useOffscreenCanvas && this._el.canvas) {
            const parent = this._el.canvas.parentElement;
            const oldCanvas = this._el.canvas;
            const newCanvas = document.createElement('canvas');
            newCanvas.className = oldCanvas.className;
            newCanvas.width = oldCanvas.width;
            newCanvas.height = oldCanvas.height;
            newCanvas.style.display = oldCanvas.style.display;
            newCanvas.setAttribute('tabindex', '0');
            parent.replaceChild(newCanvas, oldCanvas);
            this._el.canvas = newCanvas;
            this._canvas = newCanvas;
            // Note: Do NOT acquire 2D context here - _initGfxWorker will handle it
            // or _initGfxWorkerCanvas will acquire context if transfer fails
            
            // Re-attach event listeners to new canvas
            this._canvas.addEventListener('click', () => this._canvas.focus());
            this._canvas.addEventListener('mousemove', (e) => this._handleMouseMove(e));
            this._canvas.addEventListener('mousedown', (e) => this._handleMouseDown(e));
            this._canvas.addEventListener('mouseup', (e) => this._handleMouseUp(e));
            this._canvas.addEventListener('wheel', (e) => this._handleMouseWheel(e));
            this._canvas.addEventListener('contextmenu', (e) => e.preventDefault());
            
            // Re-initialize worker for next connection
            this._initGfxWorker();
        }
    }

    _handleError(message) {
        console.error('[RDPClient] Error:', message);
        this._updateStatus('error', 'Error');
        this._emit('error', { message });
    }

    _sendPing() {
        if (this._isConnected) {
            this._pingStart = performance.now();
            this._sendMessage({ type: 'ping' });
        }
    }

    _handlePong() {
        const latency = Math.round(performance.now() - this._pingStart);
        this._el.latency.textContent = `Latency: ${latency}ms`;
    }

    // --------------------------------------------------
    // PRIVATE: VIDEO FRAMES
    // --------------------------------------------------

    /**
     * Handle frame update on main thread (fallback - should not be called)
     * All rendering is handled by the GFX worker via OffscreenCanvas
     */
    _handleFrameUpdate(data) {
        // This should not be reached - all frames go to GFX worker
        console.warn('[RDPClient] Unexpected frame on main thread - GFX worker should handle all frames');
    }

    // --------------------------------------------------
    // PRIVATE: AUDIO
    // --------------------------------------------------

    _initAudio() {
        if (this._audioContext) return;
        
        try {
            this._audioContext = new (window.AudioContext || window.webkitAudioContext)();
            this._audioGainNode = this._audioContext.createGain();
            this._audioGainNode.connect(this._audioContext.destination);
            this._audioGainNode.gain.value = this._isMuted ? 0 : 1;
        } catch (e) {
            console.error('[RDPClient] Audio init failed:', e);
        }
    }

    async _handleOpusFrame(bytes) {
        if (!this._audioContext) this._initAudio();
        if (this._isMuted) return;
        
        try {
            if (this._audioContext?.state === 'suspended') {
                this._audioContext.resume();
            }
            
            const view = new DataView(bytes.buffer, bytes.byteOffset);
            const sampleRate = view.getUint32(4, true);
            const channels = view.getUint16(8, true);
            const frameSize = view.getUint16(10, true);
            
            if (frameSize === 0 || frameSize > 4000) return;
            
            if (!this._opusDecoder || this._opusSampleRate !== sampleRate || 
                this._opusChannels !== channels) {
                await this._initOpusDecoder(sampleRate, channels);
            }
            
            if (!this._opusDecoder || this._opusDecoder.state === 'closed') return;
            
            const opusData = bytes.slice(12, 12 + frameSize);
            if (opusData.length === 0) return;
            
            this._opusDecoder.decode(new EncodedAudioChunk({
                type: 'key',
                timestamp: performance.now() * 1000,
                data: opusData
            }));
        } catch (e) {
            console.error('[RDPClient] Opus error:', e);
        }
    }

    async _initOpusDecoder(sampleRate, channels) {
        if (typeof AudioDecoder === 'undefined') return false;
        
        if (this._opusDecoder) {
            try { this._opusDecoder.close(); } catch (e) {}
        }
        
        try {
            this._opusSampleRate = sampleRate;
            this._opusChannels = channels;
            
            this._opusDecoder = new AudioDecoder({
                output: (audioData) => this._handleDecodedAudio(audioData),
                error: (e) => console.error('[RDPClient] Opus decode error:', e)
            });
            
            await this._opusDecoder.configure({
                codec: 'opus',
                sampleRate,
                numberOfChannels: channels,
            });
            
            this._opusInitialized = true;
            return true;
        } catch (e) {
            this._opusDecoder = null;
            return false;
        }
    }

    _handleDecodedAudio(audioData) {
        if (!this._audioContext || this._isMuted) {
            audioData.close();
            return;
        }
        
        try {
            if (this._audioContext.state === 'suspended') {
                this._audioContext.resume();
            }
            
            const numFrames = audioData.numberOfFrames;
            const numChannels = audioData.numberOfChannels;
            const sampleRate = audioData.sampleRate;
            const format = audioData.format; // e.g., 'f32-planar', 'f32', 's16', etc.
            
            const audioBuffer = this._audioContext.createBuffer(numChannels, numFrames, sampleRate);
            
            // Handle based on format
            if (format && format.includes('planar')) {
                // Planar format - copy each channel separately
                for (let ch = 0; ch < numChannels; ch++) {
                    const channelData = audioBuffer.getChannelData(ch);
                    audioData.copyTo(channelData, { planeIndex: ch });
                }
            } else {
                // Interleaved format - need to allocate proper buffer and de-interleave
                const bytesPerSample = format === 's16' ? 2 : format === 's32' ? 4 : 4;
                const totalBytes = numFrames * numChannels * bytesPerSample;
                
                let tempBuffer;
                if (format === 's16') {
                    tempBuffer = new Int16Array(numFrames * numChannels);
                } else if (format === 's32') {
                    tempBuffer = new Int32Array(numFrames * numChannels);
                } else {
                    // f32 or default
                    tempBuffer = new Float32Array(numFrames * numChannels);
                }
                
                audioData.copyTo(tempBuffer, { planeIndex: 0 });
                
                // De-interleave into channels
                for (let ch = 0; ch < numChannels; ch++) {
                    const channelData = audioBuffer.getChannelData(ch);
                    for (let i = 0; i < numFrames; i++) {
                        let sample = tempBuffer[i * numChannels + ch];
                        // Normalize to float [-1, 1]
                        if (format === 's16') {
                            sample = sample / 32768.0;
                        } else if (format === 's32') {
                            sample = sample / 2147483648.0;
                        }
                        channelData[i] = sample;
                    }
                }
            }
            
            this._queueAudioBuffer(audioBuffer);
        } catch (e) {
            console.error('[RDPClient] Audio error:', e);
        } finally {
            audioData.close();
        }
    }

    _handleAudioFrame(bytes) {
        if (!this._audioContext || this._isMuted) return;
        
        try {
            if (this._audioContext.state === 'suspended') {
                this._audioContext.resume();
            }
            
            const view = new DataView(bytes.buffer, bytes.byteOffset);
            const sampleRate = view.getUint32(4, true);
            const channels = view.getUint16(8, true);
            const bits = view.getUint16(10, true);
            
            const pcmData = bytes.slice(12);
            if (pcmData.length === 0) return;
            
            const bytesPerSample = bits / 8;
            const numSamples = Math.floor(pcmData.length / (channels * bytesPerSample));
            if (numSamples === 0) return;
            
            const audioBuffer = this._audioContext.createBuffer(channels, numSamples, sampleRate);
            
            for (let ch = 0; ch < channels; ch++) {
                const channelData = audioBuffer.getChannelData(ch);
                for (let i = 0; i < numSamples; i++) {
                    const sampleIndex = i * channels + ch;
                    const byteOffset = sampleIndex * bytesPerSample;
                    
                    if (bits === 16) {
                        const int16 = pcmData[byteOffset] | (pcmData[byteOffset + 1] << 8);
                        channelData[i] = int16 > 32767 ? (int16 - 65536) / 32768 : int16 / 32768;
                    } else if (bits === 8) {
                        channelData[i] = (pcmData[byteOffset] - 128) / 128;
                    }
                }
            }
            
            this._queueAudioBuffer(audioBuffer);
        } catch (e) {
            console.error('[RDPClient] PCM audio error:', e);
        }
    }

    _queueAudioBuffer(audioBuffer) {
        const source = this._audioContext.createBufferSource();
        source.buffer = audioBuffer;
        source.connect(this._audioGainNode);
        
        const currentTime = this._audioContext.currentTime;
        const bufferDuration = audioBuffer.duration;
        
        this._audioQueue = this._audioQueue.filter(item => item.endTime > currentTime);
        
        let startTime;
        if (this._audioNextPlayTime <= currentTime) {
            startTime = currentTime + 0.1;
        } else {
            startTime = this._audioNextPlayTime;
        }
        
        const maxLatency = 0.5;
        if (startTime > currentTime + maxLatency) {
            startTime = currentTime + 0.1;
        }
        
        source.start(startTime);
        this._audioNextPlayTime = startTime + bufferDuration;
        this._audioQueue.push({ endTime: this._audioNextPlayTime, source });
        
        while (this._audioQueue.length > 50) {
            this._audioQueue.shift();
        }
    }

    _cleanupAudio() {
        this._audioQueue = [];
        this._audioNextPlayTime = 0;
        
        if (this._opusDecoder) {
            try { this._opusDecoder.close(); } catch (e) {}
            this._opusDecoder = null;
            this._opusInitialized = false;
        }
        
        if (this._audioContext) {
            this._audioContext.close();
            this._audioContext = null;
            this._audioGainNode = null;
        }
    }

    // --------------------------------------------------
    // PRIVATE: INPUT HANDLING
    // --------------------------------------------------

    _getMousePos(e) {
        const rect = this._canvas.getBoundingClientRect();
        const scaleX = this._canvas.width / rect.width;
        const scaleY = this._canvas.height / rect.height;
        return {
            x: Math.round((e.clientX - rect.left) * scaleX),
            y: Math.round((e.clientY - rect.top) * scaleY)
        };
    }

    _handleMouseMove(e) {
        if (!this._isConnected) return;
        
        const now = Date.now();
        if (now - this._lastMouseSend < this.options.mouseThrottleMs) return;
        this._lastMouseSend = now;
        
        const pos = this._getMousePos(e);
        this._sendMessage({ type: 'mouse', action: 'move', x: pos.x, y: pos.y });
    }

    _handleMouseDown(e) {
        if (!this._isConnected) return;
        e.preventDefault();
        this._canvas.focus();
        
        const pos = this._getMousePos(e);
        this._sendMessage({ type: 'mouse', action: 'down', button: e.button, x: pos.x, y: pos.y });
    }

    _handleMouseUp(e) {
        if (!this._isConnected) return;
        e.preventDefault();
        
        const pos = this._getMousePos(e);
        this._sendMessage({ type: 'mouse', action: 'up', button: e.button, x: pos.x, y: pos.y });
    }

    _handleMouseWheel(e) {
        if (!this._isConnected) return;
        e.preventDefault();
        
        const pos = this._getMousePos(e);
        this._sendMessage({ 
            type: 'mouse', action: 'wheel', 
            deltaX: e.deltaX, deltaY: e.deltaY, 
            x: pos.x, y: pos.y 
        });
    }

    _handleKeyDown(e) {
        if (!this._isConnected) return;
        if (this._shadow.activeElement !== this._canvas) return;
        
        e.preventDefault();
        this._sendMessage({
            type: 'key', action: 'down',
            key: e.key, code: e.code, keyCode: e.keyCode,
            ctrlKey: e.ctrlKey, shiftKey: e.shiftKey,
            altKey: e.altKey, metaKey: e.metaKey
        });
    }

    _handleKeyUp(e) {
        if (!this._isConnected) return;
        if (this._shadow.activeElement !== this._canvas) return;
        
        e.preventDefault();
        this._sendMessage({
            type: 'key', action: 'up',
            key: e.key, code: e.code, keyCode: e.keyCode,
            ctrlKey: e.ctrlKey, shiftKey: e.shiftKey,
            altKey: e.altKey, metaKey: e.metaKey
        });
    }

    // --------------------------------------------------
    // PRIVATE: RESIZE
    // --------------------------------------------------

    _getAvailableDimensions() {
        const rect = this._el.screen.getBoundingClientRect();
        let width = Math.floor(rect.width - 4);
        let height = Math.floor(rect.height - 4);
        
        width = Math.max(640, Math.min(width, 4096));
        height = Math.max(480, Math.min(height, 2160));
        width = Math.floor(width / 2) * 2;
        height = Math.floor(height / 2) * 2;
        
        return { width, height };
    }

    _handleResize() {
        if (!this._isConnected) return;
        
        if (this._resizeTimeout) {
            clearTimeout(this._resizeTimeout);
        }
        
        this._resizeTimeout = setTimeout(() => {
            if (!this._isConnected) return;
            
            const { width, height } = this._getAvailableDimensions();
            
            if ((width !== this._canvas.width || height !== this._canvas.height) &&
                (width !== this._lastRequestedWidth || height !== this._lastRequestedHeight)) {
                this._lastRequestedWidth = width;
                this._lastRequestedHeight = height;
                this._sendMessage({ type: 'resize', width, height });
            }
        }, this.options.resizeDebounceMs);
    }

    _handleServerResize(width, height) {
        if (this._canvas.width === width && this._canvas.height === height) return;
        
        this._canvas.width = width;
        this._canvas.height = height;
        this._el.resolution.textContent = `Resolution: ${width}x${height}`;
        
        // Notify GFX worker of resize
        if (this._gfxWorker && this._gfxWorkerReady) {
            this._gfxWorker.postMessage({
                type: 'resize',
                data: { width, height }
            });
        }
        
        this._emit('resize', { width, height });
    }
}

// Default export for convenience
export default RDPClient;
