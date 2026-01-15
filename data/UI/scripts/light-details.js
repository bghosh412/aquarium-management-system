// Light Device Details Page JavaScript

let deviceMac = null;
let tankId = null;
let deviceData = null;

document.addEventListener('DOMContentLoaded', () => {
    const params = new URLSearchParams(window.location.search);
    deviceMac = params.get('mac') || localStorage.getItem('selectedDeviceMac');
    tankId = parseInt(params.get('tankId')) || parseInt(localStorage.getItem('selectedTankId'));
    
    if (!deviceMac) {
        // Try to pick the first device from localStorage
        const devices = JSON.parse(localStorage.getItem('devices') || '[]');
        if (devices.length > 0) {
            deviceMac = devices[0].mac;
            localStorage.setItem('selectedDeviceMac', deviceMac);
            tankId = tankId || devices[0].tankId;
        } else {
            // No devices available — show friendly placeholder state and disable controls
            document.getElementById('deviceName').textContent = 'No Device Selected';
            document.getElementById('deviceMac').textContent = '--:--:--:--:--:--';
            document.getElementById('tankId').textContent = '-';
            const badge = document.getElementById('statusBadge');
            badge.className = 'badge badge-offline';
            badge.textContent = 'No device';

            // Disable interactive elements
            document.querySelectorAll('button').forEach(b => b.setAttribute('disabled', 'true'));
            document.querySelectorAll('input, select').forEach(el => el.setAttribute('disabled', 'true'));

            // Adjust Apply button text
            const applyBtn = Array.from(document.querySelectorAll('button')).find(btn => btn.textContent.includes('Apply'));
            if (applyBtn) applyBtn.textContent = 'No Device';

            return;
        }
    }
    
    loadDeviceData();
    setupManualMode();
    
    // Request device data from hub
    if (window.ws && window.ws.readyState === WebSocket.OPEN) {
        sendCommand({
            type: 'getDevice',
            mac: deviceMac,
            tankId: tankId
        });
    }
});

function loadDeviceData() {
    // Load from localStorage
    const devices = JSON.parse(localStorage.getItem('devices') || '[]');
    deviceData = devices.find(d => d.mac === deviceMac);
    
    if (deviceData) {
        displayDeviceInfo();
    }
}

function displayDeviceInfo() {
    document.getElementById('deviceName').textContent = deviceData.name;
    document.getElementById('deviceMac').textContent = deviceData.mac;
    document.getElementById('tankId').textContent = deviceData.tankId;
    
    // Status badge
    const badge = document.getElementById('statusBadge');
    if (deviceData.online) {
        badge.className = 'badge badge-online';
        badge.textContent = 'Online';
    } else {
        badge.className = 'badge badge-offline';
        badge.textContent = 'Offline';
    }
    
    // Current state
    if (deviceData.state) {
        document.getElementById('whiteLevel').value = deviceData.state.white || 0;
        document.getElementById('blueLevel').value = deviceData.state.blue || 0;
        document.getElementById('redLevel').value = deviceData.state.red || 0;
        updateChannel('white', deviceData.state.white);
        updateChannel('blue', deviceData.state.blue);
        updateChannel('red', deviceData.state.red);
        updateColorPreview();
    }
    
    // Statistics
    if (deviceData.uptime) {
        document.getElementById('uptime').textContent = formatUptime(deviceData.uptime);
    }
    if (deviceData.lastUpdate) {
        document.getElementById('lastUpdate').textContent = formatTime(deviceData.lastUpdate);
    }
    document.getElementById('commandCount').textContent = deviceData.commandCount || 0;
    document.getElementById('health').textContent = (deviceData.health || 100) + '%';
}

function setupManualMode() {
    const checkbox = document.getElementById('manualMode');
    const ranges = ['whiteLevel', 'blueLevel', 'redLevel'];
    
    checkbox.addEventListener('change', () => {
        ranges.forEach(id => {
            document.getElementById(id).disabled = !checkbox.checked;
        });
    });
    
    // Disable by default
    ranges.forEach(id => {
        document.getElementById(id).disabled = true;
    });
}

function toggleLight() {
    const currentState = deviceData?.state?.enabled || false;
    const newState = !currentState;
    
    sendCommand({
        type: 'lightCommand',
        tankId: tankId,
        mac: deviceMac,
        action: newState ? 'turnOn' : 'turnOff'
    });
    
    // Update UI
    document.getElementById('powerIcon').textContent = newState ? '🔆' : '💡';
    document.getElementById('powerText').textContent = newState ? 'Turn Off' : 'Turn On';
    
    showNotification(newState ? 'Light turned on' : 'Light turned off', 'success');
}

function updateChannel(channel, value) {
    document.getElementById(`${channel}Value`).textContent = value;
    updateColorPreview();
}

function updateColorPreview() {
    const white = parseInt(document.getElementById('whiteLevel').value);
    const blue = parseInt(document.getElementById('blueLevel').value);
    const red = parseInt(document.getElementById('redLevel').value);
    
    // Convert percentages to RGB values
    const r = Math.round((white * 2.55) + (red * 2.55)) / 2;
    const g = Math.round(white * 2.55);
    const b = Math.round((white * 2.55) + (blue * 2.55)) / 2;
    
    const preview = document.getElementById('colorPreview');
    preview.style.background = `rgb(${r}, ${g}, ${b})`;
}

function applyManualSettings() {
    const white = parseInt(document.getElementById('whiteLevel').value);
    const blue = parseInt(document.getElementById('blueLevel').value);
    const red = parseInt(document.getElementById('redLevel').value);
    
    sendCommand({
        type: 'lightCommand',
        tankId: tankId,
        mac: deviceMac,
        action: 'setLevels',
        levels: { white, blue, red }
    });
    
    showNotification('Settings applied', 'success');
}

function applyPreset(presetName) {
    const presets = {
        morning: { white: 60, blue: 40, red: 20 },
        noon: { white: 100, blue: 80, red: 40 },
        evening: { white: 50, blue: 70, red: 60 },
        night: { white: 0, blue: 20, red: 10 },
        off: { white: 0, blue: 0, red: 0 }
    };
    
    const preset = presets[presetName];
    if (!preset) return;
    
    // Update sliders
    document.getElementById('whiteLevel').value = preset.white;
    document.getElementById('blueLevel').value = preset.blue;
    document.getElementById('redLevel').value = preset.red;
    
    updateChannel('white', preset.white);
    updateChannel('blue', preset.blue);
    updateChannel('red', preset.red);
    
    // Send command
    sendCommand({
        type: 'lightCommand',
        tankId: tankId,
        mac: deviceMac,
        action: 'setLevels',
        levels: preset
    });
    
    showNotification(`Applied ${presetName} preset`, 'success');
}

function formatUptime(seconds) {
    const days = Math.floor(seconds / 86400);
    const hours = Math.floor((seconds % 86400) / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    
    if (days > 0) return `${days}d ${hours}h`;
    if (hours > 0) return `${hours}h ${minutes}m`;
    return `${minutes}m`;
}

function formatTime(timestamp) {
    const now = Date.now();
    const diff = now - timestamp;
    const seconds = Math.floor(diff / 1000);
    
    if (seconds < 60) return 'Just now';
    if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
    if (seconds < 86400) return `${Math.floor(seconds / 3600)}h ago`;
    return `${Math.floor(seconds / 86400)}d ago`;
}

function showNotification(message, type) {
    const notification = document.createElement('div');
    notification.style.cssText = `
        position: fixed;
        top: 80px;
        right: 20px;
        padding: 1rem 1.5rem;
        background: ${type === 'success' ? 'var(--color-accent)' : 'var(--color-accent-danger)'};
        color: white;
        border-radius: var(--radius-md);
        box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
        z-index: 10000;
        animation: slideIn 0.3s ease;
    `;
    notification.textContent = message;
    
    document.body.appendChild(notification);
    
    setTimeout(() => {
        notification.style.animation = 'slideOut 0.3s ease';
        setTimeout(() => notification.remove(), 300);
    }, 3000);
}

// Handle WebSocket updates
function handleLightUpdate(data) {
    if (data.type === 'lightStatus' && data.mac === deviceMac) {
        deviceData = { ...deviceData, ...data };
        displayDeviceInfo();
    }
}

window.handleLightUpdate = handleLightUpdate;
