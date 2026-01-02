// Device Setup Page JavaScript

let deviceMac = null;
let deviceData = null;

document.addEventListener('DOMContentLoaded', () => {
    const params = new URLSearchParams(window.location.search);
    deviceMac = params.get('mac') || localStorage.getItem('selectedDeviceMac');
    
    if (!deviceMac) {
        // Try to pick the first device from localStorage
        const devices = JSON.parse(localStorage.getItem('devices') || '[]');
        if (devices.length > 0) {
            deviceMac = devices[0].mac;
            localStorage.setItem('selectedDeviceMac', deviceMac);
        } else {
            // No devices available — load tank options but show friendly placeholder and disable controls
            loadTankOptions();

            document.getElementById('deviceName').textContent = 'No Device Selected';
            document.getElementById('deviceMac').textContent = '--:--:--:--:--:--';
            document.getElementById('deviceType').textContent = 'N/A';
            const badge = document.getElementById('statusBadge');
            badge.className = 'badge badge-offline';
            badge.textContent = 'No device';

            // Disable interactive elements
            document.querySelectorAll('button').forEach(b => b.setAttribute('disabled', 'true'));
            document.querySelectorAll('input, select').forEach(el => el.setAttribute('disabled', 'true'));

            // Provide an inline CTA to add a device
            const pageHeader = document.querySelector('.page-header');
            if (pageHeader) {
                const cta = document.createElement('div');
                cta.style.margin = '1rem 0';
                cta.innerHTML = `<a href="add-device.html" class="btn btn-primary"><span>➕</span> Add Your First Device</a>`;
                pageHeader.parentNode.insertBefore(cta, pageHeader.nextSibling);
            }

            return;
        }
    }
    
    loadDeviceData();
    loadTankOptions();
    setupForm();
    
    // Request device data from hub
    if (window.ws && window.ws.readyState === WebSocket.OPEN) {
        sendCommand({
            type: 'getDevice',
            mac: deviceMac
        });
    }
});

function loadDeviceData() {
    const devices = JSON.parse(localStorage.getItem('devices') || '[]');
    deviceData = devices.find(d => d.mac === deviceMac);
    
    if (deviceData) {
        displayDeviceInfo();
    }
}

function displayDeviceInfo() {
    document.getElementById('deviceName').textContent = deviceData.name;
    document.getElementById('deviceMac').textContent = deviceData.mac;
    document.getElementById('deviceType').textContent = getDeviceTypeName(deviceData.type);
    
    // Status badge
    const badge = document.getElementById('statusBadge');
    if (deviceData.online) {
        badge.className = 'badge badge-online';
        badge.textContent = 'Online';
    } else {
        badge.className = 'badge badge-offline';
        badge.textContent = 'Offline';
    }
    
    // Fill form fields
    document.getElementById('editDeviceName').value = deviceData.name;
    document.getElementById('tankId').value = deviceData.tankId;
    document.getElementById('deviceEnabled').checked = deviceData.enabled !== false;
    
    if (deviceData.heartbeatInterval) {
        document.getElementById('heartbeatInterval').value = deviceData.heartbeatInterval;
        updateHeartbeat(deviceData.heartbeatInterval);
    }
    
    if (deviceData.maxRetries !== undefined) {
        document.getElementById('maxRetries').value = deviceData.maxRetries;
    }
    
    if (deviceData.failSafeMode) {
        document.getElementById('failSafeMode').value = deviceData.failSafeMode;
    }
    
    // Statistics
    updateStatistics();
}

function updateStatistics() {
    if (deviceData.uptime) {
        document.getElementById('uptime').textContent = formatUptime(deviceData.uptime);
    }
    if (deviceData.commandCount !== undefined) {
        document.getElementById('commandCount').textContent = deviceData.commandCount;
    }
    if (deviceData.successRate !== undefined) {
        document.getElementById('successRate').textContent = deviceData.successRate + '%';
    }
    if (deviceData.lastHeartbeat) {
        document.getElementById('lastHeartbeat').textContent = formatTime(deviceData.lastHeartbeat);
    }
    if (deviceData.firmwareVersion) {
        document.getElementById('firmwareVersion').textContent = deviceData.firmwareVersion;
    }
    if (deviceData.health !== undefined) {
        document.getElementById('health').textContent = deviceData.health + '%';
    }
}

function loadTankOptions() {
    const aquariums = JSON.parse(localStorage.getItem('aquariums') || '[]');
    const tankSelect = document.getElementById('tankId');
    
    tankSelect.innerHTML = '<option value="">Select aquarium...</option>';
    aquariums.forEach(tank => {
        const option = document.createElement('option');
        option.value = tank.tankId;
        option.textContent = `Tank ${tank.tankId} - ${tank.name}`;
        tankSelect.appendChild(option);
    });
}

function setupForm() {
    const form = document.getElementById('deviceSetupForm');
    form.addEventListener('submit', (e) => {
        e.preventDefault();
        saveDeviceSettings();
    });
}

function saveDeviceSettings() {
    const updatedDevice = {
        ...deviceData,
        name: document.getElementById('editDeviceName').value,
        tankId: parseInt(document.getElementById('tankId').value),
        enabled: document.getElementById('deviceEnabled').checked,
        heartbeatInterval: parseInt(document.getElementById('heartbeatInterval').value),
        maxRetries: parseInt(document.getElementById('maxRetries').value),
        failSafeMode: document.getElementById('failSafeMode').value
    };
    
    // Send to hub
    if (window.ws && window.ws.readyState === WebSocket.OPEN) {
        sendCommand({
            type: 'updateDevice',
            device: updatedDevice
        });
    }
    
    // Update localStorage
    const devices = JSON.parse(localStorage.getItem('devices') || '[]');
    const index = devices.findIndex(d => d.mac === deviceMac);
    if (index !== -1) {
        devices[index] = updatedDevice;
        localStorage.setItem('devices', JSON.stringify(devices));
    }
    
    deviceData = updatedDevice;
    showNotification('Settings saved successfully', 'success');
}

function updateHeartbeat(value) {
    document.getElementById('heartbeatValue').textContent = value;
}

function testCommunication() {
    const resultDiv = document.getElementById('testResult');
    resultDiv.style.display = 'block';
    resultDiv.innerHTML = '<div style="color: var(--color-primary);">Testing communication...</div>';
    
    // Send test command
    if (window.ws && window.ws.readyState === WebSocket.OPEN) {
        sendCommand({
            type: 'testDevice',
            mac: deviceMac
        });
        
        // Wait for response (timeout after 5 seconds)
        setTimeout(() => {
            if (resultDiv.innerHTML.includes('Testing')) {
                resultDiv.innerHTML = '<div style="color: var(--color-accent-danger);">❌ Test failed - No response</div>';
            }
        }, 5000);
    } else {
        resultDiv.innerHTML = '<div style="color: var(--color-accent-danger);">❌ WebSocket not connected</div>';
    }
}

function handleTestResponse(data) {
    if (data.mac === deviceMac) {
        const resultDiv = document.getElementById('testResult');
        if (data.success) {
            resultDiv.innerHTML = '<div style="color: var(--color-accent);">✓ Test successful - Device responded</div>';
        } else {
            resultDiv.innerHTML = '<div style="color: var(--color-accent-danger);">❌ Test failed - ' + data.error + '</div>';
        }
    }
}

function triggerFailSafe() {
    if (!confirm('This will trigger the fail-safe behavior on the device. Continue?')) {
        return;
    }
    
    sendCommand({
        type: 'triggerFailSafe',
        mac: deviceMac
    });
    
    showNotification('Fail-safe triggered', 'success');
}

function deleteDevice() {
    if (!confirm('Are you sure you want to delete this device? This cannot be undone.')) {
        return;
    }
    
    // Send to hub
    if (window.ws && window.ws.readyState === WebSocket.OPEN) {
        sendCommand({
            type: 'deleteDevice',
            mac: deviceMac
        });
    }
    
    // Remove from localStorage
    const devices = JSON.parse(localStorage.getItem('devices') || '[]');
    const filtered = devices.filter(d => d.mac !== deviceMac);
    localStorage.setItem('devices', JSON.stringify(filtered));
    
    showNotification('Device deleted', 'success');
    
    setTimeout(() => {
        window.location.href = 'manage-devices.html';
    }, 1500);
}

function resetStatistics() {
    if (!confirm('Reset all statistics for this device?')) {
        return;
    }
    
    sendCommand({
        type: 'resetDeviceStats',
        mac: deviceMac
    });
    
    showNotification('Statistics reset', 'success');
}

function exportDiagnostics() {
    const diagnostics = {
        device: deviceData,
        timestamp: new Date().toISOString(),
        statistics: {
            uptime: deviceData.uptime,
            commandCount: deviceData.commandCount,
            successRate: deviceData.successRate,
            lastHeartbeat: deviceData.lastHeartbeat
        }
    };
    
    const blob = new Blob([JSON.stringify(diagnostics, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `diagnostics_${deviceMac}_${Date.now()}.json`;
    a.click();
    URL.revokeObjectURL(url);
    
    showNotification('Diagnostics exported', 'success');
}

function getDeviceTypeName(type) {
    const names = {
        'light': 'Light Controller',
        'co2': 'CO₂ Regulator',
        'heater': 'Heater',
        'feeder': 'Fish Feeder',
        'sensor': 'Water Quality Sensor',
        'repeater': 'Repeater'
    };
    return names[type] || 'Unknown';
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

window.handleTestResponse = handleTestResponse;
