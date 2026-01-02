// Add Device Page JavaScript

let discoveredDevices = [];
let pollInterval = null;

document.addEventListener('DOMContentLoaded', () => {
    loadTankOptions();
    setupForm();
    startDiscoveryPolling();
});

function loadTankOptions() {
    // Load from API
    fetch('/api/aquariums')
        .then(response => response.json())
        .then(data => {
            const tankSelect = document.getElementById('tankId');
            tankSelect.innerHTML = '<option value="">Select aquarium...</option>';
            
            if (data.aquariums && data.aquariums.length > 0) {
                data.aquariums.forEach(tank => {
                    const option = document.createElement('option');
                    option.value = tank.tankId;
                    option.textContent = `Tank ${tank.tankId} - ${tank.name}`;
                    tankSelect.appendChild(option);
                });
            }
        })
        .catch(error => {
            console.error('Error loading tanks:', error);
            showNotification('Error loading aquarium list', 'error');
        });
}

function setupForm() {
    const form = document.getElementById('addDeviceForm');
    
    form.addEventListener('submit', (e) => {
        e.preventDefault();
        addDeviceManually();
    });
    
    // Update preview as user types
    document.getElementById('deviceName').addEventListener('input', updatePreview);
    document.getElementById('deviceType').addEventListener('change', updatePreview);
}

function updateDeviceIcon() {
    updatePreview();
}

function updatePreview() {
    const name = document.getElementById('deviceName').value;
    const type = document.getElementById('deviceType').value;
    const preview = document.getElementById('devicePreview');
    
    if (name && type) {
        preview.style.display = 'block';
        document.getElementById('previewIcon').textContent = getDeviceIcon(type);
        document.getElementById('previewName').textContent = name;
        document.getElementById('previewType').textContent = getDeviceTypeName(type);
    } else {
        preview.style.display = 'none';
    }
}

function getDeviceIcon(type) {
    const icons = {
        'LIGHT': '💡',
        'CO2': '🫧',
        'HEATER': '🔥',
        'FISH_FEEDER': '🐟',
        'SENSOR': '📊',
        'REPEATER': '📡',
        // Legacy lowercase support
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
        'LIGHT': 'Light Controller',
        'CO2': 'CO₂ Regulator',
        'HEATER': 'Heater',
        'FISH_FEEDER': 'Fish Feeder',
        'SENSOR': 'Water Quality Sensor',
        'REPEATER': 'Repeater',
        // Legacy lowercase support
        'light': 'Light Controller',
        'co2': 'CO₂ Regulator',
        'heater': 'Heater',
        'feeder': 'Fish Feeder',
        'sensor': 'Water Quality Sensor',
        'repeater': 'Repeater'
    };
    return names[type] || 'Unknown';
}

function startDiscoveryPolling() {
    // Update status
    document.getElementById('discoveryStatus').textContent = 'Listening';
    document.getElementById('discoveryStatus').className = 'badge badge-online';
    
    // Fetch unmapped devices immediately
    fetchUnmappedDevices();
    
    // Poll every 5 seconds
    pollInterval = setInterval(fetchUnmappedDevices, 5000);
    
    console.log('Started polling for unmapped devices');
}

function stopDiscoveryPolling() {
    if (pollInterval) {
        clearInterval(pollInterval);
        pollInterval = null;
    }
    
    document.getElementById('discoveryStatus').textContent = 'Stopped';
    document.getElementById('discoveryStatus').className = 'badge badge-offline';
}

function fetchUnmappedDevices() {
    fetch('/api/unmapped-devices')
        .then(response => response.json())
        .then(data => {
            if (data.unmappedDevices && Array.isArray(data.unmappedDevices)) {
                // Clear existing display
                const container = document.getElementById('discoveredDevices');
                const existingCards = container.querySelectorAll('.device-card');
                const existingMacs = Array.from(existingCards).map(card => card.dataset.mac);
                
                // Update discovered devices array
                discoveredDevices = data.unmappedDevices;
                
                // Display new devices that aren't already shown
                discoveredDevices.forEach(device => {
                    if (!existingMacs.includes(device.mac)) {
                        displayDiscoveredDevice(device);
                    }
                });
                
                // Update count
                const count = discoveredDevices.length;
                document.getElementById('discoveryStatus').textContent = 
                    count > 0 ? `Found ${count} device${count !== 1 ? 's' : ''}` : 'Listening';
            }
        })
        .catch(error => {
            console.error('Error fetching unmapped devices:', error);
        });
}

function displayDiscoveredDevice(device) {
    const container = document.getElementById('discoveredDevices');
    
    const deviceCard = document.createElement('div');
    deviceCard.className = 'device-card';
    deviceCard.dataset.mac = device.mac;
    deviceCard.style.cssText = `
        padding: 1rem;
        margin-top: 1rem;
        background: white;
        border: 2px solid var(--color-accent);
        border-radius: var(--radius-md);
        display: flex;
        justify-content: space-between;
        align-items: center;
        gap: 1rem;
        flex-wrap: wrap;
        animation: slideIn 0.3s ease;
    `;
    
    // Format timestamp
    const discoveredAt = new Date(device.discoveredAt).toLocaleString();
    const announceCount = device.announceCount || 1;
    
    deviceCard.innerHTML = `
        <div style="flex: 1;">
            <div style="font-size: 1.5rem; margin-bottom: 0.25rem;">${getDeviceIcon(device.type)}</div>
            <div style="font-weight: 600;">${getDeviceTypeName(device.type)}</div>
            <div style="font-size: 0.875rem; color: var(--color-text-secondary); margin-top: 0.25rem;">
                MAC: <code>${device.mac}</code>
            </div>
            <div style="font-size: 0.75rem; color: var(--color-text-secondary); margin-top: 0.25rem;">
                Firmware: v${device.firmwareVersion} | 
                Discovered: ${discoveredAt} | 
                Announces: ${announceCount}
            </div>
        </div>
        <button class="btn btn-primary" onclick="registerDiscoveredDevice('${device.mac}')">
            <span>✓</span> Provision
        </button>
    `;
    
    container.appendChild(deviceCard);
}

function registerDiscoveredDevice(mac) {
    const device = discoveredDevices.find(d => d.mac === mac);
    if (!device) return;
    
    // Pre-fill form with device info
    document.getElementById('deviceMac').value = device.mac;
    
    // Map type to lowercase for form select (if needed)
    const typeMapping = {
        'LIGHT': 'light',
        'CO2': 'co2',
        'HEATER': 'heater',
        'FISH_FEEDER': 'feeder',
        'SENSOR': 'sensor',
        'REPEATER': 'repeater'
    };
    const formType = typeMapping[device.type] || device.type.toLowerCase();
    document.getElementById('deviceType').value = formType;
    
    // Set a default name
    document.getElementById('deviceName').value = `${getDeviceTypeName(device.type)} ${device.mac.slice(-5)}`;
    
    updatePreview();
    
    // Scroll to form
    document.getElementById('addDeviceForm').scrollIntoView({ behavior: 'smooth' });
    
    showNotification('Device information pre-filled. Please assign a name and tank.', 'success');
}

function addDeviceManually() {
    const deviceName = document.getElementById('deviceName').value;
    const deviceMac = document.getElementById('deviceMac').value.toUpperCase();
    const deviceType = document.getElementById('deviceType').value;
    const tankId = parseInt(document.getElementById('tankId').value);
    
    // Validation
    if (!deviceName || !deviceMac || !deviceType || !tankId) {
        showNotification('Please fill in all required fields', 'error');
        return;
    }
    
    // Validate MAC address format
    const macRegex = /^([A-F0-9]{2}:){5}[A-F0-9]{2}$/;
    if (!macRegex.test(deviceMac)) {
        showNotification('Invalid MAC address format. Use AA:BB:CC:DD:EE:FF', 'error');
        return;
    }
    
    // Check if this MAC is in discovered devices
    const discoveredDevice = discoveredDevices.find(d => d.mac === deviceMac);
    
    if (!discoveredDevice) {
        showNotification('Device MAC not found in discovered devices. Discovery required first.', 'error');
        return;
    }
    
    // Send provision request to hub
    const provisionData = {
        mac: deviceMac,
        name: deviceName,
        tankId: tankId
    };
    
    fetch('/api/provision-device', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(provisionData)
    })
    .then(response => response.json())
    .then(data => {
        if (data.success) {
            showNotification('Device provisioned successfully!', 'success');
            
            // Remove from discovered devices display
            const container = document.getElementById('discoveredDevices');
            const deviceCard = container.querySelector(`[data-mac="${deviceMac}"]`);
            if (deviceCard) {
                deviceCard.style.animation = 'slideOut 0.3s ease';
                setTimeout(() => deviceCard.remove(), 300);
            }
            
            // Clear form
            document.getElementById('addDeviceForm').reset();
            updatePreview();
            
            // Redirect to manage devices after delay
            setTimeout(() => {
                window.location.href = 'manage-devices.html';
            }, 1500);
        } else {
            showNotification('Error provisioning device: ' + (data.error || 'Unknown error'), 'error');
        }
    })
    .catch(error => {
        console.error('Error provisioning device:', error);
        showNotification('Failed to provision device. Check console for details.', 'error');
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
        font-weight: 500;
    `;
    notification.textContent = message;
    
    document.body.appendChild(notification);
    
    setTimeout(() => {
        notification.style.animation = 'slideOut 0.3s ease';
        setTimeout(() => notification.remove(), 300);
    }, 3000);
}

// Cleanup on page unload
window.addEventListener('beforeunload', () => {
    stopDiscoveryPolling();
});

