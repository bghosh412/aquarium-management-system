document.addEventListener('DOMContentLoaded', () => {
    loadOtaUrls();
    setupButtons();
    loadLightNodeVersion();
});

function loadOtaUrls() {
    fetch('/api/settings/ota-urls')
        .then(response => response.json())
        .then(data => {
            document.getElementById('firmwareUrl').value = data.firmwareUrl || '';
            document.getElementById('littlefsUrl').value = data.littlefsUrl || '';
            document.getElementById('lightNodeOtaUrl').value = data.lightNodeOtaUrl || '';
        })
        .catch(error => {
            console.error('Error loading OTA URLs:', error);
        });
}

function loadLightNodeVersion() {
    // Get current version from first online light node
    fetch('/api/nodes/light/version')
        .then(response => response.json())
        .then(data => {
            if (data.version) {
                document.getElementById('lightNodeCurrentVersion').textContent = `v${data.version}`;
            } else {
                document.getElementById('lightNodeCurrentVersion').textContent = 'No device online';
            }
        })
        .catch(error => {
            console.error('Error loading light node version:', error);
            document.getElementById('lightNodeCurrentVersion').textContent = 'Error';
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

    // Light Node OTA buttons
    document.getElementById('checkLightNodeUpdateBtn').addEventListener('click', checkLightNodeUpdate);
    document.getElementById('applyLightNodeUpdateBtn').addEventListener('click', applyLightNodeUpdate);
}

// Light Node OTA state
let lightNodeOtaInfo = {
    hasUpdate: false,
    hasFirmware: false,
    hasConfig: false,
    availableVersion: null
};

function checkLightNodeUpdate() {
    const statusDiv = document.getElementById('lightNodeUpdateStatus');
    const fileListDiv = document.getElementById('lightNodeUpdateFiles');
    const fileList = document.getElementById('lightNodeFileList');
    const availableVersionSpan = document.getElementById('lightNodeAvailableVersion');
    const applyBtn = document.getElementById('applyLightNodeUpdateBtn');

    statusDiv.innerHTML = '<div style="color: var(--color-primary);">Checking for updates...</div>';
    fileListDiv.style.display = 'none';
    applyBtn.disabled = true;

    fetch('/api/nodes/light/check-update', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
            if (data.error) {
                statusDiv.innerHTML = `<div style="color: var(--color-accent-danger);">${data.error}</div>`;
                availableVersionSpan.textContent = '--';
                return;
            }

            lightNodeOtaInfo = {
                hasUpdate: data.hasUpdate,
                hasFirmware: data.hasFirmware,
                hasConfig: data.hasConfig,
                availableVersion: data.availableVersion
            };

            availableVersionSpan.textContent = data.availableVersion ? `v${data.availableVersion}` : '--';

            if (!data.hasUpdate) {
                statusDiv.innerHTML = '<div style="color: var(--color-accent);">✓ Node is up to date</div>';
                return;
            }

            // Show available files
            fileList.innerHTML = '';
            if (data.hasFirmware) {
                fileList.innerHTML += '<li>firmware.bin (new firmware)</li>';
            }
            if (data.hasConfig) {
                fileList.innerHTML += '<li>node_config.txt (configuration update)</li>';
            }
            fileListDiv.style.display = 'block';

            statusDiv.innerHTML = '<div style="color: var(--color-accent);">✓ Update available! Click "Apply Update" to proceed.</div>';
            applyBtn.disabled = false;
        })
        .catch(error => {
            console.error('Check update failed:', error);
            statusDiv.innerHTML = '<div style="color: var(--color-accent-danger);">Failed to check for updates.</div>';
        });
}

function applyLightNodeUpdate() {
    const statusDiv = document.getElementById('lightNodeUpdateStatus');
    const applyBtn = document.getElementById('applyLightNodeUpdateBtn');

    if (!lightNodeOtaInfo.hasUpdate) {
        statusDiv.innerHTML = '<div style="color: var(--color-accent-warning);">No update available. Check for updates first.</div>';
        return;
    }

    applyBtn.disabled = true;
    statusDiv.innerHTML = '<div style="color: var(--color-primary);">Downloading and sending OTA update to node...</div>';

    // Show progress
    const progressDiv = document.createElement('div');
    progressDiv.id = 'otaProgress';
    progressDiv.style.cssText = 'margin-top: 0.5rem; padding: 0.5rem; background: var(--color-bg-secondary); border-radius: var(--radius-sm);';
    progressDiv.innerHTML = '<div id="otaProgressText">Starting...</div><progress id="otaProgressBar" value="0" max="100" style="width: 100%; margin-top: 0.5rem;"></progress>';
    statusDiv.appendChild(progressDiv);

    // Start OTA process
    fetch('/api/nodes/light/apply-update', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
            if (data.success) {
                statusDiv.innerHTML = '<div style="color: var(--color-accent);">✓ OTA update sent successfully! Node will reboot.</div>';
                // Refresh version after a delay
                setTimeout(loadLightNodeVersion, 10000);
            } else {
                statusDiv.innerHTML = `<div style="color: var(--color-accent-danger);">${data.error || 'Update failed'}</div>`;
                applyBtn.disabled = false;
            }
        })
        .catch(error => {
            console.error('Apply update failed:', error);
            statusDiv.innerHTML = '<div style="color: var(--color-accent-danger);">Failed to apply update.</div>';
            applyBtn.disabled = false;
        });
}
