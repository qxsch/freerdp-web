# Creating Custom Themes for RDP Web Client

This guide walks you through creating custom themes for the RDP Web Client. Whether you want to match your corporate branding, improve accessibility, or just personalize the look and feel, this guide covers everything you need to know.

## Table of Contents

1. [Understanding the Theme System](#understanding-the-theme-system)
2. [Theme Structure](#theme-structure)
3. [Quick Start: Your First Custom Theme](#quick-start-your-first-custom-theme)
4. [Color Properties Reference](#color-properties-reference)
5. [Typography Properties Reference](#typography-properties-reference)
6. [Shape Properties Reference](#shape-properties-reference)
7. [Step-by-Step: Creating a Corporate Theme](#step-by-step-creating-a-corporate-theme)
8. [Step-by-Step: Creating an Accessible Theme](#step-by-step-creating-an-accessible-theme)
9. [Best Practices](#best-practices)
10. [Troubleshooting](#troubleshooting)

---

## Understanding the Theme System

### How It Works

The RDP Web Client uses **CSS Custom Properties** (CSS Variables) under the hood. When you provide a theme configuration, the client:

1. **Validates** your color values (rejects invalid CSS colors)
2. **Merges** your settings with a base preset (defaults to `dark`)
3. **Converts** the theme object to CSS variables
4. **Injects** those variables into the Shadow DOM

This approach ensures:
- ✅ **Isolation**: Themes don't leak to or from your page
- ✅ **Safety**: Invalid values are rejected, not applied
- ✅ **Performance**: CSS variables are highly optimized
- ✅ **Flexibility**: Change themes instantly at runtime

### Theme Inheritance

Themes follow a **layered inheritance** model:

```
┌─────────────────────────────────────────────────────────┐
│  Your Custom Overrides (highest priority)               │
│  { colors: { accent: '#ff5722' } }                      │
├─────────────────────────────────────────────────────────┤
│  Base Preset (if specified)                             │
│  { preset: 'dark' } → uses darkTheme as base            │
├─────────────────────────────────────────────────────────┤
│  Default Theme (dark)                                   │
│  Always used as fallback for any missing values         │
└─────────────────────────────────────────────────────────┘
```

This means you only need to specify the values you want to change!

---

## Theme Structure

A complete theme has three sections: **colors**, **typography**, and **shape**.

```javascript
const myTheme = {
    // Optional: Start from a preset and override
    preset: 'dark',  // 'dark' | 'light' | 'midnight' | 'highContrast'
    
    colors: {
        // Backgrounds
        background: '#1a1a2e',      // Main app background
        surface: '#16213e',         // Panels, modals, toolbars
        
        // Borders
        border: '#0f3460',          // Borders and separators
        
        // Text
        text: '#eeeeee',            // Primary text
        textMuted: '#888888',       // Secondary/dimmed text
        
        // Accent (brand color)
        accent: '#51cf66',          // Focus rings, active states
        accentText: '#000000',      // Text on accent backgrounds
        
        // Status
        error: '#ff6b6b',           // Error/disconnect indicator
        success: '#51cf66',         // Connected indicator
        
        // Buttons
        buttonBg: '#0f3460',        // Button background
        buttonHover: '#1a4a7a',     // Button hover state
        buttonText: '#eeeeee',      // Button text
        buttonActiveBg: '#51cf66',  // Button pressed/active background
        buttonActiveText: '#000000', // Button pressed/active text
        
        // Form inputs
        inputBg: '#1a1a2e',         // Input background
        inputBorder: '#0f3460',     // Input border
        inputFocusBorder: '#51cf66', // Input focus border
    },
    
    typography: {
        fontFamily: "-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif",
        fontSize: '14px',           // Base font size
        fontSizeSmall: '0.85rem',   // Small text (labels, status)
    },
    
    shape: {
        borderRadius: '4px',        // Buttons, inputs
        borderRadiusLarge: '8px',   // Modals, overlays
    }
};
```

---

## Quick Start: Your First Custom Theme

### Step 1: Import the RDPClient

```javascript
import { RDPClient } from './rdp-client.js';
```

### Step 2: Define Your Theme

Start simple - just override what you want to change:

```javascript
const myTheme = {
    colors: {
        accent: '#e91e63',  // Pink accent
        success: '#4caf50', // Green for connected state
    }
};
```

### Step 3: Apply at Construction

```javascript
const client = new RDPClient(document.getElementById('container'), {
    wsUrl: 'ws://localhost:8765',
    theme: myTheme
});
```

### Step 4: Or Apply Dynamically

```javascript
// Change theme at any time
client.setTheme(myTheme);

// Switch to a preset
client.setTheme({ preset: 'light' });

// Combine preset with overrides
client.setTheme({
    preset: 'midnight',
    colors: { accent: '#00bcd4' }
});
```

That's it! Your custom colors are now applied.

---

## Color Properties Reference

### Background Colors

| Property | Purpose | Used By |
|----------|---------|---------|
| `background` | Main application background | Container, screen area |
| `surface` | Elevated surfaces | Toolbars, modals, panels, keyboard overlay |

**Design Tip**: `surface` should be slightly lighter/darker than `background` to create visual hierarchy.

### Border Colors

| Property | Purpose | Used By |
|----------|---------|---------|
| `border` | Separators and outlines | Toolbar borders, input borders, screen border |

### Text Colors

| Property | Purpose | Used By |
|----------|---------|---------|
| `text` | Primary text | Labels, headings, button text |
| `textMuted` | Secondary text | Status bar, hints, disabled text |

**Contrast Tip**: Ensure at least 4.5:1 contrast ratio between `text` and `background` for accessibility.

### Accent Colors

| Property | Purpose | Used By |
|----------|---------|---------|
| `accent` | Primary brand/action color | Focus rings, primary buttons, active states |
| `accentText` | Text on accent backgrounds | Primary button text, active key text |

**Design Tip**: Choose an `accentText` that contrasts well with `accent`. Usually black on light accents, white on dark accents.

### Status Colors

| Property | Purpose | Used By |
|----------|---------|---------|
| `success` | Positive/connected state | Connection status dot (when connected) |
| `error` | Negative/error state | Connection status dot (disconnected), error messages |

### Button Colors

| Property | Purpose | Used By |
|----------|---------|---------|
| `buttonBg` | Default button background | All buttons, keyboard keys |
| `buttonHover` | Hover state background | Button hover effect |
| `buttonText` | Button text color | Button labels |
| `buttonActiveBg` | Pressed/active state | Keyboard key pressed, button click |
| `buttonActiveText` | Text when active | Text on pressed buttons |

### Input Colors

| Property | Purpose | Used By |
|----------|---------|---------|
| `inputBg` | Input field background | Text inputs in connection modal |
| `inputBorder` | Input border color | Default input border |
| `inputFocusBorder` | Focused input border | Input when focused |

---

## Typography Properties Reference

| Property | Purpose | Example Values |
|----------|---------|----------------|
| `fontFamily` | Font stack for all text | `"'Inter', sans-serif"` |
| `fontSize` | Base font size | `'14px'`, `'16px'` |
| `fontSizeSmall` | Small/secondary text | `'0.85rem'`, `'12px'` |

### Font Stack Examples

```javascript
// System fonts (fastest loading)
fontFamily: "-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif"

// Google Font (requires @import or <link>)
fontFamily: "'Inter', 'Helvetica Neue', Arial, sans-serif"

// Monospace for technical look
fontFamily: "'JetBrains Mono', 'Fira Code', monospace"
```

---

## Shape Properties Reference

| Property | Purpose | Example Values |
|----------|---------|----------------|
| `borderRadius` | Small elements | `'4px'`, `'8px'`, `'0px'` |
| `borderRadiusLarge` | Large elements | `'8px'`, `'12px'`, `'16px'` |

### Shape Styles

```javascript
// Sharp/technical look
shape: { borderRadius: '0px', borderRadiusLarge: '0px' }

// Subtle rounding (default)
shape: { borderRadius: '4px', borderRadiusLarge: '8px' }

// Soft/modern look
shape: { borderRadius: '8px', borderRadiusLarge: '16px' }

// Pill-shaped buttons
shape: { borderRadius: '9999px', borderRadiusLarge: '16px' }
```

---

## Step-by-Step: Creating a Corporate Theme

Let's create a theme for a fictional company "TechCorp" with blue branding.

### Step 1: Gather Brand Colors

First, identify your brand colors:
- **Primary**: #0066cc (TechCorp Blue)
- **Secondary**: #004499 (Darker blue for hover)
- **Accent**: #00a3e0 (Lighter blue for highlights)

### Step 2: Choose a Base Palette

Decide on light or dark mode:
```javascript
// Dark mode base
const darkBase = {
    background: '#0a1929',  // Very dark blue-gray
    surface: '#132f4c',     // Slightly lighter
};

// Light mode base
const lightBase = {
    background: '#f5f7fa',  // Light gray
    surface: '#ffffff',     // Pure white
};
```

### Step 3: Build the Complete Theme

```javascript
const techCorpTheme = {
    colors: {
        // Backgrounds (dark mode)
        background: '#0a1929',
        surface: '#132f4c',
        
        // Borders
        border: '#1e4976',
        
        // Text
        text: '#ffffff',
        textMuted: '#94a3b8',
        
        // Brand colors
        accent: '#0066cc',
        accentText: '#ffffff',
        
        // Status
        success: '#10b981',
        error: '#ef4444',
        
        // Buttons using brand colors
        buttonBg: '#1e4976',
        buttonHover: '#2d5a8a',
        buttonText: '#ffffff',
        buttonActiveBg: '#0066cc',
        buttonActiveText: '#ffffff',
        
        // Inputs
        inputBg: '#0a1929',
        inputBorder: '#1e4976',
        inputFocusBorder: '#0066cc',
    },
    
    typography: {
        fontFamily: "'Inter', -apple-system, BlinkMacSystemFont, sans-serif",
        fontSize: '14px',
        fontSizeSmall: '0.875rem',
    },
    
    shape: {
        borderRadius: '6px',
        borderRadiusLarge: '12px',
    }
};
```

### Step 4: Apply the Theme

```javascript
const client = new RDPClient(container, {
    wsUrl: 'wss://rdp.techcorp.com',
    theme: techCorpTheme
});
```

### Step 5: Test All States

Verify your theme looks good in all states:
- [ ] Disconnected state (modal visible)
- [ ] Connecting state (loading spinner)
- [ ] Connected state (green indicator)
- [ ] Error state (red indicator)
- [ ] Button hover and active states
- [ ] Input focus states
- [ ] Virtual keyboard open

---

## Step-by-Step: Creating an Accessible Theme

Let's create a high-contrast theme optimized for users with visual impairments.

### Step 1: Understand WCAG Requirements

- **Text contrast**: Minimum 4.5:1 for normal text, 3:1 for large text
- **UI components**: Minimum 3:1 contrast ratio
- **Focus indicators**: Clearly visible focus states

### Step 2: Choose High-Contrast Colors

```javascript
const accessibleTheme = {
    colors: {
        // Pure black background for maximum contrast
        background: '#000000',
        surface: '#1a1a1a',
        
        // High contrast borders
        border: '#ffffff',
        
        // White text on black = 21:1 contrast ratio
        text: '#ffffff',
        textMuted: '#ffff00',  // Yellow for secondary (still high contrast)
        
        // Bright, distinguishable accent
        accent: '#00ff00',      // Bright green
        accentText: '#000000',
        
        // Distinct status colors
        success: '#00ff00',     // Bright green
        error: '#ff0000',       // Bright red
        
        // High contrast buttons
        buttonBg: '#333333',
        buttonHover: '#4d4d4d',
        buttonText: '#ffffff',
        buttonActiveBg: '#ffff00',  // Yellow when active
        buttonActiveText: '#000000',
        
        // Clear input states
        inputBg: '#000000',
        inputBorder: '#ffffff',
        inputFocusBorder: '#00ff00',
    },
    
    typography: {
        // Larger base font for readability
        fontSize: '16px',
        fontSizeSmall: '14px',  // Don't go too small
    },
    
    shape: {
        // Sharp corners for clarity
        borderRadius: '0px',
        borderRadiusLarge: '0px',
    }
};
```

### Step 3: Test with Accessibility Tools

1. **Color contrast checker**: Use WebAIM or similar tools
2. **Screen reader**: Test with NVDA, VoiceOver, or JAWS
3. **Keyboard navigation**: Ensure all controls are reachable

---

## Best Practices

### 1. Start from a Preset

Always extend an existing preset rather than starting from scratch:

```javascript
// ✅ Good: Extend dark preset
{ preset: 'dark', colors: { accent: '#e91e63' } }

// ⚠️ Risky: All colors from scratch (easy to miss something)
{ colors: { background: '#000', text: '#fff', /* ... many more */ } }
```

### 2. Maintain Sufficient Contrast

| Element | Minimum Contrast |
|---------|-----------------|
| Body text | 4.5:1 |
| Large text (18px+) | 3:1 |
| UI components | 3:1 |
| Focus indicators | 3:1 |

Use a [contrast checker](https://webaim.org/resources/contrastchecker/) to verify.

### 3. Test Both States

Always test connected AND disconnected states:
- Modal appearance
- Button states (normal, hover, active, disabled)
- Input focus states
- Virtual keyboard

### 4. Use Semantic Naming

When creating reusable themes, name by purpose not color:

```javascript
// ✅ Good: Semantic naming
const brandTheme = { colors: { accent: '#0066cc' } };

// ❌ Bad: Color-based naming
const blueTheme = { colors: { accent: '#0066cc' } };
```

### 5. Consider Color Blindness

- Don't rely solely on red/green for status
- Use icons or text labels alongside colors
- Test with color blindness simulators

### 6. Validate Colors

The theme system validates color values. Valid formats:
- Hex: `#fff`, `#ffffff`, `#ffffffff`
- RGB: `rgb(255, 255, 255)`
- RGBA: `rgba(255, 255, 255, 0.5)`
- HSL: `hsl(0, 0%, 100%)`
- Named: `white`, `black`, `transparent`

Invalid colors are logged to console and ignored.

---

## Troubleshooting

### Theme Not Applying

**Symptoms**: Colors don't change after calling `setTheme()`

**Solutions**:
1. Check browser console for validation warnings
2. Verify you're using valid CSS color values
3. Ensure you're calling `setTheme()` on the correct client instance

```javascript
// Check for errors
client.setTheme({ colors: { accent: 'not-a-color' } });
// Console: [RDPTheme] Invalid color value for "accent": not-a-color
```

### Only Some Colors Changed

**Symptoms**: Some elements use new colors, others don't

**Solutions**:
1. You may have missed some related properties
2. Check that you're setting both foreground and background

```javascript
// ❌ Incomplete: accent changed but text might be invisible
{ colors: { accent: '#000000' } }

// ✅ Complete: accent and text both updated
{ colors: { accent: '#000000', accentText: '#ffffff' } }
```

### Colors Look Wrong

**Symptoms**: Colors appear different than expected

**Solutions**:
1. Check for CSS `color-mix()` usage (used by special keys)
2. Verify your hex values are correct (no typos)
3. Test in different browsers

### Font Not Loading

**Symptoms**: Custom font not appearing

**Solutions**:
1. Ensure the font is loaded before the client initializes
2. Add a fallback font family
3. Check for CORS issues if loading from CDN

```html
<!-- Load font before your script -->
<link href="https://fonts.googleapis.com/css2?family=Inter&display=swap" rel="stylesheet">
```

```javascript
// Always include fallbacks
fontFamily: "'Inter', -apple-system, sans-serif"
```

---

## Examples Gallery

### Minimal Dark

```javascript
{ preset: 'dark' }
```

### Ocean Blue

```javascript
{
    preset: 'dark',
    colors: {
        accent: '#00b4d8',
        success: '#00b4d8',
        buttonActiveBg: '#00b4d8',
        inputFocusBorder: '#00b4d8',
    }
}
```

### Sunset Orange

```javascript
{
    preset: 'dark',
    colors: {
        accent: '#ff7043',
        accentText: '#000000',
        success: '#66bb6a',
        buttonActiveBg: '#ff7043',
        buttonActiveText: '#000000',
    }
}
```

### Forest Green

```javascript
{
    preset: 'dark',
    colors: {
        background: '#1b2d1b',
        surface: '#2d4a2d',
        border: '#3d5c3d',
        accent: '#81c784',
        success: '#81c784',
    }
}
```

### Clean Light

```javascript
{ preset: 'light' }
```

### Warm Light

```javascript
{
    preset: 'light',
    colors: {
        background: '#faf8f5',
        surface: '#ffffff',
        accent: '#d97706',
        success: '#059669',
    }
}
```

---

## Next Steps

- See the [README.md](./README.md#theming) for quick reference
- Check [rdp-themes.js](./frontend/rdp-themes.js) for preset source code
- Open an issue if you create a theme you'd like to share!
