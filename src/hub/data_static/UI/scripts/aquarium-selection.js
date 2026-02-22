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
                <!-- Maintenance Mode Toggle -->
                <div class="checkbox-group" style="margin-top: 0.75rem; padding: 0.5rem 0.75rem; background: ${aquarium.maintenanceMode ? 'rgba(251, 191, 36, 0.15)' : 'rgba(255,255,255,0.03)'}; border-radius: 0.5rem; border: 1px solid ${aquarium.maintenanceMode ? 'rgba(251, 191, 36, 0.4)' : 'rgba(255,255,255,0.08)'};" onclick="event.stopPropagation();">
                    <label style="display: flex; align-items: center; gap: 0.5rem; cursor: pointer; font-size: 0.85rem; margin: 0;">
                        <input type="checkbox" 
                               ${aquarium.maintenanceMode ? 'checked' : ''} 
                               onchange="toggleMaintenanceMode(${aquarium.id}, this.checked)"
                               style="width: 16px; height: 16px; accent-color: #f59e0b; cursor: pointer;">
                        <span style="color: ${aquarium.maintenanceMode ? '#fbbf24' : 'var(--color-text-secondary)'}; font-weight: ${aquarium.maintenanceMode ? '600' : '400'};">
                            🔧 Maintenance Mode ${aquarium.maintenanceMode ? '(Active)' : ''}
                        </span>
                    </label>
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

function toggleMaintenanceMode(tankId, enabled) {
    const aquarium = aquariums.find(a => a.id === tankId);
    const name = aquarium ? aquarium.name : `Tank ${tankId}`;
    const action = enabled ? 'enable' : 'disable';
    
    if (!confirm(`${action.charAt(0).toUpperCase() + action.slice(1)} Maintenance Mode for "${name}"?\n\n` +
        (enabled 
            ? '• Lights will be turned ON\n• CO₂, heater, wave maker, and feeder will be STOPPED\n• Scheduled tasks will be PAUSED'
            : '• Lights will be turned OFF\n• Scheduled tasks will RESUME'))) {
        // User cancelled - revert checkbox
        event.target.checked = !enabled;
        return;
    }
    
    fetch('/api/aquarium/maintenance', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ id: tankId, maintenanceMode: enabled })
    })
    .then(response => response.json())
    .then(data => {
        if (data.success) {
            console.log(`Maintenance mode ${enabled ? 'enabled' : 'disabled'} for tank ${tankId}`);
            // Reload aquariums to reflect updated state
            loadAquariums();
        } else {
            alert('Failed to toggle maintenance mode: ' + (data.error || 'Unknown error'));
            event.target.checked = !enabled;
        }
    })
    .catch(error => {
        console.error('Error toggling maintenance mode:', error);
        alert('Failed to toggle maintenance mode. Check connection.');
        event.target.checked = !enabled;
    });
}

// Add to window for WebSocket message handling
window.handleAquariumData = handleAquariumData;
