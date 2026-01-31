// Aquarium Devices Page JavaScript

let aquarium = null;
let allDevices = [];
let filteredDevices = [];

document.addEventListener('DOMContentLoaded', () => {
    // Get aquarium ID from URL
    const urlParams = new URLSearchParams(window.location.search);
    const tankId = urlParams.get('tankId') || urlParams.get('id');
    
    if (!tankId) {
        showError('No aquarium ID provided');
        return;
    }
    
    loadAquariumDetails(tankId);
    loadDevices(tankId);
    
    // Refresh on visibility change
    document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'visible') {
            loadAquariumDetails(tankId);
            loadDevices(tankId);
        }
    });
});

function loadAquariumDetails(tankId) {
    fetch('/api/aquariums')
        .then(response => response.json())
        .then(data => {
            if (data.aquariums && Array.isArray(data.aquariums)) {
                aquarium = data.aquariums.find(a => String(a.id) === String(tankId));
                if (aquarium) {
                    renderAquariumDetails();
                } else {
                    showError('Aquarium not found');
                }
            }
        })
        .catch(error => {
            console.error('Error loading aquarium:', error);
            showError('Failed to load aquarium details');
        });
}

function loadDevices(tankId) {
    fetch('/api/devices')
        .then(response => response.json())
        .then(data => {
            if (data.devices && Array.isArray(data.devices)) {
                // Filter devices by tankId
                allDevices = data.devices.filter(d => String(d.tankId) === String(tankId));
                filteredDevices = [...allDevices];
                renderDevices();
                updateOfflineWarning();
            }
        })
        .catch(error => {
            console.error('Error loading devices:', error);
            // Try localStorage fallback
            const stored = localStorage.getItem('devices');
            if (stored) {
                const devices = JSON.parse(stored);
                allDevices = devices.filter(d => String(d.tankId) === String(tankId));
                filteredDevices = [...allDevices];
                renderDevices();
                updateOfflineWarning();
            }
        });
}

function renderAquariumDetails() {
    if (!aquarium) return;
    
    document.getElementById('aquariumTitle').textContent = `${aquarium.name} - Devices`;
    document.getElementById('aquariumName').textContent = aquarium.name;
    document.getElementById('aquariumTankId').textContent = aquarium.id;
    document.getElementById('aquariumVolume').textContent = `${aquarium.volumeLiters}L`;
    document.getElementById('aquariumDeviceCount').textContent = allDevices.length;
    
    // Update status badge
    const statusBadge = document.getElementById('aquariumStatusBadge');
    if (aquarium.enabled) {
        statusBadge.textContent = 'Active';
        statusBadge.className = 'badge badge-online';
    } else {
        statusBadge.textContent = 'Disabled';
        statusBadge.className = 'badge badge-offline';
    }
    
    // Show location if available
    if (aquarium.location) {
        document.getElementById('aquariumLocation').style.display = 'block';
        document.getElementById('aquariumLocationText').textContent = aquarium.location;
    }
}

function updateOfflineWarning() {
    const offlineCount = allDevices.filter(d => !d.online).length;
    const warningDiv = document.getElementById('offlineWarning');
    const countSpan = document.getElementById('offlineDeviceCount');
    
    if (offlineCount > 0) {
        warningDiv.style.display = 'block';
        countSpan.textContent = offlineCount;
    } else {
        warningDiv.style.display = 'none';
    }
}

function filterDevices() {
    const statusFilter = document.getElementById('statusFilter').value;
    
    filteredDevices = allDevices.filter(device => {
        if (statusFilter === 'all') return true;
        if (statusFilter === 'online') return device.online;
        if (statusFilter === 'offline') return !device.online;
        return true;
    });
    
    renderDevices();
}

function showOfflineDevices() {
    document.getElementById('statusFilter').value = 'offline';
    filterDevices();
}

function renderDevices() {
    const grid = document.getElementById('devicesGrid');
    const emptyState = document.getElementById('emptyState');
    
    // Update device count in aquarium details
    document.getElementById('aquariumDeviceCount').textContent = allDevices.length;
    
    if (filteredDevices.length === 0) {
        grid.style.display = 'none';
        emptyState.style.display = 'block';
        return;
    }
    
    grid.style.display = 'grid';
    emptyState.style.display = 'none';
    
    grid.innerHTML = filteredDevices.map(device => {
        const icon = getDeviceIcon(device.type);
        const statusClass = device.online ? 'badge-online' : 'badge-offline';
        const statusText = device.online ? 'Online' : 'Offline';
        const deviceTypeLower = String(device.type || '').toLowerCase();
        
        return `
            <div class="card">
                <div class="card-header">
                    <div style="display: flex; align-items: center; gap: 0.5rem;">
                        <h3 style="margin: 0;">${icon} ${device.name}</h3>
                        <span class="badge badge-secondary">${getDeviceTypeName(device.type)}</span>
                    </div>
                    <span class="badge ${statusClass}">${statusText}</span>
                </div>
                <div class="card-body">
                    <div style="margin-bottom: 1rem;">
                        <div style="color: var(--color-text-secondary); font-size: 0.875rem;">
                            <strong>MAC:</strong> ${device.mac}
                        </div>
                        ${device.ip ? `
                            <div style="color: var(--color-text-secondary); font-size: 0.875rem;">
                                <strong>IP:</strong> ${device.ip}
                            </div>
                        ` : ''}
                    </div>
                    
                    ${device.online && device.lastSeen ? `
                        <div style="color: var(--color-text-secondary); font-size: 0.75rem;">
                            Last seen: ${formatTimestamp(device.lastSeen)}
                        </div>
                    ` : ''}
                </div>
                <div class="card-footer" style="display: flex; gap: 0.5rem;">
                    ${getDeviceActions(device)}
                </div>
            </div>
        `;
    }).join('');
}

function getDeviceIcon(type) {
    const icons = {
        'light': '💡',
        'heater': '🌡️',
        'feeder': '🐟',
        'co2': '🌿',
        'filter': '💧',
        'pump': '🔄',
        'sensor': '📊',
        'repeater': '📡'
    };
    return icons[String(type || '').toLowerCase()] || '🔌';
}

function getDeviceTypeName(type) {
    const names = {
        'light': 'Light',
        'heater': 'Heater',
        'feeder': 'Feeder',
        'co2': 'CO₂',
        'filter': 'Filter',
        'pump': 'Pump',
        'sensor': 'Sensor',
        'repeater': 'Repeater'
    };
    return names[String(type || '').toLowerCase()] || type || 'Unknown';
}

function getDeviceActions(device) {
    const deviceType = String(device.type || '').toLowerCase();
    
    switch(deviceType) {
        case 'light':
            return `
                <a href="../device/control/light-control.html?mac=${device.mac}" class="btn btn-primary" style="flex: 1;">
                    🎚️ Control
                </a>
                <a href="../device/schedule/light-schedule.html?mac=${device.mac}" class="btn btn-secondary" style="flex: 1;">
                    📅 Schedule
                </a>
            `;
        case 'heater':
            return `
                <button class="btn btn-primary" style="flex: 1;" onclick="controlHeater('${device.mac}')">
                    🌡️ Control
                </button>
            `;
        case 'feeder':
            return `
                <button class="btn btn-primary" style="flex: 1;" onclick="feedNow('${device.mac}')">
                    🐟 Feed Now
                </button>
            `;
        default:
            return `
                <button class="btn btn-secondary" style="flex: 1;" onclick="viewDeviceDetails('${device.mac}')">
                    ℹ️ Details
                </button>
            `;
    }
}

function formatTimestamp(timestamp) {
    if (!timestamp) return 'Unknown';
    const date = new Date(timestamp * 1000);
    return date.toLocaleString();
}

function editAquarium() {
    if (aquarium) {
        window.location.href = `manage-aquarium.html?id=${aquarium.id}`;
    }
}

function viewDeviceDetails(mac) {
    alert('Device details page coming soon!\nMAC: ' + mac);
}

function controlHeater(mac) {
    alert('Heater control page coming soon!\nMAC: ' + mac);
}

function feedNow(mac) {
    if (confirm('Feed fish now?')) {
        fetch('/api/feed', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ mac: mac })
        })
        .then(response => response.json())
        .then(data => {
            if (data.success) {
                alert('Feeding command sent!');
            } else {
                alert('Failed to send feeding command: ' + (data.error || 'Unknown error'));
            }
        })
        .catch(error => {
            console.error('Error:', error);
            alert('Failed to send feeding command');
        });
    }
}

function showError(message) {
    const grid = document.getElementById('devicesGrid');
    const emptyState = document.getElementById('emptyState');
    
    grid.style.display = 'none';
    emptyState.style.display = 'block';
    emptyState.innerHTML = `
        <div style="font-size: 4rem; margin-bottom: 1rem;">⚠️</div>
        <h2>Error</h2>
        <p style="color: var(--color-text-secondary); margin: 1rem 0 2rem;">
            ${message}
        </p>
        <a href="aquarium-selection.html" class="btn btn-primary">
            ← Back to Aquariums
        </a>
    `;
}
