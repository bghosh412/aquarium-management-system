// Aquarium Selection Page JavaScript

let aquariums = [];

document.addEventListener('DOMContentLoaded', () => {
    loadAquariums();
});

// Handle WebSocket messages
function handleAquariumData(data) {
    if (data.type === 'aquariumList') {
        aquariums = data.aquariums || [];
        renderAquariums();
    }
}

function loadAquariums() {
    // Load from API
    fetch('/api/aquariums')
        .then(response => response.json())
        .then(data => {
            if (data.aquariums && Array.isArray(data.aquariums)) {
                aquariums = data.aquariums;
                // Store in localStorage as backup
                localStorage.setItem('aquariums', JSON.stringify(aquariums));
                renderAquariums();
            }
        })
        .catch(error => {
            console.error('Error loading aquariums:', error);
            // Fallback to localStorage
            const stored = localStorage.getItem('aquariums');
            if (stored) {
                aquariums = JSON.parse(stored);
                renderAquariums();
            }
        });
}

function renderAquariums() {
    const grid = document.getElementById('aquariumGrid');
    const emptyState = document.getElementById('emptyState');
    
    if (aquariums.length === 0) {
        grid.style.display = 'none';
        emptyState.style.display = 'block';
        return;
    }
    
    grid.style.display = 'grid';
    emptyState.style.display = 'none';
    
    grid.innerHTML = aquariums.map(aquarium => `
        <div class="card device-card" style="cursor: pointer;" onclick="selectAquarium(${aquarium.id})">
            <div class="card-header">
                <div class="device-header-top">
                    <div class="device-name-container">
                        <h3 class="device-name">🐠 ${aquarium.name}</h3>
                    </div>
                    <span class="badge ${aquarium.enabled ? 'badge-online' : 'badge-offline'}">
                        ${aquarium.enabled ? 'Active' : 'Offline'}
                    </span>
                </div>
                <span class="badge badge-secondary device-type-badge">Aquarium</span>
            </div>
            <div class="card-body">
                <div class="device-meta">
                    <span class="meta-label">ID:</span>
                    <span class="meta-value">${aquarium.id}</span>
                    
                    <span class="meta-label">Volume:</span>
                    <span class="meta-value">${aquarium.volumeLiters}L</span>
                    
                    ${aquarium.location ? `
                        <span class="meta-label">Location:</span>
                        <span class="meta-value">${aquarium.location}</span>
                    ` : ''}

                    <span class="meta-label">Devices:</span>
                    <span class="meta-value">${aquarium.deviceCount || 0} online</span>
                </div>
            </div>
            <div class="card-footer">
                <button class="device-action-btn btn-primary" onclick="event.stopPropagation(); viewDevices(${aquarium.id})">
                    <i class="fas fa-plug"></i> Devices
                </button>
                <button class="device-action-btn btn-success" onclick="event.stopPropagation(); manageAquarium(${aquarium.id})">
                    <i class="fas fa-cog"></i> Settings
                </button>
            </div>
        </div>
    `).join('');
}

function selectAquarium(tankId) {
    localStorage.setItem('selectedTankId', tankId);
    window.location.href = `aquarium-devices.html?tankId=${tankId}`;
}

function viewDevices(tankId) {
    localStorage.setItem('selectedTankId', tankId);
    window.location.href = `aquarium-devices.html?tankId=${tankId}`;
}

function manageAquarium(tankId) {
    localStorage.setItem('selectedTankId', tankId);
    window.location.href = `manage-aquarium.html?tankId=${tankId}`;
}

// Add to window for WebSocket message handling
window.handleAquariumData = handleAquariumData;
