# UI Design Guide - Aquarium Management System

**Supplemental instructions for creating new web UI pages using established design standards**

---

## 📋 Overview

This guide ensures consistency across all web interface pages by following the design system established in `index.html`. Use this as a reference when creating new pages like device configuration screens, scheduling interfaces, settings panels, etc.

---

## 🎨 Design System

### Color Palette

```css
/* Primary Colors (Blues) */
--color-primary: #0ea5e9;        /* Sky blue - primary actions */
--color-primary-dark: #0c4a6e;   /* Deep blue - headers, emphasis */
--color-primary-light: #e0f2fe;  /* Light blue - backgrounds */

/* Accent Colors */
--color-accent: #10b981;         /* Green - success, online status */
--color-accent-warning: #f59e0b; /* Orange - warnings */
--color-accent-danger: #ef4444;  /* Red - errors, critical */

/* Neutral Colors */
--color-bg: #f8fafc;             /* Light gray - page background */
--color-surface: #ffffff;        /* White - card/panel background */
--color-border: #e2e8f0;         /* Light gray - borders */
--color-text: #1e293b;           /* Dark gray - primary text */
--color-text-secondary: #64748b; /* Medium gray - secondary text */

/* Gradients */
background: linear-gradient(135deg, #0c4a6e 0%, #0ea5e9 100%);
```

### Typography

**Font Family:**
```html
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&display=swap" rel="stylesheet">
```

```css
font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
```

**Font Sizes:**
```css
/* Headings */
h1: 2rem (32px)        /* Page titles */
h2: 1.5rem (24px)      /* Section titles */
h3: 1.25rem (20px)     /* Card titles */

/* Body Text */
body: 0.875rem (14px)  /* Default text */
small: 0.75rem (12px)  /* Labels, timestamps */

/* Mobile Adjustments */
@media (max-width: 480px) {
    h1: 1.5rem (24px)
    h2: 1.25rem (20px)
    h3: 1.125rem (18px)
    body: 0.8125rem (13px)
}
```

**Font Weights:**
- Light: 300 (secondary text)
- Regular: 400 (body text)
- Medium: 500 (emphasis)
- Semibold: 600 (headings)
- Bold: 700 (important elements)

### Spacing System

Use consistent spacing multiples of 4px:

```css
--spacing-xs: 0.25rem;   /* 4px */
--spacing-sm: 0.5rem;    /* 8px */
--spacing-md: 1rem;      /* 16px */
--spacing-lg: 1.5rem;    /* 24px */
--spacing-xl: 2rem;      /* 32px */
--spacing-2xl: 3rem;     /* 48px */
```

### Shadows

```css
/* Cards and panels */
box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);

/* Hover states */
box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);

/* Elevated elements (modals) */
box-shadow: 0 10px 25px rgba(0, 0, 0, 0.2);
```

### Border Radius

```css
--radius-sm: 0.375rem;   /* 6px - buttons, inputs */
--radius-md: 0.5rem;     /* 8px - cards */
--radius-lg: 0.75rem;    /* 12px - panels */
--radius-full: 9999px;   /* Circular - badges, avatars */
```

---

## 🏗️ Page Structure Template

### Basic HTML Structure

```html
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=5.0, user-scalable=yes">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
    <meta name="theme-color" content="#0c4a6e">
    <title>Page Title - Aquarium Hub</title>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&display=swap" rel="stylesheet">
    <link rel="stylesheet" href="styles/styles.css">
</head>
<body>
    <!-- Navbar -->
    <nav class="navbar">
        <div class="navbar-content">
            <div class="navbar-brand">
                <img src="images/logo.png" alt="Logo" class="navbar-logo">
                <div>
                    <h1 class="brand-title">Aquarium Hub</h1>
                    <p class="brand-subtitle">Page Title</p>
                </div>
            </div>
            <div class="navbar-actions">
                <!-- Add action buttons here -->
            </div>
        </div>
    </nav>

    <div class="container">
        <!-- Sidebar -->
        <aside class="sidebar">
            <div class="sidebar-header">
                <h2>Navigation</h2>
            </div>
            <nav class="sidebar-nav">
                <!-- Navigation items -->
            </nav>
            <div class="sidebar-footer">
                <p>&copy; 2026 Aquarium Hub</p>
            </div>
        </aside>

        <!-- Main Content -->
        <main class="main-content">
            <div class="page-header">
                <h1>Page Title</h1>
                <p class="page-description">Page description goes here</p>
            </div>

            <!-- Your content here -->

        </main>
    </div>

    <script src="scripts/app.js"></script>
    <script src="scripts/your-page.js"></script>
</body>
</html>
```

---

## 🧩 Reusable Components

### 1. Card Component

```html
<div class="card">
    <div class="card-header">
        <h3>Card Title</h3>
        <span class="card-badge">Badge</span>
    </div>
    <div class="card-body">
        <p>Card content goes here</p>
    </div>
    <div class="card-footer">
        <button class="btn btn-primary">Action</button>
    </div>
</div>
```

```css
.card {
    background: var(--color-surface);
    border-radius: var(--radius-md);
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
    overflow: hidden;
    transition: all 0.3s ease;
}

.card:hover {
    box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
    transform: translateY(-2px);
}

.card-header {
    padding: var(--spacing-lg);
    border-bottom: 1px solid var(--color-border);
    display: flex;
    justify-content: space-between;
    align-items: center;
}

.card-body {
    padding: var(--spacing-lg);
}

.card-footer {
    padding: var(--spacing-lg);
    border-top: 1px solid var(--color-border);
    background: var(--color-bg);
}
```

### 2. Button Variants

```html
<!-- Primary Action -->
<button class="btn btn-primary">Primary</button>

<!-- Secondary Action -->
<button class="btn btn-secondary">Secondary</button>

<!-- Success Action -->
<button class="btn btn-success">Success</button>

<!-- Warning Action -->
<button class="btn btn-warning">Warning</button>

<!-- Danger Action -->
<button class="btn btn-danger">Danger</button>

<!-- Icon Button -->
<button class="btn btn-icon">
    <span class="icon">🔧</span>
    Settings
</button>
```

```css
.btn {
    padding: 0.75rem 1.5rem;
    border: none;
    border-radius: var(--radius-sm);
    font-weight: 500;
    cursor: pointer;
    transition: all 0.2s ease;
    display: inline-flex;
    align-items: center;
    gap: 0.5rem;
    touch-action: manipulation;
}

.btn-primary {
    background: var(--color-primary);
    color: white;
}

.btn-primary:hover {
    background: var(--color-primary-dark);
}

.btn-secondary {
    background: var(--color-border);
    color: var(--color-text);
}

.btn-success {
    background: var(--color-accent);
    color: white;
}

.btn-warning {
    background: var(--color-accent-warning);
    color: white;
}

.btn-danger {
    background: var(--color-accent-danger);
    color: white;
}
```

### 3. Form Components

```html
<div class="form-group">
    <label for="input-id" class="form-label">Label</label>
    <input type="text" id="input-id" class="form-input" placeholder="Enter value">
    <small class="form-help">Helper text</small>
</div>

<div class="form-group">
    <label for="select-id" class="form-label">Select</label>
    <select id="select-id" class="form-input">
        <option>Option 1</option>
        <option>Option 2</option>
    </select>
</div>

<div class="form-group">
    <label class="checkbox-label">
        <input type="checkbox" class="checkbox">
        <span>Checkbox label</span>
    </label>
</div>
```

```css
.form-group {
    margin-bottom: var(--spacing-lg);
}

.form-label {
    display: block;
    margin-bottom: var(--spacing-sm);
    font-weight: 500;
    color: var(--color-text);
}

.form-input {
    width: 100%;
    padding: 0.75rem;
    border: 1px solid var(--color-border);
    border-radius: var(--radius-sm);
    font-family: inherit;
    font-size: 0.875rem;
    transition: border-color 0.2s ease;
}

.form-input:focus {
    outline: none;
    border-color: var(--color-primary);
    box-shadow: 0 0 0 3px rgba(14, 165, 233, 0.1);
}

.form-help {
    display: block;
    margin-top: var(--spacing-sm);
    color: var(--color-text-secondary);
    font-size: 0.75rem;
}
```

### 4. Status Badges

```html
<span class="badge badge-online">Online</span>
<span class="badge badge-offline">Offline</span>
<span class="badge badge-warning">Warning</span>
<span class="badge badge-error">Error</span>
```

```css
.badge {
    display: inline-flex;
    align-items: center;
    gap: 0.25rem;
    padding: 0.25rem 0.75rem;
    border-radius: var(--radius-full);
    font-size: 0.75rem;
    font-weight: 500;
}

.badge-online {
    background: rgba(16, 185, 129, 0.1);
    color: var(--color-accent);
}

.badge-offline {
    background: rgba(100, 116, 139, 0.1);
    color: var(--color-text-secondary);
}

.badge-warning {
    background: rgba(245, 158, 11, 0.1);
    color: var(--color-accent-warning);
}

.badge-error {
    background: rgba(239, 68, 68, 0.1);
    color: var(--color-accent-danger);
}
```

### 5. Statistics Card

```html
<div class="stat-card">
    <div class="stat-icon stat-icon-online">
        <span>🟢</span>
    </div>
    <div class="stat-content">
        <div class="stat-value">12</div>
        <div class="stat-label">Active Devices</div>
    </div>
</div>
```

```css
.stat-card {
    background: white;
    padding: var(--spacing-lg);
    border-radius: var(--radius-md);
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
    display: flex;
    align-items: center;
    gap: var(--spacing-md);
    transition: all 0.3s ease;
}

.stat-card:hover {
    transform: translateY(-2px);
    box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
}

.stat-icon {
    width: 60px;
    height: 60px;
    border-radius: var(--radius-md);
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 1.5rem;
}

.stat-icon-online {
    background: linear-gradient(135deg, rgba(16, 185, 129, 0.2) 0%, rgba(16, 185, 129, 0.1) 100%);
}

.stat-value {
    font-size: 2rem;
    font-weight: 700;
    color: var(--color-text);
}

.stat-label {
    font-size: 0.875rem;
    color: var(--color-text-secondary);
}
```

### 6. Activity/Timeline Item

```html
<div class="activity-item">
    <div class="activity-icon activity-icon-success">
        <span>✓</span>
    </div>
    <div class="activity-content">
        <div class="activity-title">Action completed</div>
        <div class="activity-time">2 minutes ago</div>
    </div>
</div>
```

```css
.activity-item {
    display: flex;
    gap: var(--spacing-md);
    padding: var(--spacing-md);
    border-bottom: 1px solid var(--color-border);
}

.activity-item:last-child {
    border-bottom: none;
}

.activity-icon {
    width: 40px;
    height: 40px;
    border-radius: var(--radius-full);
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
}

.activity-icon-success {
    background: rgba(16, 185, 129, 0.1);
    color: var(--color-accent);
}

.activity-title {
    font-weight: 500;
    color: var(--color-text);
}

.activity-time {
    font-size: 0.75rem;
    color: var(--color-text-secondary);
    margin-top: 0.25rem;
}
```

---

## 📱 Responsive Design

### Breakpoints

```css
/* Tablet and Mobile */
@media (max-width: 768px) {
    /* Adjust layouts for tablets and large phones */
}

/* Extra Small Mobile */
@media (max-width: 480px) {
    /* Adjust layouts for small phones */
}
```

### Mobile-First Rules

1. **Navigation**
   - Convert sidebar to horizontal scroll on mobile
   - Use hamburger menu for complex navigation
   - Ensure touch targets are at least 44px

2. **Layout**
   - Stack cards vertically (single column)
   - Reduce padding/margins proportionally
   - Hide non-essential elements

3. **Typography**
   - Scale down font sizes (but keep readable: min 13px)
   - Reduce line height slightly
   - Maintain hierarchy

4. **Touch Interactions**
   ```css
   body {
       -webkit-tap-highlight-color: transparent;
   }
   
   button, a, input, select {
       touch-action: manipulation;
   }
   ```

5. **Viewport Meta Tag**
   ```html
   <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=5.0, user-scalable=yes">
   ```

### Mobile Layout Example

```css
/* Desktop: Two-column grid */
.content-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(350px, 1fr));
    gap: var(--spacing-lg);
}

/* Mobile: Single column */
@media (max-width: 768px) {
    .content-grid {
        grid-template-columns: 1fr;
        gap: var(--spacing-md);
    }
}

/* Extra Small: Reduced spacing */
@media (max-width: 480px) {
    .content-grid {
        gap: var(--spacing-sm);
    }
}
```

---

## 🎯 Interactive States

### Hover Effects

```css
.interactive-element {
    transition: all 0.2s ease;
}

.interactive-element:hover {
    transform: translateY(-2px);
    box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
}
```

### Active/Focus States

```css
.btn:active {
    transform: scale(0.98);
}

.form-input:focus {
    outline: none;
    border-color: var(--color-primary);
    box-shadow: 0 0 0 3px rgba(14, 165, 233, 0.1);
}
```

### Loading States

```html
<button class="btn btn-primary" disabled>
    <span class="spinner"></span>
    Loading...
</button>
```

```css
.spinner {
    display: inline-block;
    width: 16px;
    height: 16px;
    border: 2px solid rgba(255, 255, 255, 0.3);
    border-top-color: white;
    border-radius: 50%;
    animation: spin 0.6s linear infinite;
}

@keyframes spin {
    to { transform: rotate(360deg); }
}
```

---

## 🔌 WebSocket Integration

### Connection Pattern

```javascript
let ws;

function connectWebSocket() {
    ws = new WebSocket(`ws://${window.location.hostname}/ws`);
    
    ws.onopen = () => {
        console.log('WebSocket connected');
        updateConnectionStatus('online');
    };
    
    ws.onmessage = (event) => {
        const message = JSON.parse(event.data);
        handleMessage(message);
    };
    
    ws.onerror = (error) => {
        console.error('WebSocket error:', error);
        updateConnectionStatus('error');
    };
    
    ws.onclose = () => {
        console.log('WebSocket disconnected');
        updateConnectionStatus('offline');
        setTimeout(connectWebSocket, 3000); // Reconnect after 3s
    };
}

function sendCommand(command) {
    if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(command));
    } else {
        console.error('WebSocket not connected');
    }
}
```

### Message Format

```javascript
// Command from UI to Hub
const command = {
    type: 'command',
    tankId: 1,
    deviceMac: 'AA:BB:CC:DD:EE:FF',
    action: 'setLevel',
    params: {
        level: 75
    }
};

// Status update from Hub to UI
const status = {
    type: 'status',
    tankId: 1,
    deviceMac: 'AA:BB:CC:DD:EE:FF',
    online: true,
    data: {
        level: 75,
        timestamp: 1234567890
    }
};
```

---

## ✅ Checklist for New Pages

### Before Creating a Page

- [ ] Define page purpose and user flow
- [ ] Sketch layout (dashboard, form, detail view, etc.)
- [ ] Identify required components from this guide
- [ ] Plan mobile responsive behavior

### HTML Structure

- [ ] Include viewport meta tag with mobile settings
- [ ] Include Inter font from Google Fonts
- [ ] Link to styles/styles.css
- [ ] Include navbar with gradient background
- [ ] Include sidebar (or horizontal nav on mobile)
- [ ] Use semantic HTML (main, section, article, nav)
- [ ] Add proper aria-labels for accessibility

### CSS Implementation

- [ ] Use CSS custom properties from design system
- [ ] Implement responsive breakpoints (768px, 480px)
- [ ] Add hover/focus/active states
- [ ] Include touch-action: manipulation on interactive elements
- [ ] Test horizontal scrolling (should only be intentional)
- [ ] Ensure text contrast meets WCAG AA standards

### JavaScript Functionality

- [ ] Initialize WebSocket connection
- [ ] Handle connection status updates
- [ ] Implement message handlers
- [ ] Add error handling and user feedback
- [ ] Include reconnection logic
- [ ] Add activity logging (if applicable)

### Testing

- [ ] Test in Chrome/Firefox/Safari
- [ ] Test on mobile device (or Chrome DevTools mobile emulation)
- [ ] Verify touch targets are at least 44px
- [ ] Check text readability (minimum 13px on mobile)
- [ ] Test WebSocket connection/disconnection
- [ ] Verify forms work correctly
- [ ] Check loading states and error messages

---

## 🚀 Quick Start Example

Here's a minimal new page following all standards:

**schedule.html**
```html
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=5.0, user-scalable=yes">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <meta name="theme-color" content="#0c4a6e">
    <title>Schedule - Aquarium Hub</title>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&display=swap" rel="stylesheet">
    <link rel="stylesheet" href="styles/styles.css">
</head>
<body>
    <nav class="navbar">
        <div class="navbar-content">
            <div class="navbar-brand">
                <img src="images/logo.png" alt="Logo" class="navbar-logo">
                <div>
                    <h1 class="brand-title">Aquarium Hub</h1>
                    <p class="brand-subtitle">Schedule Management</p>
                </div>
            </div>
        </div>
    </nav>

    <div class="container">
        <aside class="sidebar">
            <nav class="sidebar-nav">
                <a href="index.html" class="nav-item">
                    <span class="nav-icon">🏠</span>
                    <span class="nav-label">Dashboard</span>
                </a>
                <a href="schedule.html" class="nav-item active">
                    <span class="nav-icon">📅</span>
                    <span class="nav-label">Schedule</span>
                </a>
            </nav>
        </aside>

        <main class="main-content">
            <div class="page-header">
                <h1>Schedule Management</h1>
                <p class="page-description">Configure automated schedules for your devices</p>
            </div>

            <div class="card">
                <div class="card-header">
                    <h3>Create New Schedule</h3>
                </div>
                <div class="card-body">
                    <form id="scheduleForm">
                        <div class="form-group">
                            <label for="device" class="form-label">Device</label>
                            <select id="device" class="form-input">
                                <option>Select device...</option>
                            </select>
                        </div>
                        
                        <div class="form-group">
                            <label for="action" class="form-label">Action</label>
                            <input type="text" id="action" class="form-input" placeholder="e.g., Turn On">
                        </div>
                        
                        <button type="submit" class="btn btn-primary">Create Schedule</button>
                    </form>
                </div>
            </div>
        </main>
    </div>

    <script src="scripts/app.js"></script>
    <script src="scripts/schedule.js"></script>
</body>
</html>
```

**scripts/schedule.js**
```javascript
document.addEventListener('DOMContentLoaded', () => {
    const form = document.getElementById('scheduleForm');
    
    form.addEventListener('submit', (e) => {
        e.preventDefault();
        
        const data = {
            type: 'createSchedule',
            device: document.getElementById('device').value,
            action: document.getElementById('action').value
        };
        
        if (window.ws && window.ws.readyState === WebSocket.OPEN) {
            window.ws.send(JSON.stringify(data));
        }
    });
});
```

---

## 📚 Additional Resources

- **Existing Pages**: Reference `index.html` for complete implementation
- **Styles**: Full design system in `styles/styles.css`
- **Scripts**: WebSocket patterns in `scripts/app.js`
- **Backups**: Original simple UI in `*.backup` files

---

**Last Updated**: January 1, 2026  
**Version**: 1.0  
**Based on**: index.html professional redesign  
**Repository**: bghosh412/aquarium-management-system
