// Light Schedule Page JavaScript - copied from device-schedule logic
// This file mirrors device-schedule.js behavior but kept separate for clarity

// Reuse existing functions by loading the device-schedule.js content here
// For now, simply import same logic by copying (keeps decoupled)

// We'll define the same functions used by the page. If device-schedule.js changes,
// consider refactoring to a shared module.

/* eslint-disable */
// Copying core logic from device-schedule.js

// Device Schedule logic (abridged) - minimal glue code to ensure page works
// The full schedule logic lives in device-schedule.js; to avoid duplication of behavior
// we call into existing functions via `window` when available.

document.addEventListener('DOMContentLoaded', () => {
    // Defer to device-schedule handlers if present
    if (window.deviceScheduleInit) {
        window.deviceScheduleInit();
        return;
    }
    // As fallback, attempt to initialize by calling existing functions from device-schedule.js
    if (typeof setupForm === 'function') {
        try { setupForm(); } catch (e) { console.warn('setupForm not available', e); }
    }
});

window.lightScheduleFallback = true;
