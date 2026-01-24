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

function updateStatusBadge(type, message) {
    const badge = document.getElementById('lightNodeStatusBadge');
    const icon = document.getElementById('lightNodeStatusIcon');
    const text = document.getElementById('lightNodeStatusText');
    
    badge.style.display = 'flex';
    badge.style.alignItems = 'center';
    badge.style.gap = '0.5rem';
    
    switch(type) {
        case 'checking':
            badge.style.background = 'rgba(59, 130, 246, 0.1)';
            badge.style.borderLeft = '4px solid var(--color-primary)';
            icon.textContent = '🔄';
            break;
        case 'uptodate':
            badge.style.background = 'rgba(16, 185, 129, 0.1)';
            badge.style.borderLeft = '4px solid var(--color-accent)';
            icon.textContent = '✅';
            break;
        case 'available':
            badge.style.background = 'rgba(245, 158, 11, 0.1)';
            badge.style.borderLeft = '4px solid var(--color-accent-warning)';
            icon.textContent = '🆕';
            break;
        case 'error':
            badge.style.background = 'rgba(239, 68, 68, 0.1)';
            badge.style.borderLeft = '4px solid var(--color-accent-danger)';
            icon.textContent = '❌';
            break;
        case 'success':
            badge.style.background = 'rgba(16, 185, 129, 0.1)';
            badge.style.borderLeft = '4px solid var(--color-accent)';
            icon.textContent = '✅';
            break;
    }
    text.textContent = message;
}

function checkLightNodeUpdate() {
    const fileListDiv = document.getElementById('lightNodeUpdateFiles');
    const fileList = document.getElementById('lightNodeFileList');
    const availableVersionSpan = document.getElementById('lightNodeAvailableVersion');
    const applyBtn = document.getElementById('applyLightNodeUpdateBtn');
    const progressSection = document.getElementById('lightNodeProgressSection');

    updateStatusBadge('checking', 'Checking for updates...');
    fileListDiv.style.display = 'none';
    progressSection.style.display = 'none';
    applyBtn.disabled = true;

    fetch('/api/nodes/light/check-update', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
            if (data.error) {
                updateStatusBadge('error', data.error);
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
                updateStatusBadge('uptodate', 'Node firmware is up to date');
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

            updateStatusBadge('available', 'Update available! Click "Apply Update" to install.');
            applyBtn.disabled = false;
        })
        .catch(error => {
            console.error('Check update failed:', error);
            updateStatusBadge('error', 'Failed to check for updates');
        });
}

function applyLightNodeUpdate() {
    const applyBtn = document.getElementById('applyLightNodeUpdateBtn');
    const checkBtn = document.getElementById('checkLightNodeUpdateBtn');
    const progressSection = document.getElementById('lightNodeProgressSection');
    const progressBar = document.getElementById('lightNodeProgressBar');
    const progressText = document.getElementById('lightNodeProgressText');
    const progressPercent = document.getElementById('lightNodeProgressPercent');
    const deviceInfo = document.getElementById('lightNodeDeviceInfo');

    if (!lightNodeOtaInfo.hasUpdate) {
        updateStatusBadge('error', 'No update available. Check for updates first.');
        return;
    }

    // Disable buttons during update
    applyBtn.disabled = true;
    checkBtn.disabled = true;
    
    // Show and reset progress section
    progressSection.style.display = 'block';
    progressBar.style.width = '0%';
    progressText.textContent = 'Starting OTA update...';
    progressPercent.textContent = '0%';
    deviceInfo.textContent = '';
    
    updateStatusBadge('checking', 'Downloading and sending OTA update...');

    // Start OTA process
    fetch('/api/nodes/light/apply-update', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
            if (data.success) {
                if (data.deviceCount > 1) {
                    deviceInfo.textContent = `Updating ${data.deviceCount} device(s)...`;
                }
                // Start polling for status
                pollOtaStatus();
            } else {
                updateStatusBadge('error', data.error || 'Update failed');
                progressSection.style.display = 'none';
                applyBtn.disabled = false;
                checkBtn.disabled = false;
            }
        })
        .catch(error => {
            console.error('Apply update failed:', error);
            updateStatusBadge('error', 'Failed to apply update');
            progressSection.style.display = 'none';
            applyBtn.disabled = false;
            checkBtn.disabled = false;
        });
}

let otaPollInterval = null;

function pollOtaStatus() {
    // Clear any existing poll
    if (otaPollInterval) {
        clearInterval(otaPollInterval);
    }

    otaPollInterval = setInterval(() => {
        fetch('/api/nodes/light/ota-status')
            .then(response => response.json())
            .then(data => {
                const progressBar = document.getElementById('lightNodeProgressBar');
                const progressText = document.getElementById('lightNodeProgressText');
                const progressPercent = document.getElementById('lightNodeProgressPercent');
                const deviceInfo = document.getElementById('lightNodeDeviceInfo');

                if (progressBar && progressText) {
                    const progress = data.progress || 0;
                    progressBar.style.width = `${progress}%`;
                    progressPercent.textContent = `${progress}%`;
                    
                    if (data.deviceCount > 1) {
                        deviceInfo.textContent = `Device ${data.currentDevice}/${data.deviceCount} • ${data.devicesUpdated} updated • ${data.devicesFailed} failed`;
                    }

                    if (data.active) {
                        if (data.firmwareSaved && !data.firmwareSent) {
                            progressText.textContent = `Sending firmware: ${data.firmwareChunks}/${data.firmwareTotalChunks} chunks`;
                        } else if (data.configSaved && !data.configSent) {
                            progressText.textContent = `Sending config: ${data.configChunks}/${data.configTotalChunks} chunks`;
                        } else if (data.firmwareSent) {
                            progressText.textContent = 'Waiting for node confirmation...';
                            progressBar.style.background = 'var(--color-accent-warning)';
                        } else {
                            progressText.textContent = 'Downloading files...';
                        }
                    }

                    if (data.completed) {
                        clearInterval(otaPollInterval);
                        otaPollInterval = null;

                        const applyBtn = document.getElementById('applyLightNodeUpdateBtn');
                        const checkBtn = document.getElementById('checkLightNodeUpdateBtn');
                        const progressSection = document.getElementById('lightNodeProgressSection');
                        
                        if (data.success) {
                            progressBar.style.width = '100%';
                            progressBar.style.background = 'var(--color-accent)';
                            progressPercent.textContent = '100%';
                            progressText.textContent = 'Update complete!';
                            
                            let msg = 'OTA update completed successfully!';
                            if (data.deviceCount > 1) {
                                msg += ` (${data.devicesUpdated}/${data.deviceCount} devices updated)`;
                            }
                            if (data.devicesFailed > 0) {
                                msg += ` Warning: ${data.devicesFailed} device(s) failed.`;
                                updateStatusBadge('available', msg);
                            } else {
                                updateStatusBadge('success', msg);
                            }
                            // Refresh version after a delay
                            setTimeout(loadLightNodeVersion, 5000);
                            // Hide progress bar after delay
                            setTimeout(() => { progressSection.style.display = 'none'; }, 3000);
                        } else {
                            progressBar.style.background = 'var(--color-accent-danger)';
                            progressText.textContent = 'Update failed';
                            updateStatusBadge('error', data.error || 'Update failed');
                        }
                        
                        // Re-enable buttons but keep Apply disabled (need to check again)
                        checkBtn.disabled = false;
                        applyBtn.disabled = true;
                        lightNodeOtaInfo.hasUpdate = false;
                    }
                }
            })
            .catch(error => {
                console.error('Poll status failed:', error);
            });
    }, 1000);  // Poll every second
}
