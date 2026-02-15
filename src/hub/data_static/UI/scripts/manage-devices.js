// Manage Devices Page JavaScript

let allDevices = [];
let filteredDevices = [];
let selectedDevices = new Set();

document.addEventListener('DOMContentLoaded', () => {
    loadDevices();
    loadTankFilter();

    // Do not auto-refresh in background. Refresh on visibility/focus instead.
    document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'visible') {
            loadDevices();
        }
    });
    window.addEventListener('focus', () => loadDevices());
});

function loadDevices() {
    // Load from backend API
    fetch('/api/devices')
        .then(response => response.json())
        .then(data => {
            if (data.devices && Array.isArray(data.devices)) {
                allDevices = data.devices;
                // Store in localStorage as backup
                localStorage.setItem('devices', JSON.stringify(allDevices));
                filteredDevices = [...allDevices];
                updateStatistics();
                renderDevices();
            }
        })
        .catch(error => {
            console.error('Error loading devices:', error);
            // Fallback to localStorage
            allDevices = JSON.parse(localStorage.getItem('devices') || '[]');
            filteredDevices = [...allDevices];
            updateStatistics();
            renderDevices();
        });
}

function loadTankFilter() {
    // Load from backend API
    fetch('/api/aquariums')
        .then(response => response.json())
        .then(data => {
            if (data.aquariums && Array.isArray(data.aquariums)) {
                const tankFilter = document.getElementById('tankFilter');
                tankFilter.innerHTML = '<option value="all">All Tanks</option>';
                
                data.aquariums.forEach(tank => {
                    const option = document.createElement('option');
                    option.value = tank.id;
                    option.textContent = `Tank ${tank.id} - ${tank.name}`;
                    tankFilter.appendChild(option);
                });
            }
        })
        .catch(error => {
            console.error('Error loading tanks:', error);
            // Fallback to localStorage
            const aquariums = JSON.parse(localStorage.getItem('aquariums') || '[]');
            const tankFilter = document.getElementById('tankFilter');
            
            aquariums.forEach(tank => {
                const option = document.createElement('option');
                option.value = tank.id;
                option.textContent = `Tank ${tank.id} - ${tank.name}`;
                tankFilter.appendChild(option);
            });
        });
}

function filterDevices() {
    const tankFilter = document.getElementById('tankFilter').value;
    const typeFilter = document.getElementById('typeFilter').value;
    const statusFilter = document.getElementById('statusFilter').value;
    
    filteredDevices = allDevices.filter(device => {
        let matches = true;
        
            if (tankFilter !== 'all' && String(device.tankId) !== String(tankFilter)) {
            matches = false;
        }
        
        if (typeFilter !== 'all' && String(device.type || '').toLowerCase() !== String(typeFilter).toLowerCase()) {
            matches = false;
        }
        
        if (statusFilter !== 'all') {
            if (statusFilter === 'online' && !device.online) matches = false;
            if (statusFilter === 'offline' && device.online) matches = false;
            if (statusFilter === 'error' && device.hasError !== true) matches = false;
        }
        
        return matches;
    });
    
    renderDevices();
}

function updateStatistics() {
    const total = allDevices.length;
    const online = allDevices.filter(d => d.online).length;
    const offline = total - online;
    const scheduled = allDevices.filter(d => d.schedules && d.schedules.length > 0).length;
    
    document.getElementById('totalDevices').textContent = total;
    document.getElementById('onlineDevices').textContent = online;
    document.getElementById('offlineDevices').textContent = offline;
    document.getElementById('scheduledDevices').textContent = scheduled;
}

function renderDevices() {
    const grid = document.getElementById('devicesGrid');
    const emptyState = document.getElementById('emptyState');
    
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
        const isSelected = selectedDevices.has(device.mac);
        const deviceTypeLower = String(device.type || '').toLowerCase();
        const typeName = getDeviceTypeName(device.type);
        
        return `
            <div class="card device-card" style="${isSelected ? 'border: 2px solid var(--color-primary); box-shadow: 0 0 0 4px rgba(14, 165, 233, 0.1);' : ''}">
                <div class="card-header">
                    <div class="device-header-top">
                        <div class="device-name-container">
                            <input type="checkbox" class="checkbox" ${isSelected ? 'checked' : ''} 
                                   onchange="toggleDeviceSelection('${device.mac}', this.checked)">
                            <h3 class="device-name" title="${device.name}">${icon} ${device.name}</h3>
                        </div>
                        <span class="badge ${statusClass}">${statusText}</span>
                    </div>
                    <span class="badge badge-secondary device-type-badge">${typeName}</span>
                </div>
                <div class="card-body">
                    <div class="device-meta">
                        <span class="meta-label">MAC:</span>
                        <span class="meta-value">${device.mac}</span>
                        
                        <span class="meta-label">Tank:</span>
                        <span class="meta-value">${device.tankId}</span>
                    </div>
                    
                    ${device.schedules && device.schedules.length > 0 ? `
                        <div style="margin-top: 1rem;">
                            <span class="badge badge-online" style="font-size: 0.75rem;">
                                📅 ${device.schedules.length} Active Schedule${device.schedules.length > 1 ? 's' : ''}
                            </span>
                        </div>
                    ` : ''}
                </div>
                <div class="card-footer">
                    ${deviceTypeLower.includes('light') || deviceTypeLower.includes('feeder') || deviceTypeLower.includes('co2') || deviceTypeLower.includes('heater') || deviceTypeLower.includes('wave_maker') ? 
                        `<button class="device-action-btn btn-primary" onclick="viewDevice('${device.mac}')" title="View Schedules">
                            <i class="fas fa-calendar-alt"></i> Schedule
                        </button>` : ''}
                    <button class="device-action-btn btn-success" onclick="setupDevice('${device.mac}')" title="Configure Device">
                        <i class="fas fa-cog"></i> Setup
                    </button>
                    <button class="device-action-btn btn-warning" onclick="controlDevice('${device.mac}')" title="Manual Control">
                        <i class="fas fa-sliders-h"></i> Control
                    </button>
                </div>
            </div>
        `;
    }).join('');
    
    updateSelectedCount();
}

function getDeviceIcon(type) {
    const typeLower = String(type || '').toLowerCase();
    const icons = {
        'light': '💡',
        'co2': '🫧',
        'heater': '🔥',
        'feeder': '🐟',
        'sensor': '📊',
        'repeater': '📡',
        'wave_maker': '🌊'
    };
    return icons[typeLower] || '🔌';
}

function getDeviceTypeName(type) {
    const typeLower = String(type || '').toLowerCase();
    const names = {
        'light': 'Light Controller',
        'co2': 'CO₂ Regulator',
        'heater': 'Heater',
        'feeder': 'Fish Feeder',
        'sensor': 'Water Quality Sensor',
        'repeater': 'Repeater',
        'wave_maker': 'Wave Maker'
    };
    return names[typeLower] || 'Unknown';
}

function toggleDeviceSelection(mac, checked) {
    if (checked) {
        selectedDevices.add(mac);
    } else {
        selectedDevices.delete(mac);
    }
    updateSelectedCount();
    updateBulkButtons();
}

function updateSelectedCount() {
    document.getElementById('selectedCount').textContent = `${selectedDevices.size} selected`;
}

function updateBulkButtons() {
    const hasSelection = selectedDevices.size > 0;
    document.getElementById('bulkEnableBtn').disabled = !hasSelection;
    document.getElementById('bulkUnmapBtn').disabled = !hasSelection;
    document.getElementById('bulkDeleteBtn').disabled = !hasSelection;
}

function selectAll() {
    filteredDevices.forEach(device => selectedDevices.add(device.mac));
    renderDevices();
}

function deselectAll() {
    selectedDevices.clear();
    renderDevices();
}

function bulkEnable() {
    if (selectedDevices.size === 0) return;
    
    selectedDevices.forEach(mac => {
        sendCommand({
            type: 'enableDevice',
            mac: mac
        });
    });
    
    showNotification(`Enabled ${selectedDevices.size} device(s)`, 'success');
}

function bulkUnmap() {
    if (selectedDevices.size === 0) return;

    if (!confirm(`Are you sure you want to unmap ${selectedDevices.size} device(s)?\n\nThis will reset them to discovery mode.`)) {
        return;
    }

    const macsToUnmap = Array.from(selectedDevices);
    const requests = macsToUnmap.map(mac =>
        fetch('/api/unmap-device', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ mac })
        }).then(response => response.json())
    );

    Promise.all(requests)
        .then(() => {
            allDevices = allDevices.filter(d => !selectedDevices.has(d.mac));
            localStorage.setItem('devices', JSON.stringify(allDevices));
            selectedDevices.clear();
            filterDevices();
            updateStatistics();
            showNotification(`Unmapped ${macsToUnmap.length} device(s)`, 'success');
        })
        .catch(error => {
            console.error('Error unmapping devices:', error);
            showNotification('Failed to unmap selected devices', 'error');
        });
}

function bulkDelete() {
    if (selectedDevices.size === 0) return;
    
    if (!confirm(`Are you sure you want to delete ${selectedDevices.size} device(s)?\n\nThis will permanently remove devices and all schedules.`)) {
        return;
    }
    
    const macsToRemove = Array.from(selectedDevices);
    const requests = macsToRemove.map(mac =>
        fetch('/api/delete-device', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ mac })
        }).then(response => response.json())
    );

    Promise.all(requests)
        .then(() => {
            allDevices = allDevices.filter(d => !selectedDevices.has(d.mac));
            localStorage.setItem('devices', JSON.stringify(allDevices));
            selectedDevices.clear();
            filterDevices();
            updateStatistics();
            showNotification(`Deleted ${macsToRemove.length} device(s)`, 'success');
        })
        .catch(error => {
            console.error('Error deleting devices:', error);
            showNotification('Failed to delete selected devices', 'error');
        });
}

function viewDevice(mac) {
    const device = allDevices.find(d => d.mac === mac);
    if (!device) return;
    
    localStorage.setItem('selectedDeviceMac', mac);
    
    const deviceType = String(device.type || '').toLowerCase();

    if (deviceType === 'light' || deviceType === 'lighting' || deviceType === 'lights') {
        window.location.href = `schedule/light-schedule.html?mac=${mac}`;
        return;
    }

    // Feeder: open feeder schedule from device card
    if (deviceType.includes('feeder')) {
        window.location.href = `schedule/feeder-schedule.html?mac=${mac}`;
        return;
    }

    // Wave Maker: open wavemaker schedule
    if (deviceType.includes('wave_maker')) {
        window.location.href = `schedule/wm-schedule.html?mac=${mac}`;
        return;
    }

    // Route to appropriate details page
    const detailsPages = {
        'co2': '../aquarium/details/co2-details.html',
        'heater': '../aquarium/details/heater-details.html',
        'feeder': '../aquarium/details/feeder-details.html',
        'sensor': '../aquarium/details/sensor-details.html'
    };
    
    const page = detailsPages[deviceType] || 'device-setup.html';
    window.location.href = page + `?mac=${mac}`;
}

function setupDevice(mac) {
    localStorage.setItem('selectedDeviceMac', mac);
    window.location.href = `device-setup.html?mac=${mac}`;
}

function controlDevice(mac) {
    const device = allDevices.find(d => d.mac === mac);
    if (!device) return;

    const deviceType = String(device.type || '').toLowerCase();
    if (deviceType === 'light' || deviceType === 'lighting' || deviceType === 'lights') {
        localStorage.setItem('selectedDeviceMac', mac);
        window.location.href = `control/light-control.html?mac=${mac}`;
        return;
    }

    // Feeder: open calibration/control page from device card
    if (deviceType.includes('feeder')) {
        localStorage.setItem('selectedDeviceMac', mac);
        window.location.href = `calibration/feeder-calibration.html?mac=${mac}`;
        return;
    }

    // Wave Maker: open wavemaker control page
    if (deviceType.includes('wave_maker')) {
        localStorage.setItem('selectedDeviceMac', mac);
        window.location.href = `control/wm-control.html?mac=${mac}`;
        return;
    }

    showNotification('Control is only available for light, feeder, and wave maker devices.', 'error');
}

function unmapDevice(mac, name) {
    if (!confirm(`Are you sure you want to unmap "${name}"?\n\nThis will:\n• Remove device from hub registry\n• Reset device to discovery mode\n• Device will start announcing again\n\nThis action cannot be undone.`)) {
        return;
    }
    
    showNotification('Unmapping device...', 'info');
    
    fetch('/api/unmap-device', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({ mac: mac })
    })
    .then(response => response.json())
    .then(data => {
        if (data.success) {
            showNotification(`Device "${name}" unmapped successfully!`, 'success');
            
            // Remove from local list
            allDevices = allDevices.filter(d => d.mac !== mac);
            localStorage.setItem('devices', JSON.stringify(allDevices));
            
            // Reload device list
            setTimeout(() => {
                loadDevices();
            }, 500);
        } else {
            showNotification('Error unmapping device: ' + (data.error || 'Unknown error'), 'error');
        }
    })
    .catch(error => {
        console.error('Error unmapping device:', error);
        showNotification('Failed to unmap device. Check console for details.', 'error');
    });
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

// Handle WebSocket device updates
function handleDeviceListUpdate(data) {
    if (data.type === 'deviceList') {
        allDevices = data.devices;
        localStorage.setItem('devices', JSON.stringify(allDevices));
        filterDevices();
        updateStatistics();
    }
}

window.handleDeviceListUpdate = handleDeviceListUpdate;
