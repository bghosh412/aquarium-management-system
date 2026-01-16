document.addEventListener('DOMContentLoaded', () => {
    loadOtaUrls();
    setupButtons();
});

function loadOtaUrls() {
    fetch('/api/settings/ota-urls')
        .then(response => response.json())
        .then(data => {
            document.getElementById('firmwareUrl').value = data.firmwareUrl || '';
            document.getElementById('littlefsUrl').value = data.littlefsUrl || '';
        })
        .catch(error => {
            console.error('Error loading OTA URLs:', error);
        });
}

function setupButtons() {
    const updateStatus = document.getElementById('updateStatus');

    document.getElementById('updateFirmwareBtn').addEventListener('click', () => {
        updateStatus.innerHTML = '<div style="color: var(--color-primary);">Updating firmware...</div>';
        fetch('/api/ota/firmware', { method: 'POST' })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    updateStatus.innerHTML = '<div style="color: var(--color-accent);">Firmware update started. Hub will reboot if successful.</div>';
                } else {
                    updateStatus.innerHTML = `<div style="color: var(--color-accent-danger);">${data.error || 'Update failed'}</div>`;
                }
            })
            .catch(error => {
                console.error('Firmware update failed:', error);
                updateStatus.innerHTML = '<div style="color: var(--color-accent-danger);">Update failed.</div>';
            });
    });

    document.getElementById('updateLittlefsBtn').addEventListener('click', () => {
        updateStatus.innerHTML = '<div style="color: var(--color-primary);">Updating LittleFS...</div>';
        fetch('/api/ota/littlefs', { method: 'POST' })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    updateStatus.innerHTML = '<div style="color: var(--color-accent);">LittleFS update started. Hub will reboot if successful.</div>';
                } else {
                    updateStatus.innerHTML = `<div style="color: var(--color-accent-danger);">${data.error || 'Update failed'}</div>`;
                }
            })
            .catch(error => {
                console.error('LittleFS update failed:', error);
                updateStatus.innerHTML = '<div style="color: var(--color-accent-danger);">Update failed.</div>';
            });
    });
}
