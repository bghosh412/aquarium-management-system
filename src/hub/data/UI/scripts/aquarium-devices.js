// Aquarium Devices Page JavaScript

let currentTankId = null;
let currentAquarium = null;
let devices = [];
let currentFilter = 'all';

document.addEventListener('DOMContentLoaded', () => {
    // Get tank ID from URL
    const params = new URLSearchParams(window.location.search);
    currentTankId = parseInt(params.get('tankId')) || parseInt(localStorage.getItem('selectedTankId'));
    
    // If no tankId provided, try to pick the first aquarium from localStorage.
    if (!currentTankId) {
        const aquariums = JSON.parse(localStorage.getItem('aquariums') || '[]');
        if (aquariums.length > 0) {
            currentTankId = aquariums[0].tankId;
            localStorage.setItem('selectedTankId', currentTankId);
        } else {
            // No aquariums available — show friendly empty state and stop further loading
            document.getElementById('aquariumName').textContent = 'No Aquarium Selected';
            // Ensure devices empty state is visible
            document.getElementById('devicesGrid').style.display = 'none';
            const empty = document.getElementById('emptyState');
            if (empty) {
                empty.style.display = 'block';
                empty.querySelector('h2').textContent = 'No Aquarium Selected';
                empty.querySelector('p').textContent = 'Please add an aquarium first or select one from the Aquariums page.';
            }
            return;
        }
    }
    
    loadAquariumData();
    loadDevices();
    setupFilters();
    
    // Request data from hub
    if (window.ws && window.ws.readyState === WebSocket.OPEN) {
        sendCommand({
            type: 'getAquarium',
            tankId: currentTankId
        });
        sendCommand({
            type: 'getDevices',
            tankId: currentTankId
        });
    }
});

function loadAquariumData() {
    // Load from localStorage
    const aquariums = JSON.parse(localStorage.getItem('aquariums') || '[]');
    currentAquarium = aquariums.find(a => a.tankId === currentTankId);
    
    if (currentAquarium) {
        displayAquariumInfo();
    }
}

function displayAquariumInfo() {
    document.getElementById('aquariumName').textContent = currentAquarium.name;
    document.getElementById('tankId').textContent = currentAquarium.tankId;
    document.getElementById('volume').textContent = currentAquarium.volume;
    document.getElementById('location').textContent = currentAquarium.location || 'Not specified';
    
    // Display thresholds
    if (currentAquarium.thresholds) {
        const t = currentAquarium.thresholds;
        document.getElementById('tempRange').textContent = `${t.temperature.min}-${t.temperature.max}°C`;
        document.getElementById('phRange').textContent = `${t.ph.min}-${t.ph.max}`;
        document.getElementById('tdsRange').textContent = `${t.tds.min}-${t.tds.max} ppm`;
    }
}

function loadDevices() {
    // Try to load from localStorage
    const allDevices = JSON.parse(localStorage.getItem('devices') || '[]');
    devices = allDevices.filter(d => d.tankId === currentTankId);
    renderDevices();
}

function setupFilters() {
    const filterButtons = document.querySelectorAll('.device-filter');
    filterButtons.forEach(btn => {
        btn.addEventListener('click', () => {
            filterButtons.forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            currentFilter = btn.dataset.filter;
            renderDevices();
        });
    });
}

function renderDevices() {
    const grid = document.getElementById('devicesGrid');
    const emptyState = document.getElementById('emptyState');
    
    // Filter devices
    const filteredDevices = currentFilter === 'all' 
        ? devices 
        : devices.filter(d => d.type === currentFilter);
    
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
        
        return `
            <div class="card" style="cursor: pointer;" onclick="viewDeviceDetails('${device.mac}', '${device.type}')">
                <div class="card-header">
                    <h3>${icon} ${device.name}</h3>
                    <span class="badge ${statusClass}">${statusText}</span>
                </div>
                <div class="card-body">
                    <div style="color: var(--color-text-secondary); font-size: 0.875rem; margin-bottom: 0.5rem;">
                        MAC: ${device.mac}
                    </div>
                    ${getDeviceStatus(device)}
                </div>
                <div class="card-footer" style="display: flex; gap: 0.5rem;">
                    <button class="btn btn-primary" style="flex: 1;" onclick="event.stopPropagation(); controlDevice('${device.mac}')">
                        Control
                    </button>
                    <button class="btn btn-secondary" style="flex: 1;" onclick="event.stopPropagation(); viewSchedule('${device.mac}')">
                        Schedule
                    </button>
                </div>
            </div>
        `;
    }).join('');
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

function getDeviceStatus(device) {
    switch(device.type) {
        case 'light':
            return `
                <div style="display: flex; gap: 0.5rem; margin-top: 0.5rem;">
                    <span class="badge badge-online">W: ${device.state?.white || 0}%</span>
                    <span class="badge badge-online">B: ${device.state?.blue || 0}%</span>
                    <span class="badge badge-online">R: ${device.state?.red || 0}%</span>
                </div>
            `;
        case 'co2':
            return `<div style="margin-top: 0.5rem;">Status: ${device.state?.injecting ? 'Injecting' : 'Off'}</div>`;
        case 'heater':
            return `<div style="margin-top: 0.5rem;">Temp: ${device.state?.currentTemp || '--'}°C | Target: ${device.state?.targetTemp || '--'}°C</div>`;
        case 'sensor':
            return `
                <div style="margin-top: 0.5rem;">
                    <div>Temp: ${device.readings?.temperature || '--'}°C</div>
                    <div>pH: ${device.readings?.ph || '--'}</div>
                    <div>TDS: ${device.readings?.tds || '--'} ppm</div>
                </div>
            `;
        default:
            return '';
    }
}

function viewDeviceDetails(mac, type) {
    localStorage.setItem('selectedDeviceMac', mac);
    
    // Route to appropriate details page
    const detailsPages = {
        'light': 'details/light-details.html',
        'co2': 'details/co2-details.html',
        'heater': 'details/heater-details.html',
        'feeder': 'details/feeder-details.html',
        'sensor': 'details/sensor-details.html'
    };
    
    const page = detailsPages[type] || '#';
    if (page !== '#') {
        window.location.href = page + `?mac=${mac}&tankId=${currentTankId}`;
    }
}

function controlDevice(mac) {
    // Quick control modal or action
    console.log('Control device:', mac);
}

function viewSchedule(mac) {
    localStorage.setItem('selectedDeviceMac', mac);
    window.location.href = '../device/schedule/light-schedule.html?mac=' + mac;
}

// Handle WebSocket updates
function handleDeviceData(data) {
    if (data.type === 'deviceList' && data.tankId === currentTankId) {
        devices = data.devices;
        renderDevices();
    } else if (data.type === 'deviceUpdate') {
        const index = devices.findIndex(d => d.mac === data.device.mac);
        if (index !== -1) {
            devices[index] = { ...devices[index], ...data.device };
            renderDevices();
        }
    } else if (data.type === 'sensorReading' && data.tankId === currentTankId) {
        updateWaterParameters(data);
    }
}

function updateWaterParameters(data) {
    if (data.temperature !== undefined) {
        document.getElementById('currentTemp').textContent = data.temperature.toFixed(1) + '°C';
    }
    if (data.ph !== undefined) {
        document.getElementById('currentPh').textContent = data.ph.toFixed(1);
    }
    if (data.tds !== undefined) {
        document.getElementById('currentTds').textContent = data.tds + ' ppm';
    }
    
    // Update sensor status
    document.getElementById('sensorStatus').textContent = 'Live';
    document.getElementById('sensorStatus').className = 'badge badge-online';
}

window.handleDeviceData = handleDeviceData;
