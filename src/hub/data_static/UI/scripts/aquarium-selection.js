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
        <div class="card" style="cursor: pointer; transition: all 0.3s ease;" onclick="selectAquarium(${aquarium.id})">
            <div class="card-header">
                <h3>${aquarium.name}</h3>
                <span class="badge ${aquarium.enabled ? 'badge-online' : 'badge-offline'}">
                    ${aquarium.enabled ? 'Active' : 'Offline'}
                </span>
            </div>
            <div class="card-body">
                <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; margin-bottom: 1rem;">
                    <div>
                        <div style="color: var(--color-text-secondary); font-size: 0.75rem;">Tank ID</div>
                        <div style="font-weight: 600; font-size: 1.25rem;">${aquarium.id}</div>
                    </div>
                    <div>
                        <div style="color: var(--color-text-secondary); font-size: 0.75rem;">Volume</div>
                        <div style="font-weight: 600; font-size: 1.25rem;">${aquarium.volumeLiters}L</div>
                    </div>
                </div>
                
                ${aquarium.location ? `
                    <div style="color: var(--color-text-secondary); font-size: 0.875rem; margin-bottom: 1rem;">
                        📍 ${aquarium.location}
                    </div>
                ` : ''}
                
                <div style="display: flex; gap: 0.5rem; margin-top: 1rem;">
                    <span class="badge badge-online">${aquarium.deviceCount || 0} devices</span>
                </div>
            </div>
            <div class="card-footer" style="display: flex; gap: 0.5rem;">
                <button class="btn btn-primary" style="flex: 1;" onclick="event.stopPropagation(); viewDevices(${aquarium.id})">
                    🔌 Devices
                </button>
                <button class="btn btn-secondary" style="flex: 1;" onclick="event.stopPropagation(); manageAquarium(${aquarium.id})">
                    ⚙️ Settings
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
