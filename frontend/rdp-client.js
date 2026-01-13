/**
 * RDP Web Client - Reusable ES Module
 * Shadow DOM isolated component for browser-based RDP via WebSocket
 * 
 * @example Basic usage
 * import { RDPClient } from './rdp-client.js';
 * 
 * const client = new RDPClient(document.getElementById('container'), {
 *   wsUrl: 'ws://localhost:8765',
 *   showTopBar: true,
 *   showBottomBar: true,
 *   keepConnectionModalOpen: false
 * });
 * 
 * await client.connect({ host: '192.168.1.100', user: 'admin', pass: 'secret' });
 * 
 * @example With theme customization
 * import { RDPClient } from './rdp-client.js';
 * 
 * const client = new RDPClient(container, {
 *   theme: {
 *     preset: 'dark',
 *     colors: {
 *       accent: '#00b4d8',
 *       buttonHover: '#0096c7'
 *     }
 *   }
 * });
 */

import { resolveTheme, themeToCssVars, sanitizeTheme, fontsToCss, themes } from './rdp-themes.js';
import { Magic, matchMagic } from './wire-format.js';

// ============================================================
// STYLES - Shadow DOM isolated styles (uses CSS custom properties for theming)
// ============================================================
const STYLES = `
:host {
    display: block;
    width: 100%;
    height: 100%;
    
    /* Typography */
    --rdp-font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    --rdp-font-size: 14px;
    --rdp-font-size-small: 0.85rem;
    
    /* Colors - defaults (overridden by theme) */
    --rdp-bg: #1a1a2e;
    --rdp-surface: #16213e;
    --rdp-border: #0f3460;
    --rdp-text: #eeeeee;
    --rdp-text-muted: #888888;
    --rdp-accent: #51cf66;
    --rdp-accent-text: #000000;
    --rdp-error: #ff6b6b;
    --rdp-success: #51cf66;
    --rdp-btn-bg: #0f3460;
    --rdp-btn-hover: #1a4a7a;
    --rdp-btn-text: #eeeeee;
    --rdp-btn-active-bg: #51cf66;
    --rdp-btn-active-text: #000000;
    --rdp-input-bg: #1a1a2e;
    --rdp-input-border: #0f3460;
    --rdp-input-focus-border: #51cf66;
    
    /* Shape */
    --rdp-border-radius: 4px;
    --rdp-border-radius-lg: 8px;
    
    font-family: var(--rdp-font-family);
    font-size: var(--rdp-font-size);
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
    background: var(--rdp-surface);
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
    font-size: var(--rdp-font-size-small);
}

.rdp-status-dot {
    width: 10px;
    height: 10px;
    border-radius: 50%;
    background: var(--rdp-error);
    transition: background 0.3s;
}

.rdp-status-dot.connected { background: var(--rdp-success); }

.rdp-controls {
    margin-left: auto;
    display: flex;
    gap: 8px;
}

.rdp-btn {
    background: var(--rdp-btn-bg);
    border: none;
    color: var(--rdp-btn-text);
    padding: 6px 14px;
    border-radius: var(--rdp-border-radius);
    cursor: pointer;
    font-size: var(--rdp-font-size-small);
    font-family: inherit;
    transition: background 0.2s, color 0.2s;
}

.rdp-btn:hover { background: var(--rdp-btn-hover); }
.rdp-btn:active { background: var(--rdp-btn-active-bg); color: var(--rdp-btn-active-text); }
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
    border-radius: var(--rdp-border-radius);
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
    background: var(--rdp-surface);
    padding: 6px 16px;
    font-size: var(--rdp-font-size-small);
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
    z-index: 10;
}

.rdp-modal.active { display: flex; }

.rdp-modal-content {
    background: var(--rdp-surface);
    padding: 24px;
    border-radius: var(--rdp-border-radius-lg);
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
    border: 1px solid var(--rdp-input-border);
    border-radius: var(--rdp-border-radius);
    background: var(--rdp-input-bg);
    color: var(--rdp-text);
    font-size: var(--rdp-font-size-small);
    font-family: inherit;
}

.rdp-form-group input:focus {
    outline: none;
    border-color: var(--rdp-input-focus-border);
}

.rdp-form-checkbox label {
    display: flex;
    align-items: center;
    gap: 8px;
    cursor: pointer;
}

.rdp-form-checkbox input[type="checkbox"] {
    width: auto;
    cursor: pointer;
}

.rdp-modal-buttons {
    display: flex;
    gap: 8px;
    margin-top: 16px;
}

.rdp-modal-buttons .rdp-btn { flex: 1; }
.rdp-modal-buttons .rdp-btn-primary { background: var(--rdp-accent); color: var(--rdp-accent-text); }
.rdp-modal-buttons .rdp-btn-primary:hover { opacity: 0.9; }

/* Virtual Keyboard Overlay */
.rdp-keyboard-overlay {
    display: none;
    position: absolute;
    background: color-mix(in srgb, var(--rdp-surface) 95%, transparent);
    border: 2px solid var(--rdp-border);
    border-radius: var(--rdp-border-radius-lg);
    padding: 8px;
    z-index: 100;
    user-select: none;
    box-shadow: 0 4px 20px rgba(0, 0, 0, 0.5);
    overflow: hidden;
}

.rdp-keyboard-overlay.visible { display: block; }

.rdp-keyboard-titlebar {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 4px 8px;
    margin-bottom: 8px;
    background: var(--rdp-surface);
    border-radius: var(--rdp-border-radius);
    cursor: move;
}

.rdp-keyboard-title {
    font-size: 0.8rem;
    color: var(--rdp-text-muted);
}

.rdp-keyboard-close {
    background: transparent;
    border: none;
    color: var(--rdp-text-muted);
    font-size: 1.2rem;
    cursor: pointer;
    padding: 2px 6px;
    line-height: 1;
}

.rdp-keyboard-close:hover { color: var(--rdp-error); }

.rdp-keyboard-content {
    transform-origin: top left;
    width: max-content;
}

.rdp-keyboard-rows {
    display: flex;
    flex-direction: column;
    gap: 4px;
}

.rdp-keyboard-row {
    display: flex;
    gap: 4px;
    justify-content: center;
}

.rdp-key {
    background: var(--rdp-btn-bg);
    border: 1px solid var(--rdp-border);
    color: var(--rdp-btn-text);
    padding: 8px 12px;
    min-width: 36px;
    height: 36px;
    border-radius: var(--rdp-border-radius);
    cursor: pointer;
    font-size: 0.75rem;
    font-family: inherit;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: background 0.1s, transform 0.05s, color 0.1s;
}

.rdp-key:hover { background: var(--rdp-btn-hover); }
.rdp-key:active { transform: scale(0.95); background: var(--rdp-btn-active-bg); color: var(--rdp-btn-active-text); }
.rdp-key.pressed { background: var(--rdp-btn-active-bg); color: var(--rdp-btn-active-text); }

.rdp-key-wide { min-width: 60px; }
.rdp-key-wider { min-width: 80px; }
.rdp-key-widest { min-width: 100px; }
.rdp-key-space { min-width: 200px; }
.rdp-key-special { background: color-mix(in srgb, var(--rdp-btn-bg) 80%, var(--rdp-accent) 20%); }
.rdp-key-special:hover { background: color-mix(in srgb, var(--rdp-btn-hover) 80%, var(--rdp-accent) 20%); }

.rdp-keyboard-resize {
    position: absolute;
    bottom: 4px;
    right: 4px;
    width: 16px;
    height: 16px;
    cursor: nwse-resize;
    opacity: 0.5;
}

.rdp-keyboard-resize:hover { opacity: 1; }

.rdp-keyboard-resize::before,
.rdp-keyboard-resize::after {
    content: '';
    position: absolute;
    background: var(--rdp-text-muted);
    border-radius: 1px;
}

.rdp-keyboard-resize::before {
    width: 12px;
    height: 2px;
    bottom: 2px;
    right: 0;
    transform: rotate(-45deg);
    transform-origin: right bottom;
}

.rdp-keyboard-resize::after {
    width: 8px;
    height: 2px;
    bottom: 6px;
    right: 0;
    transform: rotate(-45deg);
    transform-origin: right bottom;
}
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
            <button class="rdp-btn rdp-btn-keyboard" disabled title="Toggle Virtual Keyboard">⌨️</button>
            <button class="rdp-btn rdp-btn-mute" disabled title="Toggle Audio">🔊</button>
            <button class="rdp-btn rdp-btn-screenshot" disabled title="Take Screenshot">📷</button>
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
            
            <!-- Virtual Keyboard Overlay -->
            <div class="rdp-keyboard-overlay">
                <div class="rdp-keyboard-titlebar">
                    <span class="rdp-keyboard-title">Virtual Keyboard</span>
                    <button class="rdp-keyboard-close" title="Close">×</button>
                </div>
                <div class="rdp-keyboard-content">
                    <div class="rdp-keyboard-rows">
                        <!-- Row 1: Function keys + special -->
                        <div class="rdp-keyboard-row">
                        <button class="rdp-key" data-code="Escape">Esc</button>
                        <button class="rdp-key" data-code="F1">F1</button>
                        <button class="rdp-key" data-code="F2">F2</button>
                        <button class="rdp-key" data-code="F3">F3</button>
                        <button class="rdp-key" data-code="F4">F4</button>
                        <button class="rdp-key" data-code="F5">F5</button>
                        <button class="rdp-key" data-code="F6">F6</button>
                        <button class="rdp-key" data-code="F7">F7</button>
                        <button class="rdp-key" data-code="F8">F8</button>
                        <button class="rdp-key" data-code="F9">F9</button>
                        <button class="rdp-key" data-code="F10">F10</button>
                        <button class="rdp-key" data-code="F11">F11</button>
                        <button class="rdp-key" data-code="F12">F12</button>
                        <button class="rdp-key rdp-key-wide" data-code="Delete">Del</button>
                    </div>
                    <!-- Row 2: Numbers -->
                    <div class="rdp-keyboard-row">
                        <button class="rdp-key" data-code="Backquote" data-key="\`" data-shift="~">\`</button>
                        <button class="rdp-key" data-code="Digit1" data-key="1" data-shift="!" data-altgr="¡" data-shift-altgr="¹">1</button>
                        <button class="rdp-key" data-code="Digit2" data-key="2" data-shift="@" data-altgr="²">2</button>
                        <button class="rdp-key" data-code="Digit3" data-key="3" data-shift="#" data-altgr="³">3</button>
                        <button class="rdp-key" data-code="Digit4" data-key="4" data-shift="$" data-altgr="¤" data-shift-altgr="£">4</button>
                        <button class="rdp-key" data-code="Digit5" data-key="5" data-shift="%" data-altgr="€">5</button>
                        <button class="rdp-key" data-code="Digit6" data-key="6" data-shift="^" data-altgr="¼">6</button>
                        <button class="rdp-key" data-code="Digit7" data-key="7" data-shift="&" data-altgr="½">7</button>
                        <button class="rdp-key" data-code="Digit8" data-key="8" data-shift="*" data-altgr="¾">8</button>
                        <button class="rdp-key" data-code="Digit9" data-key="9" data-shift="(" data-altgr="'">9</button>
                        <button class="rdp-key" data-code="Digit0" data-key="0" data-shift=")" data-altgr="'">0</button>
                        <button class="rdp-key" data-code="Minus" data-key="-" data-shift="_" data-altgr="¥">-</button>
                        <button class="rdp-key" data-code="Equal" data-key="=" data-shift="+" data-altgr="×" data-shift-altgr="÷">=</button>
                        <button class="rdp-key rdp-key-wider" data-code="Backspace">⌫</button>
                    </div>
                    <!-- Row 3: QWERTY top -->
                    <div class="rdp-keyboard-row">
                        <button class="rdp-key rdp-key-wide" data-code="Tab">Tab</button>
                        <button class="rdp-key" data-code="KeyQ" data-key="q" data-shift="Q">Q</button>
                        <button class="rdp-key" data-code="KeyW" data-key="w" data-shift="W">W</button>
                        <button class="rdp-key" data-code="KeyE" data-key="e" data-shift="E">E</button>
                        <button class="rdp-key" data-code="KeyR" data-key="r" data-shift="R">R</button>
                        <button class="rdp-key" data-code="KeyT" data-key="t" data-shift="T">T</button>
                        <button class="rdp-key" data-code="KeyY" data-key="y" data-shift="Y">Y</button>
                        <button class="rdp-key" data-code="KeyU" data-key="u" data-shift="U">U</button>
                        <button class="rdp-key" data-code="KeyI" data-key="i" data-shift="I">I</button>
                        <button class="rdp-key" data-code="KeyO" data-key="o" data-shift="O">O</button>
                        <button class="rdp-key" data-code="KeyP" data-key="p" data-shift="P">P</button>
                        <button class="rdp-key" data-code="BracketLeft" data-key="[" data-shift="{">[</button>
                        <button class="rdp-key" data-code="BracketRight" data-key="]" data-shift="}">]</button>
                        <button class="rdp-key rdp-key-wide" data-code="Backslash" data-key="\\" data-shift="|">\\</button>
                    </div>
                    <!-- Row 4: ASDF middle -->
                    <div class="rdp-keyboard-row">
                        <button class="rdp-key rdp-key-wider" data-code="CapsLock">Caps</button>
                        <button class="rdp-key" data-code="KeyA" data-key="a" data-shift="A" data-altgr="á" data-shift-altgr="Á">A</button>
                        <button class="rdp-key" data-code="KeyS" data-key="s" data-shift="S" data-altgr="ß" data-shift-altgr="§">S</button>
                        <button class="rdp-key" data-code="KeyD" data-key="d" data-shift="D" data-altgr="ð" data-shift-altgr="Ð">D</button>
                        <button class="rdp-key" data-code="KeyF" data-key="f" data-shift="F">F</button>
                        <button class="rdp-key" data-code="KeyG" data-key="g" data-shift="G">G</button>
                        <button class="rdp-key" data-code="KeyH" data-key="h" data-shift="H">H</button>
                        <button class="rdp-key" data-code="KeyJ" data-key="j" data-shift="J">J</button>
                        <button class="rdp-key" data-code="KeyK" data-key="k" data-shift="K">K</button>
                        <button class="rdp-key" data-code="KeyL" data-key="l" data-shift="L" data-altgr="ø" data-shift-altgr="Ø">L</button>
                        <button class="rdp-key" data-code="Semicolon" data-key=";" data-shift=":" data-altgr="¶" data-shift-altgr="°">;</button>
                        <button class="rdp-key" data-code="Quote" data-key="'" data-shift='"' data-altgr="´" data-shift-altgr="¨">'</button>
                        <button class="rdp-key rdp-key-widest" data-code="Enter">Enter</button>
                    </div>
                    <!-- Row 5: ZXCV bottom -->
                    <div class="rdp-keyboard-row">
                        <button class="rdp-key rdp-key-widest rdp-key-modifier" data-code="ShiftLeft" data-modifier="shift">Shift</button>
                        <button class="rdp-key" data-code="KeyZ" data-key="z" data-shift="Z" data-altgr="æ" data-shift-altgr="Æ">Z</button>
                        <button class="rdp-key" data-code="KeyX" data-key="x" data-shift="X">X</button>
                        <button class="rdp-key" data-code="KeyC" data-key="c" data-shift="C" data-altgr="©" data-shift-altgr="¢">C</button>
                        <button class="rdp-key" data-code="KeyV" data-key="v" data-shift="V">V</button>
                        <button class="rdp-key" data-code="KeyB" data-key="b" data-shift="B">B</button>
                        <button class="rdp-key" data-code="KeyN" data-key="n" data-shift="N" data-altgr="ñ" data-shift-altgr="Ñ">N</button>
                        <button class="rdp-key" data-code="KeyM" data-key="m" data-shift="M" data-altgr="µ">M</button>
                        <button class="rdp-key" data-code="Comma" data-key="," data-shift="<" data-altgr="ç" data-shift-altgr="Ç">,</button>
                        <button class="rdp-key" data-code="Period" data-key="." data-shift=">">.</button>
                        <button class="rdp-key" data-code="Slash" data-key="/" data-shift="?" data-altgr="¿">?</button>
                        <button class="rdp-key rdp-key-widest rdp-key-modifier" data-code="ShiftRight" data-modifier="shift">Shift</button>
                    </div>
                    <!-- Row 6: Bottom row -->
                    <div class="rdp-keyboard-row">
                        <button class="rdp-key rdp-key-wide rdp-key-modifier" data-code="ControlLeft" data-modifier="ctrl">Ctrl</button>
                        <button class="rdp-key rdp-key-wide rdp-key-modifier" data-code="MetaLeft" data-modifier="meta">Win</button>
                        <button class="rdp-key rdp-key-wide rdp-key-modifier" data-code="AltLeft" data-modifier="alt">Alt</button>
                        <button class="rdp-key rdp-key-space" data-code="Space" data-key=" ">Space</button>
                        <button class="rdp-key rdp-key-wide rdp-key-modifier" data-code="AltRight" data-modifier="altgr">AltGr</button>
                        <button class="rdp-key rdp-key-wide rdp-key-modifier" data-code="ControlRight" data-modifier="ctrl">Ctrl</button>
                        <button class="rdp-key" data-code="ArrowLeft">←</button>
                        <button class="rdp-key" data-code="ArrowUp">↑</button>
                        <button class="rdp-key" data-code="ArrowDown">↓</button>
                        <button class="rdp-key" data-code="ArrowRight">→</button>
                    </div>
                        <!-- Row 7: Special combos -->
                        <div class="rdp-keyboard-row">
                            <button class="rdp-key rdp-key-widest rdp-key-special" data-combo="alt+tab">Alt+Tab</button>
                            <button class="rdp-key rdp-key-widest rdp-key-special" data-combo="ctrl+alt+delete">Ctrl+Alt+Del</button>
                        </div>
                    </div>
                </div>
                <div class="rdp-keyboard-resize"></div>
            </div>
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
     * @param {boolean} [options.keepConnectionModalOpen=false] - Keep connection modal open when disconnected (cannot be closed)
     * @param {import('./rdp-themes.js').RDPTheme} [options.theme] - Theme configuration
     */
    constructor(container, options = {}) {
        this.options = {
            wsUrl: 'ws://localhost:8765',
            showTopBar: true,
            showBottomBar: true,
            reconnectDelay: 3000,
            mouseThrottleMs: 16,
            resizeDebounceMs: 2000,
            keepConnectionModalOpen: false,
            theme: null,
            ...options
        };

        this._container = container;
        this._initShadowDOM();
        this._applyTheme(this.options.theme);
        this._initState();
        this._bindElements();
        this._setupEventListeners();
        
        // Auto-show modal on init when keepConnectionModalOpen is enabled
        if (this.options.keepConnectionModalOpen) {
            this._showModal();
        }
        
        console.log('[RDPClient] Initialized');
    }

    // --------------------------------------------------
    // INITIALIZATION
    // --------------------------------------------------
    
    _initShadowDOM() {
        this._shadow = this._container.attachShadow({ mode: 'open' });
        
        // Main styles
        const style = document.createElement('style');
        style.textContent = STYLES;
        style.id = 'rdp-main-styles';
        
        // Theme overrides (separate style element for easy updates)
        this._themeStyle = document.createElement('style');
        this._themeStyle.id = 'rdp-theme-styles';
        
        const template = document.createElement('template');
        template.innerHTML = TEMPLATE;
        
        this._shadow.appendChild(style);
        this._shadow.appendChild(this._themeStyle);
        this._shadow.appendChild(template.content.cloneNode(true));
        
        // Apply visibility options
        if (!this.options.showTopBar) {
            this._shadow.querySelector('.rdp-topbar').classList.add('hidden');
        }
        if (!this.options.showBottomBar) {
            this._shadow.querySelector('.rdp-bottombar').classList.add('hidden');
        }
    }
    
    /**
     * Apply theme configuration
     * Can be called after initialization to change theme dynamically
     * @param {import('./rdp-themes.js').RDPTheme} themeConfig - Theme configuration
     */
    _applyTheme(themeConfig) {
        if (!themeConfig) return;
        
        // Sanitize and resolve theme
        const sanitized = sanitizeTheme(themeConfig);
        const resolved = resolveTheme(sanitized);
        const cssVars = themeToCssVars(resolved);
        
        // Generate font imports CSS
        const fontCss = fontsToCss(sanitized.fonts);
        
        // Apply as :host styles with optional font imports
        this._themeStyle.textContent = `${fontCss}\n:host { ${cssVars}; }`;
    }
    
    /**
     * Set theme dynamically after initialization
     * @param {import('./rdp-themes.js').RDPTheme} themeConfig - Theme configuration
     * @example
     * client.setTheme({ preset: 'light' });
     * client.setTheme({ colors: { accent: '#ff5722' } });
     */
    setTheme(themeConfig) {
        this.options.theme = themeConfig;
        this._applyTheme(themeConfig);
    }

    _initState() {
        this._ws = null;
        this._isConnected = false;
        this._canvas = null;
        this._ctx = null;
        this._lastMouseSend = 0;
        this._pingStart = 0;
        this._lastLatency = null;
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
        this._wasmAvailable = false;  // Set by GFX worker on load
        
        // Screenshot pending requests
        this._screenshotRequests = new Map();
        this._screenshotRequestId = 0;
        
        // Virtual keyboard state
        this._keyboardVisible = false;
        this._keyboardModifiers = { ctrl: false, alt: false, shift: false, meta: false, altgr: false };
        this._keyboardDragging = false;
        this._keyboardResizing = false;
        this._keyboardDragOffset = { x: 0, y: 0 };
        this._keyboardBaseWidth = 0;    // Natural width of keyboard content
        this._keyboardBaseHeight = 0;   // Natural height of keyboard content (rows only)
        this._keyboardTitlebarHeight = 0; // Height of titlebar (not scaled)
        this._keyboardActiveKey = null;  // Currently pressed key element
        
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
            btnScreenshot: $('.rdp-btn-screenshot'),
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
            // Virtual keyboard elements
            btnKeyboard: $('.rdp-btn-keyboard'),
            keyboardOverlay: $('.rdp-keyboard-overlay'),
            keyboardContent: $('.rdp-keyboard-content'),
            keyboardTitlebar: $('.rdp-keyboard-titlebar'),
            keyboardClose: $('.rdp-keyboard-close'),
            keyboardResize: $('.rdp-keyboard-resize'),
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
                // Worker loaded with WASM pre-initialized
                this._wasmAvailable = !!msg.wasmReady;
                console.log('[RDPClient] GFX Worker loaded, WASM:', this._wasmAvailable);
                if (!this._wasmAvailable) {
                    console.warn('[RDPClient] Progressive WASM not available - will use fallback decoders');
                }
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
                
            case 'unhandled':
                // Unhandled message from worker - process on main thread
                if (msg.data) {
                    this._handleFrameUpdate(msg.data);
                }
                break;
                
            case 'screenshotResult':
                // Resolve pending screenshot request
                const pending = this._screenshotRequests.get(msg.requestId);
                if (pending) {
                    this._screenshotRequests.delete(msg.requestId);
                    if (msg.error) {
                        pending.reject(new Error(msg.error));
                    } else {
                        pending.resolve({
                            blob: msg.blob,
                            width: msg.width,
                            height: msg.height
                        });
                    }
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
        this._el.btnKeyboard.addEventListener('click', () => this._toggleKeyboard());
        this._el.btnScreenshot.addEventListener('click', () => this._handleScreenshotClick());
        this._el.btnFullscreen.addEventListener('click', () => this._toggleFullscreen());
        
        // Loading area - click to open modal if not connected/connecting
        this._el.loading.addEventListener('click', () => {
            if (!this._isConnected && !this._pendingConnect) {
                this._showModal();
            }
        });
        this._el.loading.style.cursor = 'pointer';
        
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
        
        // Virtual keyboard events
        this._setupVirtualKeyboard();

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

            // host, user and pass are required
            if (!credentials.host || !credentials.user || !credentials.pass) {
                reject(new Error('Missing required connection fields: host, user, pass'));
                return;
            }

            // Hide modal if it's open (e.g., when connect() is called via API)
            if (this._el.modal.classList.contains('active')) {
                this._el.modal.classList.remove('active');
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

                // Progressive is enabled when WASM is available
                const progressiveEnabled = this._wasmAvailable;
                
                this._sendMessage({
                    type: 'connect',
                    host: credentials.host,
                    port: credentials.port || 3389,
                    username: credentials.user,
                    password: credentials.pass,
                    width,
                    height,
                    progressiveEnabled
                });
                
                console.log('[RDPClient] Connect request - Progressive:', progressiveEnabled);
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
     * Check if connected to RDP server
     * @returns {boolean} True if connected
     */
    isConnected() {
        return this._isConnected;
    }

    /**
     * Get current latency in milliseconds
     * @returns {number|null} Latency in ms, or null if not yet measured
     */
    getLatency() {
        if (!this._isConnected || this._pingStart === 0) return null;
        return this._lastLatency || null;
    }

    /**
     * Capture a screenshot of the current remote desktop
     * @param {string} [type='png'] - Image type: 'png' or 'jpg'
     * @param {number} [quality=0.9] - JPEG quality (0-1), ignored for PNG
     * @returns {Promise<{blob: Blob, width: number, height: number}>} Screenshot data
     * @throws {Error} If not connected or screenshot fails
     * @example
     * const { blob, width, height } = await client.getScreenshot();
     * const url = URL.createObjectURL(blob);
     */
    getScreenshot(type = 'png', quality = 0.9) {
        return new Promise((resolve, reject) => {
            if (!this._isConnected) {
                reject(new Error('Not connected'));
                return;
            }
            
            if (!this._gfxWorker || !this._gfxWorkerReady) {
                reject(new Error('GFX Worker not ready'));
                return;
            }
            
            const requestId = ++this._screenshotRequestId;
            const mimeType = type === 'jpg' || type === 'jpeg' ? 'image/jpeg' : 'image/png';
            if (typeof quality !== 'number' || quality < 0 || quality > 1) {
                quality = 0.9;
            }
            
            this._screenshotRequests.set(requestId, { resolve, reject });
            
            this._gfxWorker.postMessage({
                type: 'screenshot',
                data: { requestId, mimeType, quality }
            });
            
            // Timeout after 5 seconds
            setTimeout(() => {
                if (this._screenshotRequests.has(requestId)) {
                    this._screenshotRequests.delete(requestId);
                    reject(new Error('Screenshot timeout'));
                }
            }, 5000);
        });
    }

    /**
     * Capture and download a screenshot of the remote desktop
     * @param {string} [type='png'] - Image type: 'png' or 'jpg'
     * @param {number} [quality=0.9] - JPEG quality (0-1), ignored for PNG
     * @returns {Promise<void>} Resolves when download starts
     * @example
     * // Downloads as 'screenshot-2026-01-12--14-30.png'
     * await client.downloadScreenshot();
     * 
     * // Download as JPEG
     * await client.downloadScreenshot('jpg');
     */
    async downloadScreenshot(type = 'png', quality = 0.9) {
        const { blob } = await this.getScreenshot(type, quality);
        
        // Generate filename with timestamp: screenshot-YYYY-mm-dd--hh-mm.ext
        const now = new Date();
        const pad = (n) => String(n).padStart(2, '0');
        const timestamp = `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}--${pad(now.getHours())}-${pad(now.getMinutes())}`;
        const ext = (type === 'jpg' || type === 'jpeg') ? 'jpg' : 'png';
        const filename = `screenshot-${timestamp}.${ext}`;
        
        // Create download link and trigger
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = filename;
        a.style.display = 'none';
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
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
        // Hide cancel button when keepConnectionModalOpen is enabled
        this._el.modalCancel.style.display = this.options.keepConnectionModalOpen ? 'none' : '';
        this._el.inputHost.focus();
    }

    _hideModal() {
        // Don't allow hiding modal if keepConnectionModalOpen is enabled and not connected
        if (this.options.keepConnectionModalOpen && !this._isConnected) {
            return;
        }
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

    async _handleScreenshotClick() {
        try {
            await this.downloadScreenshot();
        } catch (err) {
            console.error('[RDPClient] Screenshot failed:', err);
        }
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
            if (matchMagic(bytes, Magic.OPUS)) {
                this._handleOpusFrame(bytes);
                return;
            }
            if (matchMagic(bytes, Magic.AUDI)) {
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
        
        return matchMagic(bytes, Magic.SURF) ||
               matchMagic(bytes, Magic.DELS) ||
               matchMagic(bytes, Magic.STFR) ||
               matchMagic(bytes, Magic.ENFR) ||
               matchMagic(bytes, Magic.PROG) ||
               matchMagic(bytes, Magic.WEBP) ||
               matchMagic(bytes, Magic.TILE) ||
               matchMagic(bytes, Magic.SFIL) ||
               matchMagic(bytes, Magic.S2SF) ||
               matchMagic(bytes, Magic.C2SF);
    }

    _handleConnected(msg) {
        this._isConnected = true;
        this._updateStatus('connected', 'Connected');
        this._el.canvas.style.display = 'block';
        this._el.loading.style.display = 'none';
        this._el.btnConnect.disabled = true;
        this._el.btnDisconnect.disabled = false;
        this._el.btnKeyboard.disabled = false;
        this._el.btnMute.disabled = false;
        this._el.btnScreenshot.disabled = false;
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
        this._el.btnKeyboard.disabled = true;
        this._el.btnMute.disabled = true;
        this._el.btnScreenshot.disabled = true;
        
        // Hide virtual keyboard on disconnect
        this.hideKeyboard();
        
        this._cleanupAudio();
        this._cleanupGfxWorker();
        
        this._emit('disconnected');
        
        // Auto-show modal when keepConnectionModalOpen is enabled
        if (this.options.keepConnectionModalOpen) {
            this._showModal();
        }
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
        this._lastLatency = latency;
        this._el.latency.textContent = `Latency: ${latency}ms`;
        this._emit('latency', { latencyMs: latency });
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
        console.error('[RDPClient] Unexpected frame on main thread - GFX worker should handle all frames');
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
    // PRIVATE: VIRTUAL KEYBOARD
    // --------------------------------------------------
    
    /**
     * Setup virtual keyboard event listeners
     */
    _setupVirtualKeyboard() {
        const overlay = this._el.keyboardOverlay;
        const titlebar = this._el.keyboardTitlebar;
        const resizeHandle = this._el.keyboardResize;
        
        // Close button
        this._el.keyboardClose.addEventListener('click', () => this.hideKeyboard());
        
        // Key press handlers
        overlay.querySelectorAll('.rdp-key').forEach(key => {
            key.addEventListener('mousedown', (e) => this._handleVirtualKeyDown(e, key));
            key.addEventListener('mouseup', (e) => this._handleVirtualKeyUp(e, key));
            key.addEventListener('mouseleave', (e) => this._handleVirtualKeyUp(e, key));
            // Touch support
            key.addEventListener('touchstart', (e) => {
                e.preventDefault();
                this._handleVirtualKeyDown(e, key);
            });
            key.addEventListener('touchend', (e) => {
                e.preventDefault();
                this._handleVirtualKeyUp(e, key);
            });
        });
        
        // Dragging
        titlebar.addEventListener('mousedown', (e) => this._startKeyboardDrag(e));
        titlebar.addEventListener('touchstart', (e) => {
            e.preventDefault();
            this._startKeyboardDrag(e.touches[0]);
        });
        
        // Resizing
        resizeHandle.addEventListener('mousedown', (e) => this._startKeyboardResize(e));
        resizeHandle.addEventListener('touchstart', (e) => {
            e.preventDefault();
            this._startKeyboardResize(e.touches[0]);
        });
        
        // Global mouse/touch move and up handlers
        this._shadow.addEventListener('mousemove', (e) => {
            if (this._keyboardDragging) this._handleKeyboardDrag(e);
            if (this._keyboardResizing) this._handleKeyboardResize(e);
        });
        this._shadow.addEventListener('mouseup', () => {
            this._keyboardDragging = false;
            this._keyboardResizing = false;
        });
        this._shadow.addEventListener('touchmove', (e) => {
            if (this._keyboardDragging || this._keyboardResizing) {
                e.preventDefault();
                const touch = e.touches[0];
                if (this._keyboardDragging) this._handleKeyboardDrag(touch);
                if (this._keyboardResizing) this._handleKeyboardResize(touch);
            }
        });
        this._shadow.addEventListener('touchend', () => {
            this._keyboardDragging = false;
            this._keyboardResizing = false;
        });
    }
    
    /**
     * Toggle virtual keyboard visibility
     */
    _toggleKeyboard() {
        if (this._keyboardVisible) {
            this.hideKeyboard();
        } else {
            this.showKeyboard();
        }
    }
    
    /**
     * Show the virtual keyboard
     * @public
     */
    showKeyboard() {
        if (!this._isConnected) return;
        
        const overlay = this._el.keyboardOverlay;
        const content = this._el.keyboardContent;
        const screenRect = this._el.screen.getBoundingClientRect();
        
        // Reset scale to measure natural size
        content.style.transform = 'scale(1)';
        overlay.classList.add('visible');
        
        // Measure natural keyboard size (only once)
        if (this._keyboardBaseWidth === 0) {
            const contentRect = content.getBoundingClientRect();
            const titlebarRect = this._el.keyboardTitlebar.getBoundingClientRect();
            this._keyboardBaseWidth = contentRect.width;
            this._keyboardBaseHeight = contentRect.height;  // Content only (rows)
            this._keyboardTitlebarHeight = titlebarRect.height + 8; // +8 for margin
        }
        
        // Scale limits - adjusted for better usability
        const minScale = 0.5;
        const maxScale = 1.2;
        
        // Calculate initial scale (try to fit 80% of screen width, but clamp to limits)
        let scale = Math.min((screenRect.width * 0.8 - 16) / this._keyboardBaseWidth, maxScale);
        scale = Math.max(scale, minScale);
        
        // Calculate dimensions: width and content are scaled, titlebar is not
        const calcWidth = (s) => (this._keyboardBaseWidth * s) + 16;
        const calcHeight = (s) => (this._keyboardBaseHeight * s) + this._keyboardTitlebarHeight + 16;
        
        // Check if max scale still fits in canvas
        const maxWidth = screenRect.width - 20;  // Leave some margin
        const maxHeight = screenRect.height - 40;  // Leave some margin
        
        if (calcWidth(scale) > maxWidth) {
            scale = Math.max(minScale, (maxWidth - 16) / this._keyboardBaseWidth);
        }
        if (calcHeight(scale) > maxHeight) {
            const heightScale = (maxHeight - this._keyboardTitlebarHeight - 16) / this._keyboardBaseHeight;
            scale = Math.max(minScale, Math.min(scale, heightScale));
        }
        
        const width = calcWidth(scale);
        const height = calcHeight(scale);
        
        // Apply scale
        content.style.transform = `scale(${scale})`;
        
        // Center horizontally, position at bottom
        const left = (screenRect.width - width) / 2;
        const top = Math.max(0, screenRect.height - height - 20);
        
        overlay.style.width = `${width}px`;
        overlay.style.height = `${height}px`;
        overlay.style.left = `${Math.max(0, left)}px`;
        overlay.style.top = `${top}px`;
        
        this._keyboardVisible = true;
        this._emit('keyboardShow');
    }
    
    /**
     * Hide the virtual keyboard
     * @public
     */
    hideKeyboard() {
        this._el.keyboardOverlay.classList.remove('visible');
        this._keyboardVisible = false;
        this._resetKeyboardModifiers();
        this._emit('keyboardHide');
    }
    
    /**
     * Check if virtual keyboard is visible
     * @public
     * @returns {boolean}
     */
    isKeyboardVisible() {
        return this._keyboardVisible;
    }
    
    /**
     * Reset all keyboard modifiers
     */
    _resetKeyboardModifiers() {
        this._keyboardModifiers = { ctrl: false, alt: false, shift: false, meta: false, altgr: false };
        this._el.keyboardOverlay.querySelectorAll('.rdp-key-modifier').forEach(key => {
            key.classList.remove('pressed');
        });
        this._updateKeyboardKeyLabels();
    }
    
    /**
     * Update keyboard key labels based on active modifiers (shift, altgr, shift+altgr)
     * Priority: shift+altgr > altgr > shift > normal
     */
    _updateKeyboardKeyLabels() {
        const shiftActive = this._keyboardModifiers.shift;
        const altgrActive = this._keyboardModifiers.altgr;
        
        this._el.keyboardOverlay.querySelectorAll('.rdp-key[data-key]').forEach(key => {
            let label;
            
            if (shiftActive && altgrActive) {
                // Shift+AltGr: use data-shift-altgr, fallback to data-altgr, then data-shift, then data-key
                label = key.dataset.shiftAltgr || key.dataset.altgr || key.dataset.shift || key.dataset.key;
            } else if (altgrActive) {
                // AltGr only: use data-altgr, fallback to data-key
                label = key.dataset.altgr || key.dataset.key;
            } else if (shiftActive) {
                // Shift only: use data-shift, fallback to data-key
                label = key.dataset.shift || key.dataset.key;
            } else {
                // No modifiers: use data-key
                label = key.dataset.key;
            }
            
            key.textContent = label;
        });
    }
    
    /**
     * Handle virtual key press
     */
    _handleVirtualKeyDown(e, key) {
        if (!this._isConnected) return;
        e.preventDefault();
        e.stopPropagation();
        
        const code = key.dataset.code;
        const combo = key.dataset.combo;
        const modifier = key.dataset.modifier;
        
        // Handle special combos (no tracking needed, instant action)
        if (combo) {
            key.classList.add('pressed');
            this._sendVirtualKeyCombo(combo);
            // Remove pressed state after a short delay for visual feedback
            setTimeout(() => key.classList.remove('pressed'), 150);
            return;
        }
        
        // Handle modifier keys (toggle mode, no tracking)
        if (modifier) {
            this._keyboardModifiers[modifier] = !this._keyboardModifiers[modifier];
            if (this._keyboardModifiers[modifier]) {
                key.classList.add('pressed');
            } else {
                key.classList.remove('pressed');
            }
            // Update key labels when shift or altgr state changes
            if (modifier === 'shift' || modifier === 'altgr') {
                this._updateKeyboardKeyLabels();
            }
            return;
        }
        
        // Track this as the active key
        this._keyboardActiveKey = key;
        key.classList.add('pressed');
        
        // Get key name based on modifier state (priority: shift+altgr > altgr > shift > normal)
        const shiftActive = this._keyboardModifiers.shift;
        const altgrActive = this._keyboardModifiers.altgr;
        let keyName;
        let hasAltGrChar = false; // True if we have a dedicated AltGr character for this key
        
        if (shiftActive && altgrActive) {
            // Check if key has a dedicated shift+altgr character
            hasAltGrChar = !!key.dataset.shiftAltgr || !!key.dataset.altgr;
            keyName = key.dataset.shiftAltgr || key.dataset.altgr || key.dataset.shift || key.textContent;
        } else if (altgrActive) {
            // Check if key has a dedicated altgr character
            hasAltGrChar = !!key.dataset.altgr;
            keyName = key.dataset.altgr || key.dataset.key || key.textContent;
        } else if (shiftActive) {
            keyName = key.dataset.shift || key.textContent;
        } else {
            keyName = key.dataset.key || key.textContent;
        }
        
        // If we have a dedicated AltGr character, send it directly without modifier events.
        // The character itself (e.g., €, ñ, ß) is sent as Unicode, no Ctrl+Alt needed.
        // Only send modifier events if we DON'T have a dedicated AltGr char.
        if (!hasAltGrChar) {
            this._sendActiveModifierDownEvents();
        } else if (shiftActive && !altgrActive) {
            // Only shift is active (no altgr), send shift
            this._sendActiveModifierDownEvents();
        }
        
        // Send main key - when hasAltGrChar is true, send without modifier flags
        // so backend uses Unicode input for the special character
        this._sendMessage({
            type: 'key', action: 'down',
            key: keyName, code: code, keyCode: 0,
            ctrlKey: hasAltGrChar ? false : this._keyboardModifiers.ctrl,
            shiftKey: hasAltGrChar ? false : this._keyboardModifiers.shift,
            altKey: hasAltGrChar ? false : this._keyboardModifiers.alt,
            metaKey: hasAltGrChar ? false : this._keyboardModifiers.meta
        });
    }
    
    /**
     * Send key down events for all active modifiers
     */
    _sendActiveModifierDownEvents() {
        // AltGr is equivalent to Ctrl+Alt on Windows
        if (this._keyboardModifiers.altgr) {
            this._sendMessage({
                type: 'key', action: 'down',
                key: 'Control', code: 'ControlLeft', keyCode: 0,
                ctrlKey: true, shiftKey: false, altKey: false, metaKey: false
            });
            this._sendMessage({
                type: 'key', action: 'down',
                key: 'Alt', code: 'AltRight', keyCode: 0,
                ctrlKey: true, shiftKey: false, altKey: true, metaKey: false
            });
        } else {
            if (this._keyboardModifiers.ctrl) {
                this._sendMessage({
                    type: 'key', action: 'down',
                    key: 'Control', code: 'ControlLeft', keyCode: 0,
                    ctrlKey: true, shiftKey: false, altKey: false, metaKey: false
                });
            }
            if (this._keyboardModifiers.alt) {
                this._sendMessage({
                    type: 'key', action: 'down',
                    key: 'Alt', code: 'AltLeft', keyCode: 0,
                    ctrlKey: this._keyboardModifiers.ctrl, shiftKey: false, altKey: true, metaKey: false
                });
            }
        }
        if (this._keyboardModifiers.shift) {
            this._sendMessage({
                type: 'key', action: 'down',
                key: 'Shift', code: 'ShiftLeft', keyCode: 0,
                ctrlKey: this._keyboardModifiers.ctrl, shiftKey: true, altKey: this._keyboardModifiers.alt, metaKey: false
            });
        }
        if (this._keyboardModifiers.meta) {
            this._sendMessage({
                type: 'key', action: 'down',
                key: 'Meta', code: 'MetaLeft', keyCode: 0,
                ctrlKey: this._keyboardModifiers.ctrl, shiftKey: this._keyboardModifiers.shift, altKey: this._keyboardModifiers.alt, metaKey: true
            });
        }
    }
    
    /**
     * Send key up events for all active modifiers
     */
    _sendActiveModifierUpEvents() {
        if (this._keyboardModifiers.meta) {
            this._sendMessage({
                type: 'key', action: 'up',
                key: 'Meta', code: 'MetaLeft', keyCode: 0,
                ctrlKey: this._keyboardModifiers.ctrl, shiftKey: this._keyboardModifiers.shift, altKey: this._keyboardModifiers.alt, metaKey: false
            });
        }
        if (this._keyboardModifiers.shift) {
            this._sendMessage({
                type: 'key', action: 'up',
                key: 'Shift', code: 'ShiftLeft', keyCode: 0,
                ctrlKey: this._keyboardModifiers.ctrl, shiftKey: false, altKey: this._keyboardModifiers.alt, metaKey: false
            });
        }
        // AltGr releases Ctrl+Alt in reverse order
        if (this._keyboardModifiers.altgr) {
            this._sendMessage({
                type: 'key', action: 'up',
                key: 'Alt', code: 'AltRight', keyCode: 0,
                ctrlKey: true, shiftKey: false, altKey: false, metaKey: false
            });
            this._sendMessage({
                type: 'key', action: 'up',
                key: 'Control', code: 'ControlLeft', keyCode: 0,
                ctrlKey: false, shiftKey: false, altKey: false, metaKey: false
            });
        } else {
            if (this._keyboardModifiers.alt) {
                this._sendMessage({
                    type: 'key', action: 'up',
                    key: 'Alt', code: 'AltLeft', keyCode: 0,
                    ctrlKey: this._keyboardModifiers.ctrl, shiftKey: false, altKey: false, metaKey: false
                });
            }
            if (this._keyboardModifiers.ctrl) {
                this._sendMessage({
                    type: 'key', action: 'up',
                    key: 'Control', code: 'ControlLeft', keyCode: 0,
                    ctrlKey: false, shiftKey: false, altKey: false, metaKey: false
                });
            }
        }
    }
    
    /**
     * Handle virtual key release
     */
    _handleVirtualKeyUp(e, key) {
        if (!this._isConnected) return;
        
        const modifier = key.dataset.modifier;
        const combo = key.dataset.combo;
        
        // Don't process modifier keys or combos on mouseup (they toggle/instant)
        if (modifier || combo) return;
        
        // Only send key up if this is the actively pressed key
        if (this._keyboardActiveKey !== key) return;
        
        this._keyboardActiveKey = null;
        key.classList.remove('pressed');
        
        const code = key.dataset.code;
        
        // Get key name based on modifier state (priority: shift+altgr > altgr > shift > normal)
        const shiftActive = this._keyboardModifiers.shift;
        const altGrActive = this._keyboardModifiers.altgr;
        let keyName;
        let hasAltGrChar = false; // True if we have a dedicated AltGr character
        
        if (shiftActive && altGrActive) {
            hasAltGrChar = !!key.dataset.shiftAltgr || !!key.dataset.altgr;
            keyName = key.dataset.shiftAltgr || key.dataset.altgr || key.dataset.shift || key.textContent;
        } else if (altGrActive) {
            hasAltGrChar = !!key.dataset.altgr;
            keyName = key.dataset.altgr || key.dataset.key || key.textContent;
        } else if (shiftActive) {
            keyName = key.dataset.shift || key.textContent;
        } else {
            keyName = key.dataset.key || key.textContent;
        }
        
        // Send main key up - when hasAltGrChar, send without modifier flags
        this._sendMessage({
            type: 'key', action: 'up',
            key: keyName, code: code, keyCode: 0,
            ctrlKey: hasAltGrChar ? false : this._keyboardModifiers.ctrl,
            shiftKey: hasAltGrChar ? false : this._keyboardModifiers.shift,
            altKey: hasAltGrChar ? false : this._keyboardModifiers.alt,
            metaKey: hasAltGrChar ? false : this._keyboardModifiers.meta
        });
        
        // Send modifier key up events only if we sent them on key down
        if (!hasAltGrChar) {
            this._sendActiveModifierUpEvents();
        } else if (shiftActive && !altGrActive) {
            this._sendActiveModifierUpEvents();
        }
        
        // Reset modifiers after non-modifier key press
        this._resetKeyboardModifiers();
    }
    
    /**
     * Send a key combination (properly simulates modifier key presses)
     */
    _sendVirtualKeyCombo(combo) {
        const keys = combo.toLowerCase().split('+');
        const modifierKeys = [];
        let mainKey = null;
        let mainCode = null;
        
        keys.forEach(k => {
            switch (k) {
                case 'ctrl': 
                    modifierKeys.push({ key: 'Control', code: 'ControlLeft' });
                    break;
                case 'alt': 
                    modifierKeys.push({ key: 'Alt', code: 'AltLeft' });
                    break;
                case 'shift': 
                    modifierKeys.push({ key: 'Shift', code: 'ShiftLeft' });
                    break;
                case 'meta': case 'win': 
                    modifierKeys.push({ key: 'Meta', code: 'MetaLeft' });
                    break;
                case 'tab': 
                    mainKey = 'Tab'; 
                    mainCode = 'Tab'; 
                    break;
                case 'delete': 
                    mainKey = 'Delete'; 
                    mainCode = 'Delete'; 
                    break;
                default: 
                    mainKey = k; 
                    mainCode = k;
            }
        });
        
        if (!mainKey) return;
        
        // Build modifier state
        const modifiers = {
            ctrl: modifierKeys.some(m => m.key === 'Control'),
            alt: modifierKeys.some(m => m.key === 'Alt'),
            shift: modifierKeys.some(m => m.key === 'Shift'),
            meta: modifierKeys.some(m => m.key === 'Meta')
        };
        
        // Press modifier keys down first
        modifierKeys.forEach(mod => {
            this._sendMessage({
                type: 'key', action: 'down',
                key: mod.key, code: mod.code, keyCode: 0,
                ctrlKey: false, shiftKey: false, altKey: false, metaKey: false
            });
        });
        
        // Small delay then press main key
        setTimeout(() => {
            // Press main key down
            this._sendMessage({
                type: 'key', action: 'down',
                key: mainKey, code: mainCode, keyCode: 0,
                ctrlKey: modifiers.ctrl, shiftKey: modifiers.shift,
                altKey: modifiers.alt, metaKey: modifiers.meta
            });
            
            // Release main key
            setTimeout(() => {
                this._sendMessage({
                    type: 'key', action: 'up',
                    key: mainKey, code: mainCode, keyCode: 0,
                    ctrlKey: modifiers.ctrl, shiftKey: modifiers.shift,
                    altKey: modifiers.alt, metaKey: modifiers.meta
                });
                
                // Release modifier keys in reverse order
                setTimeout(() => {
                    [...modifierKeys].reverse().forEach(mod => {
                        this._sendMessage({
                            type: 'key', action: 'up',
                            key: mod.key, code: mod.code, keyCode: 0,
                            ctrlKey: false, shiftKey: false, altKey: false, metaKey: false
                        });
                    });
                }, 20);
            }, 30);
        }, 20);
    }
    
    /**
     * Start dragging the keyboard
     */
    _startKeyboardDrag(e) {
        this._keyboardDragging = true;
        const overlay = this._el.keyboardOverlay;
        const rect = overlay.getBoundingClientRect();
        const screenRect = this._el.screen.getBoundingClientRect();
        
        this._keyboardDragOffset = {
            x: e.clientX - (rect.left - screenRect.left),
            y: e.clientY - (rect.top - screenRect.top)
        };
    }
    
    /**
     * Handle keyboard dragging
     */
    _handleKeyboardDrag(e) {
        if (!this._keyboardDragging) return;
        
        const overlay = this._el.keyboardOverlay;
        const screenRect = this._el.screen.getBoundingClientRect();
        const overlayRect = overlay.getBoundingClientRect();
        
        let newLeft = e.clientX - this._keyboardDragOffset.x;
        let newTop = e.clientY - this._keyboardDragOffset.y;
        
        // Constrain to screen bounds
        newLeft = Math.max(0, Math.min(newLeft, screenRect.width - overlayRect.width));
        newTop = Math.max(0, Math.min(newTop, screenRect.height - overlayRect.height));
        
        overlay.style.left = `${newLeft}px`;
        overlay.style.top = `${newTop}px`;
    }
    
    /**
     * Start resizing the keyboard
     */
    _startKeyboardResize(e) {
        this._keyboardResizing = true;
        e.stopPropagation();
    }
    
    /**
     * Handle keyboard resizing with content scaling
     */
    _handleKeyboardResize(e) {
        if (!this._keyboardResizing) return;
        if (this._keyboardBaseWidth === 0) return;  // Not initialized
        
        const overlay = this._el.keyboardOverlay;
        const content = this._el.keyboardContent;
        const screenRect = this._el.screen.getBoundingClientRect();
        const overlayRect = overlay.getBoundingClientRect();
        
        // Scale limits
        const minScale = 0.6;
        const maxScale = 1.2;
        
        // Helper functions for calculating dimensions
        const calcWidth = (s) => (this._keyboardBaseWidth * s) + 16;
        const calcHeight = (s) => (this._keyboardBaseHeight * s) + this._keyboardTitlebarHeight + 16;
        
        // Calculate new width based on mouse position
        let newWidth = e.clientX - overlayRect.left + 10;
        
        // Calculate desired scale
        let scale = (newWidth - 16) / this._keyboardBaseWidth;
        
        // Clamp scale between min and max
        scale = Math.max(minScale, Math.min(maxScale, scale));
        
        // Check canvas boundaries and adjust if needed
        const maxWidth = screenRect.width - (overlayRect.left - screenRect.left);
        const maxHeight = screenRect.height - (overlayRect.top - screenRect.top);
        
        if (calcWidth(scale) > maxWidth) {
            scale = Math.max(minScale, (maxWidth - 16) / this._keyboardBaseWidth);
        }
        
        if (calcHeight(scale) > maxHeight) {
            const heightScale = (maxHeight - this._keyboardTitlebarHeight - 16) / this._keyboardBaseHeight;
            scale = Math.max(minScale, Math.min(scale, heightScale));
        }
        
        // Ensure we're still within scale limits after boundary checks
        scale = Math.max(minScale, Math.min(maxScale, scale));
        
        newWidth = calcWidth(scale);
        const newHeight = calcHeight(scale);
        
        content.style.transform = `scale(${scale})`;
        overlay.style.width = `${newWidth}px`;
        overlay.style.height = `${newHeight}px`;
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
        // Update resolution display
        this._el.resolution.textContent = `Resolution: ${width}x${height}`;
        
        // When using OffscreenCanvas (transferred to worker), we cannot resize
        // the HTMLCanvasElement directly. The worker owns the canvas now.
        // We need to notify the worker to resize the OffscreenCanvas instead.
        if (this._gfxWorker && this._gfxWorkerReady) {
            this._gfxWorker.postMessage({
                type: 'resize',
                data: { width, height }
            });
        } else {
            // Fallback: only resize HTMLCanvasElement if NOT transferred to worker
            try {
                this._canvas.width = width;
                this._canvas.height = height;
            } catch (e) {
                // Canvas was transferred to offscreen, ignore
            }
        }
        
        this._emit('resize', { width, height });
    }
}

// Re-export theme utilities for convenience
export { themes, resolveTheme, sanitizeTheme } from './rdp-themes.js';

// Default export for convenience
export default RDPClient;
