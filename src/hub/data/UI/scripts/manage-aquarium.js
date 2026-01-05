// Manage Aquarium Page JavaScript

let currentTankId = null;
let currentAquarium = null;

document.addEventListener('DOMContentLoaded', () => {
    // Get tank ID from URL
    const params = new URLSearchParams(window.location.search);
    currentTankId = parseInt(params.get('tankId')) || parseInt(localStorage.getItem('selectedTankId'));
    
    if (!currentTankId) {
        window.location.href = 'aquarium-selection.html';
        return;
    }
    
    loadAquariumData();
    
    const form = document.getElementById('manageAquariumForm');
    form.addEventListener('submit', (e) => {
        e.preventDefault();
        saveAquarium();
    });
});

function loadAquariumData() {
    // Load from API
    fetch('/api/aquariums')
        .then(response => response.json())
        .then(data => {
            if (data.aquariums && Array.isArray(data.aquariums)) {
                currentAquarium = data.aquariums.find(a => a.id === currentTankId);
                if (currentAquarium) {
                    populateForm();
                }
            }
        })
        .catch(error => {
            console.error('Error loading aquarium:', error);
            // Fallback to localStorage
            const aquariums = JSON.parse(localStorage.getItem('aquariums') || '[]');
            currentAquarium = aquariums.find(a => a.id === currentTankId);
            if (currentAquarium) {
                populateForm();
            }
        });
}

function populateForm() {
    document.getElementById('aquariumTitle').textContent = `Manage ${currentAquarium.name}`;
    document.getElementById('tankId').value = currentAquarium.id;
    document.getElementById('tankName').value = currentAquarium.name;
    document.getElementById('volume').value = currentAquarium.volumeLiters;
    document.getElementById('location').value = currentAquarium.location || '';
    
    // Thresholds
    if (currentAquarium.waterParameters) {
        document.getElementById('tempMin').value = currentAquarium.waterParameters.temperature.min;
        document.getElementById('tempMax').value = currentAquarium.waterParameters.temperature.max;
        document.getElementById('phMin').value = currentAquarium.waterParameters.ph.min;
        document.getElementById('phMax').value = currentAquarium.waterParameters.ph.max;
        document.getElementById('tdsMin').value = currentAquarium.waterParameters.tds.min;
        document.getElementById('tdsMax').value = currentAquarium.waterParameters.tds.max;
    }
    
    // Status badge
    const badge = document.getElementById('statusBadge');
    if (currentAquarium.enabled) {
        badge.className = 'badge badge-online';
        badge.textContent = 'Active';
    } else {
        badge.className = 'badge badge-offline';
        badge.textContent = 'Offline';
    }
}

function saveAquarium() {
    const updatedAquarium = {
        name: document.getElementById('tankName').value,
        volumeLiters: parseFloat(document.getElementById('volume').value),
        location: document.getElementById('location').value || '',
        waterParameters: {
            temperature: {
                min: parseFloat(document.getElementById('tempMin').value),
                max: parseFloat(document.getElementById('tempMax').value)
            },
            ph: {
                min: parseFloat(document.getElementById('phMin').value),
                max: parseFloat(document.getElementById('phMax').value)
            },
            tds: {
                min: parseInt(document.getElementById('tdsMin').value),
                max: parseInt(document.getElementById('tdsMax').value)
            }
        }
    };
    
    // Validation
    if (updatedAquarium.volumeLiters <= 0) {
        showNotification('Volume must be greater than 0', 'error');
        return;
    }
    
    if (updatedAquarium.waterParameters.temperature.min >= updatedAquarium.waterParameters.temperature.max) {
        showNotification('Min temperature must be less than max temperature', 'error');
        return;
    }
    
    if (updatedAquarium.waterParameters.ph.min >= updatedAquarium.waterParameters.ph.max) {
        showNotification('Min pH must be less than max pH', 'error');
        return;
    }
    
    if (updatedAquarium.waterParameters.tds.min >= updatedAquarium.waterParameters.tds.max) {
        showNotification('Min TDS must be less than max TDS', 'error');
        return;
    }
    
    // Send to hub via REST API
    fetch(`/api/aquarium/update?id=${currentTankId}`, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(updatedAquarium)
    })
    .then(response => {
        if (response.ok) {
            showNotification('Aquarium updated successfully!', 'success');
            // Reload data
            setTimeout(() => loadAquariumData(), 1000);
        } else {
            showNotification('Failed to update aquarium', 'error');
        }
    })
    .catch(error => {
        console.error('Error updating aquarium:', error);
        showNotification('Error updating aquarium', 'error');
    });
}

function deleteAquarium() {
    if (!confirm('Are you sure you want to delete this aquarium? This action cannot be undone.')) {
        return;
    }
    
    // Send to hub via REST API
    fetch(`/api/aquarium/delete?id=${currentTankId}`, {
        method: 'POST'
    })
    .then(response => {
        if (response.ok) {
            showNotification('Aquarium deleted successfully!', 'success');
            setTimeout(() => {
                window.location.href = 'aquarium-selection.html';
            }, 1000);
        } else {
            showNotification('Failed to delete aquarium', 'error');
        }
    })
    .catch(error => {
        console.error('Error deleting aquarium:', error);
        showNotification('Error deleting aquarium', 'error');
    });
}

// Remove old WebSocket-based delete function
/*
function deleteAquarium() {
    if (!confirm('Are you sure you want to delete this aquarium? This action cannot be undone.')) {
        return;
    }
    
    // Send to hub via WebSocket
    if (window.ws && window.ws.readyState === WebSocket.OPEN) {
        sendCommand({
            type: 'deleteAquarium',
            tankId: currentTankId
        });
    }
    
    // Remove from localStorage
    const aquariums = JSON.parse(localStorage.getItem('aquariums') || '[]');
    const filtered = aquariums.filter(a => a.tankId !== currentTankId);
    localStorage.setItem('aquariums', JSON.stringify(filtered));
    
    showNotification('Aquarium deleted', 'success');
    
    setTimeout(() => {
        window.location.href = 'aquarium-selection.html';
    }, 1500);
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

// Handle WebSocket aquarium data
function handleAquariumUpdate(data) {
    if (data.type === 'aquariumData' && data.tankId === currentTankId) {
        currentAquarium = data.aquarium;
        populateForm();
    }
}

window.handleAquariumUpdate = handleAquariumUpdate;
