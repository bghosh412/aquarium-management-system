// Manage Devices Page JavaScript

let allDevices = [];
let filteredDevices = [];
let selectedDevices = new Set();

document.addEventListener('DOMContentLoaded', () => {
    loadDevices();
    loadTankFilter();
    
    // Request device list from hub
    if (window.ws && window.ws.readyState === WebSocket.OPEN) {
        sendCommand({ type: 'getAllDevices' });
    }
});

function loadDevices() {
    // Load from localStorage
    allDevices = JSON.parse(localStorage.getItem('devices') || '[]');
    filteredDevices = [...allDevices];
    updateStatistics();
    renderDevices();
}

function loadTankFilter() {
    const aquariums = JSON.parse(localStorage.getItem('aquariums') || '[]');
    const tankFilter = document.getElementById('tankFilter');
    
    aquariums.forEach(tank => {
        const option = document.createElement('option');
        option.value = tank.tankId;
        option.textContent = `Tank ${tank.tankId} - ${tank.name}`;
        tankFilter.appendChild(option);
    });
}

function filterDevices() {
    const tankFilter = document.getElementById('tankFilter').value;
    const typeFilter = document.getElementById('typeFilter').value;
    const statusFilter = document.getElementById('statusFilter').value;
    
    filteredDevices = allDevices.filter(device => {
        let matches = true;
        
        if (tankFilter !== 'all' && device.tankId !== parseInt(tankFilter)) {
            matches = false;
        }
        
        if (typeFilter !== 'all' && device.type !== typeFilter) {
            matches = false;
        }
        
        if (statusFilter !== 'all') {
            if (statusFilter === 'online' && !device.online) matches = false;
            if (statusFilter === 'offline' && device.online) matches = false;
            if (statusFilter === 'error' && !device.hasError) matches = false;
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
        
        return `
            <div class="card" style="${isSelected ? 'border: 2px solid var(--color-primary);' : ''}">
                <div class="card-header">
                    <div style="display: flex; align-items: center; gap: 0.5rem;">
                        <input type="checkbox" class="checkbox" ${isSelected ? 'checked' : ''} 
                               onchange="toggleDeviceSelection('${device.mac}', this.checked)">
                        <h3 style="margin: 0;">${icon} ${device.name}</h3>
                    </div>
                    <span class="badge ${statusClass}">${statusText}</span>
                </div>
                <div class="card-body">
                    <div style="margin-bottom: 1rem;">
                        <div style="color: var(--color-text-secondary); font-size: 0.875rem;">
                            <strong>MAC:</strong> ${device.mac}
                        </div>
                        <div style="color: var(--color-text-secondary); font-size: 0.875rem;">
                            <strong>Tank:</strong> ${device.tankId}
                        </div>
                        <div style="color: var(--color-text-secondary); font-size: 0.875rem;">
                            <strong>Type:</strong> ${getDeviceTypeName(device.type)}
                        </div>
                    </div>
                    
                    ${device.schedules && device.schedules.length > 0 ? `
                        <div style="margin-top: 0.5rem;">
                            <span class="badge badge-online">📅 ${device.schedules.length} schedule(s)</span>
                        </div>
                    ` : ''}
                </div>
                <div class="card-footer" style="display: flex; gap: 0.5rem;">
                    <button class="btn btn-primary" style="flex: 1;" onclick="viewDevice('${device.mac}')">
                        🔌 Control
                    </button>
                    <button class="btn btn-secondary" style="flex: 1;" onclick="setupDevice('${device.mac}')">
                        ⚙️ Setup
                    </button>
                </div>
            </div>
        `;
    }).join('');
    
    updateSelectedCount();
}

function getDeviceIcon(type) {
    const icons = {
        'light': '💡',
        'co2': '🫧',
        'heater': '🔥',
        'feeder': '🐟',
        'sensor': '📊',
        'repeater': '📡'
    };
    return icons[type] || '🔌';
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
    document.getElementById('bulkDisableBtn').disabled = !hasSelection;
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

function bulkDisable() {
    if (selectedDevices.size === 0) return;
    
    selectedDevices.forEach(mac => {
        sendCommand({
            type: 'disableDevice',
            mac: mac
        });
    });
    
    showNotification(`Disabled ${selectedDevices.size} device(s)`, 'success');
}

function bulkDelete() {
    if (selectedDevices.size === 0) return;
    
    if (!confirm(`Are you sure you want to delete ${selectedDevices.size} device(s)? This cannot be undone.`)) {
        return;
    }
    
    selectedDevices.forEach(mac => {
        sendCommand({
            type: 'deleteDevice',
            mac: mac
        });
        
        // Remove from local storage
        allDevices = allDevices.filter(d => d.mac !== mac);
    });
    
    localStorage.setItem('devices', JSON.stringify(allDevices));
    selectedDevices.clear();
    filterDevices();
    updateStatistics();
    
    showNotification(`Deleted ${selectedDevices.size} device(s)`, 'success');
}

function viewDevice(mac) {
    const device = allDevices.find(d => d.mac === mac);
    if (!device) return;
    
    localStorage.setItem('selectedDeviceMac', mac);
    
    // Route to appropriate details page
    const detailsPages = {
        'light': '../aquarium/details/light-details.html',
        'co2': '../aquarium/details/co2-details.html',
        'heater': '../aquarium/details/heater-details.html',
        'feeder': '../aquarium/details/feeder-details.html',
        'sensor': '../aquarium/details/sensor-details.html'
    };
    
    const page = detailsPages[device.type] || 'device-setup.html';
    window.location.href = page + `?mac=${mac}`;
}

function setupDevice(mac) {
    localStorage.setItem('selectedDeviceMac', mac);
    window.location.href = `device-setup.html?mac=${mac}`;
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
