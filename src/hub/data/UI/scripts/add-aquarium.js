// Add Aquarium Page JavaScript

document.addEventListener('DOMContentLoaded', () => {
    const form = document.getElementById('addAquariumForm');
    
    form.addEventListener('submit', (e) => {
        e.preventDefault();
        createAquarium();
    });
});

function createAquarium() {
    const aquarium = {
        name: document.getElementById('tankName').value,
        volumeLiters: parseFloat(document.getElementById('volume').value),
        tankType: document.getElementById('tankType').value,
        location: document.getElementById('location').value || '',
        thresholds: {
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
    if (!aquarium.name || aquarium.name.trim().length === 0) {
        showNotification('Aquarium name is required', 'error');
        return;
    }
    
    if (!aquarium.tankType || aquarium.tankType === '') {
        showNotification('Tank type is required', 'error');
        return;
    }
    
    if (aquarium.volumeLiters <= 0) {
        showNotification('Volume must be greater than 0', 'error');
        return;
    }
    
    if (aquarium.thresholds.temperature.min >= aquarium.thresholds.temperature.max) {
        showNotification('Min temperature must be less than max temperature', 'error');
        return;
    }
    
    if (aquarium.thresholds.ph.min >= aquarium.thresholds.ph.max) {
        showNotification('Min pH must be less than max pH', 'error');
        return;
    }
    
    if (aquarium.thresholds.tds.min >= aquarium.thresholds.tds.max) {
        showNotification('Min TDS must be less than max TDS', 'error');
        return;
    }
    
    // Send to hub via REST API
    fetch('/api/aquariums', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(aquarium)
    })
    .then(response => {
        if (!response.ok) {
            return response.text().then(text => {
                throw new Error(text || 'Failed to create aquarium');
            });
        }
        return response.json();
    })
    .then(data => {
        showNotification('Aquarium created successfully! ID: ' + data.id, 'success');
        
        // Redirect after short delay
        setTimeout(() => {
            window.location.href = 'aquarium-selection.html';
        }, 1500);
    })
    .catch(error => {
        console.error('Error creating aquarium:', error);
        showNotification('Failed to create aquarium: ' + error.message, 'error');
    });
}

function showNotification(message, type) {
    // Create notification element
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
    
    // Remove after 3 seconds
    setTimeout(() => {
        notification.style.animation = 'slideOut 0.3s ease';
        setTimeout(() => notification.remove(), 300);
    }, 3000);
}

// Add animations
const style = document.createElement('style');
style.textContent = `
    @keyframes slideIn {
        from { transform: translateX(400px); opacity: 0; }
        to { transform: translateX(0); opacity: 1; }
    }
    @keyframes slideOut {
        from { transform: translateX(0); opacity: 1; }
        to { transform: translateX(400px); opacity: 0; }
    }
`;
document.head.appendChild(style);
