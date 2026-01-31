document.addEventListener('DOMContentLoaded', () => {
    loadOtaUrls();
    setupButtons();
    loadLightNodes();
});

function loadOtaUrls() {
    fetch('/api/settings/ota-urls')
        .then(response => response.json())
        .then(data => {
            // Hub OTA versions (URLs no longer displayed)
            document.getElementById('hubFirmwareCurrentVersion').textContent = data.hubFirmwareVersion || '--';
            document.getElementById('hubLittlefsCurrentVersion').textContent = data.hubLittlefsVersion || '--';
            
            // Light node OTA URL
            document.getElementById('lightNodeOtaUrl').value = data.lightNodeOtaUrl || 'Not configured';
        })
        .catch(error => {
            console.error('Error loading OTA URLs:', error);
        });
}

// ===== Light Nodes State =====
let lightNodesData = [];
let selectedLightNodes = new Set();

function loadLightNodes() {
    const tbody = document.getElementById('lightNodesTableBody');
    tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: var(--text-secondary);">Loading devices...</td></tr>';
    
    fetch('/api/nodes/light/list')
        .then(response => response.json())
        .then(data => {
            lightNodesData = data.devices || [];
            renderLightNodesTable();
        })
        .catch(error => {
            console.error('Error loading light nodes:', error);
            tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: var(--danger-color);">Error loading devices</td></tr>';
        });
}

function renderLightNodesTable() {
    const tbody = document.getElementById('lightNodesTableBody');
    const selectAllCheckbox = document.getElementById('selectAllLightNodes');
    
    if (lightNodesData.length === 0) {
        tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: var(--text-secondary);">No light nodes found</td></tr>';
        selectAllCheckbox.disabled = true;
        return;
    }
    
    const onlineDevices = lightNodesData.filter(d => d.online);
    selectAllCheckbox.disabled = onlineDevices.length === 0;
    
    tbody.innerHTML = lightNodesData.map(device => {
        const isOnline = device.online;
        const isChecked = selectedLightNodes.has(device.mac);
        const statusClass = isOnline ? 'online' : 'offline';
        const statusText = isOnline ? 'Online' : 'Offline';
        const fwVersion = device.firmwareVersion ? `v${device.firmwareVersion}` : '--';
        
        return `
            <tr data-mac="${device.mac}">
                <td>
                    <input type="checkbox" 
                           class="light-node-checkbox" 
                           data-mac="${device.mac}"
                           ${isChecked ? 'checked' : ''} 
                           ${!isOnline ? 'disabled' : ''}>
                </td>
                <td>${device.name || 'Unknown'}</td>
                <td>${device.tankName || '--'}</td>
                <td><span class="status-badge ${statusClass}">${statusText}</span></td>
                <td>${fwVersion}</td>
                <td class="update-status-cell"></td>
            </tr>
        `;
    }).join('');
    
    // Setup checkbox event listeners
    document.querySelectorAll('.light-node-checkbox').forEach(checkbox => {
        checkbox.addEventListener('change', (e) => {
            const mac = e.target.dataset.mac;
            if (e.target.checked) {
                selectedLightNodes.add(mac);
            } else {
                selectedLightNodes.delete(mac);
            }
            updateSelectedCount();
            updateSelectAllCheckbox();
        });
    });
    
    updateSelectedCount();
    updateSelectAllCheckbox();
}

function updateSelectedCount() {
    document.getElementById('lightNodeSelectedCount').textContent = selectedLightNodes.size;
    
    // Enable/disable apply button based on selection and update availability
    const applyBtn = document.getElementById('applyLightNodeUpdateBtn');
    applyBtn.disabled = selectedLightNodes.size === 0 || !lightNodeOtaInfo.hasUpdate;
}

function updateSelectAllCheckbox() {
    const selectAllCheckbox = document.getElementById('selectAllLightNodes');
    const onlineDevices = lightNodesData.filter(d => d.online);
    const allOnlineSelected = onlineDevices.length > 0 && 
                              onlineDevices.every(d => selectedLightNodes.has(d.mac));
    const someSelected = onlineDevices.some(d => selectedLightNodes.has(d.mac));
    
    selectAllCheckbox.checked = allOnlineSelected;
    selectAllCheckbox.indeterminate = someSelected && !allOnlineSelected;
}

// Hub OTA state
let hubOtaInfo = {
    firmwareHasUpdate: false,
    littlefsHasUpdate: false,
    firmwareAvailable: null,
    littlefsAvailable: null
};

function setupButtons() {
    // Select All checkbox handler
    document.getElementById('selectAllLightNodes').addEventListener('change', (e) => {
        const checkAll = e.target.checked;
        lightNodesData.forEach(device => {
            if (device.online) {
                if (checkAll) {
                    selectedLightNodes.add(device.mac);
                } else {
                    selectedLightNodes.delete(device.mac);
                }
            }
        });
        renderLightNodesTable();
    });

    // Hub Check for Updates button (combined firmware + littlefs)
    document.getElementById('checkHubUpdateBtn').addEventListener('click', checkHubUpdates);

    // Hub Apply Updates button
    document.getElementById('applyHubUpdateBtn').addEventListener('click', applyHubUpdates);

    // Light Node OTA buttons
    document.getElementById('checkLightNodeUpdateBtn').addEventListener('click', checkLightNodeUpdate);
    document.getElementById('applyLightNodeUpdateBtn').addEventListener('click', applyLightNodeUpdate);
}

function updateHubStatusBadge(type, message) {
    const badge = document.getElementById('hubUpdateStatusBadge');
    const icon = document.getElementById('hubUpdateStatusIcon');
    const text = document.getElementById('hubUpdateStatusText');
    
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
        case 'updating':
            badge.style.background = 'rgba(59, 130, 246, 0.1)';
            badge.style.borderLeft = '4px solid var(--color-primary)';
            icon.textContent = '⏳';
            break;
    }
    text.textContent = message;
}

function checkHubUpdates() {
    const checkBtn = document.getElementById('checkHubUpdateBtn');
    const applyBtn = document.getElementById('applyHubUpdateBtn');
    
    checkBtn.disabled = true;
    updateHubStatusBadge('checking', 'Checking for updates...');
    
    fetch('/api/hub/ota/check')
        .then(response => response.json())
        .then(data => {
            checkBtn.disabled = false;
            
            // Update firmware version display
            if (data.firmware && !data.firmware.error) {
                document.getElementById('hubFirmwareAvailableVersion').textContent = data.firmware.availableVersion || '--';
                hubOtaInfo.firmwareHasUpdate = data.firmware.hasUpdate;
                hubOtaInfo.firmwareAvailable = data.firmware.availableVersion;
            } else {
                document.getElementById('hubFirmwareAvailableVersion').textContent = data.firmware?.error ? 'Error' : '--';
                hubOtaInfo.firmwareHasUpdate = false;
            }
            
            // Update LittleFS version display
            if (data.littlefs && !data.littlefs.error) {
                document.getElementById('hubLittlefsAvailableVersion').textContent = data.littlefs.availableVersion || '--';
                hubOtaInfo.littlefsHasUpdate = data.littlefs.hasUpdate;
                hubOtaInfo.littlefsAvailable = data.littlefs.availableVersion;
            } else {
                document.getElementById('hubLittlefsAvailableVersion').textContent = data.littlefs?.error ? 'Error' : '--';
                hubOtaInfo.littlefsHasUpdate = false;
            }
            
            // Determine overall status
            const hasAnyUpdate = hubOtaInfo.firmwareHasUpdate || hubOtaInfo.littlefsHasUpdate;
            applyBtn.disabled = !hasAnyUpdate;
            
            if (hasAnyUpdate) {
                let updateList = [];
                if (hubOtaInfo.firmwareHasUpdate) updateList.push('Firmware');
                if (hubOtaInfo.littlefsHasUpdate) updateList.push('LittleFS');
                updateHubStatusBadge('available', `Updates available: ${updateList.join(', ')}`);
            } else if (data.firmware?.error || data.littlefs?.error) {
                let errors = [];
                if (data.firmware?.error) errors.push(`Firmware: ${data.firmware.error}`);
                if (data.littlefs?.error) errors.push(`LittleFS: ${data.littlefs.error}`);
                updateHubStatusBadge('error', errors.join(' | '));
            } else {
                updateHubStatusBadge('uptodate', 'Hub is up to date');
            }
        })
        .catch(error => {
            console.error('Hub update check failed:', error);
            checkBtn.disabled = false;
            updateHubStatusBadge('error', 'Failed to check for updates');
        });
}

function applyHubUpdates() {
    const hasAnyUpdate = hubOtaInfo.firmwareHasUpdate || hubOtaInfo.littlefsHasUpdate;
    if (!hasAnyUpdate) {
        updateHubStatusBadge('error', 'No updates available. Check for updates first.');
        return;
    }
    
    let updateList = [];
    if (hubOtaInfo.firmwareHasUpdate) updateList.push('Firmware');
    if (hubOtaInfo.littlefsHasUpdate) updateList.push('LittleFS');
    
    if (!confirm(`Apply updates (${updateList.join(' + ')})? The hub will reboot after the update.`)) return;
    
    const checkBtn = document.getElementById('checkHubUpdateBtn');
    const applyBtn = document.getElementById('applyHubUpdateBtn');
    const progressSection = document.getElementById('hubProgressSection');
    const progressBar = document.getElementById('hubProgressBar');
    const progressText = document.getElementById('hubProgressText');
    const progressPercent = document.getElementById('hubProgressPercent');
    const progressInfo = document.getElementById('hubProgressInfo');
    
    checkBtn.disabled = true;
    applyBtn.disabled = true;
    
    // Show progress section
    progressSection.style.display = 'block';
    progressBar.style.width = '0%';
    progressBar.style.background = 'var(--color-primary)';
    progressText.textContent = 'Starting update...';
    progressPercent.textContent = '0%';
    progressInfo.textContent = `Updating: ${updateList.join(', ')}`;
    
    updateHubStatusBadge('updating', 'Downloading and applying updates...');
    
    // Determine which endpoint to call
    // Use /api/ota/all when both updates available - it applies firmware + littlefs before single reboot
    // Use individual endpoints if only one update available
    let endpoint;
    let updateType;
    
    if (hubOtaInfo.firmwareHasUpdate && hubOtaInfo.littlefsHasUpdate) {
        endpoint = '/api/ota/all';
        updateType = 'all';
    } else if (hubOtaInfo.firmwareHasUpdate) {
        endpoint = '/api/ota/firmware';
        updateType = 'firmware';
    } else {
        endpoint = '/api/ota/littlefs';
        updateType = 'littlefs';
    }
    
    progressText.textContent = updateType === 'all' 
        ? 'Downloading firmware + LittleFS...' 
        : `Downloading ${updateType}...`;
    progressBar.style.width = '25%';
    progressPercent.textContent = '25%';
    
    fetch(endpoint, { method: 'POST' })
        .then(response => response.json())
        .then(data => {
            if (data.success) {
                progressBar.style.width = '100%';
                progressBar.style.background = 'var(--color-accent)';
                progressPercent.textContent = '100%';
                progressText.textContent = 'Update complete!';
                progressInfo.textContent = data.message || 'Hub is rebooting...';
                updateHubStatusBadge('success', data.message || 'Update applied! Hub is rebooting...');
            } else {
                progressBar.style.background = 'var(--color-accent-danger)';
                progressText.textContent = 'Update failed';
                updateHubStatusBadge('error', data.error || 'Update failed');
                checkBtn.disabled = false;
                applyBtn.disabled = false;
            }
        })
        .catch(error => {
            console.error('Hub update failed:', error);
            progressBar.style.width = '75%';
            progressBar.style.background = 'var(--color-accent-warning)';
            progressPercent.textContent = '75%';
            progressText.textContent = 'Connection lost';
            progressInfo.textContent = 'Hub may be rebooting...';
            updateHubStatusBadge('success', 'Update likely applied - hub may be rebooting');
        });
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

    // Also refresh the light nodes list
    loadLightNodes();

    fetch('/api/nodes/light/check-update', { method: 'POST' })
        .then(response => response.json())
        .then(data => {
            if (data.error) {
                updateStatusBadge('error', data.error);
                availableVersionSpan.textContent = '--';
                lightNodeOtaInfo.hasUpdate = false;
                updateSelectedCount();
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
                updateStatusBadge('uptodate', 'All nodes are up to date');
                updateSelectedCount();
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

            updateStatusBadge('available', 'Update available! Select devices and click "Apply Updates".');
            updateSelectedCount();
        })
        .catch(error => {
            console.error('Check update failed:', error);
            updateStatusBadge('error', 'Failed to check for updates');
            lightNodeOtaInfo.hasUpdate = false;
            updateSelectedCount();
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

    if (selectedLightNodes.size === 0) {
        updateStatusBadge('error', 'Please select at least one device to update.');
        return;
    }

    const selectedMacs = Array.from(selectedLightNodes);
    const confirmMsg = `Update ${selectedMacs.length} device(s)? Nodes will reboot after firmware update.`;
    if (!confirm(confirmMsg)) return;

    // Disable buttons during update
    applyBtn.disabled = true;
    checkBtn.disabled = true;
    
    // Disable all checkboxes during update
    document.querySelectorAll('.light-node-checkbox').forEach(cb => cb.disabled = true);
    document.getElementById('selectAllLightNodes').disabled = true;
    
    // Show and reset progress section
    progressSection.style.display = 'block';
    progressBar.style.width = '0%';
    progressBar.style.background = 'var(--color-primary)';
    progressText.textContent = 'Starting OTA update...';
    progressPercent.textContent = '0%';
    deviceInfo.textContent = `Updating ${selectedMacs.length} device(s)...`;
    
    updateStatusBadge('checking', 'Downloading and sending OTA update...');

    // Clear any previous update status indicators
    document.querySelectorAll('.update-status-cell').forEach(cell => {
        cell.innerHTML = '';
    });

    // Start OTA process with selected MACs
    fetch('/api/nodes/light/apply-update', { 
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({ macs: selectedMacs })
    })
        .then(response => response.json())
        .then(data => {
            if (data.success) {
                // Start polling for status
                pollOtaStatus();
            } else {
                updateStatusBadge('error', data.error || 'Update failed');
                progressSection.style.display = 'none';
                resetButtonsAfterUpdate();
            }
        })
        .catch(error => {
            console.error('Apply update failed:', error);
            updateStatusBadge('error', 'Failed to apply update');
            progressSection.style.display = 'none';
            resetButtonsAfterUpdate();
        });
}

function resetButtonsAfterUpdate() {
    document.getElementById('applyLightNodeUpdateBtn').disabled = true;
    document.getElementById('checkLightNodeUpdateBtn').disabled = false;
    lightNodeOtaInfo.hasUpdate = false;
    selectedLightNodes.clear();
    
    // Re-render table to re-enable checkboxes
    renderLightNodesTable();
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
                    
                    // Show which device is being updated
                    if (data.deviceCount > 0) {
                        const currentDeviceName = data.currentDeviceName || `Device ${data.currentDevice}`;
                        deviceInfo.textContent = `Updating: ${currentDeviceName} (${data.currentDevice}/${data.deviceCount}) • ${data.devicesUpdated || 0} updated • ${data.devicesFailed || 0} failed`;
                        
                        // Update the table to show which device is being updated
                        updateTableDeviceStatus(data.currentDeviceMac, 'updating');
                        
                        // Mark completed devices
                        if (data.completedMacs) {
                            data.completedMacs.forEach(mac => updateTableDeviceStatus(mac, 'done'));
                        }
                        if (data.failedMacs) {
                            data.failedMacs.forEach(mac => updateTableDeviceStatus(mac, 'failed'));
                        }
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

                        const progressSection = document.getElementById('lightNodeProgressSection');
                        
                        if (data.success || data.devicesUpdated > 0) {
                            progressBar.style.width = '100%';
                            progressBar.style.background = 'var(--color-accent)';
                            progressPercent.textContent = '100%';
                            progressText.textContent = 'Update complete!';
                            
                            let msg = 'OTA update completed!';
                            if (data.deviceCount > 1) {
                                msg += ` (${data.devicesUpdated}/${data.deviceCount} devices updated)`;
                            }
                            if (data.devicesFailed > 0) {
                                msg += ` Warning: ${data.devicesFailed} device(s) failed.`;
                                updateStatusBadge('available', msg);
                            } else {
                                updateStatusBadge('success', msg);
                            }
                            
                            // Hide progress bar after delay
                            setTimeout(() => { progressSection.style.display = 'none'; }, 3000);
                        } else {
                            progressBar.style.background = 'var(--color-accent-danger)';
                            progressText.textContent = 'Update failed';
                            updateStatusBadge('error', data.error || 'Update failed');
                        }
                        
                        // Refresh device list after a delay to show new firmware versions
                        setTimeout(loadLightNodes, 5000);
                        
                        resetButtonsAfterUpdate();
                    }
                }
            })
            .catch(error => {
                console.error('Poll status failed:', error);
            });
    }, 1000);  // Poll every second
}

function updateTableDeviceStatus(mac, status) {
    if (!mac) return;
    
    const row = document.querySelector(`tr[data-mac="${mac}"]`);
    if (!row) return;
    
    const statusCell = row.querySelector('.update-status-cell');
    if (!statusCell) return;
    
    switch(status) {
        case 'updating':
            statusCell.innerHTML = '<span class="updating-indicator">⏳ Updating...</span>';
            row.style.background = 'rgba(59, 130, 246, 0.1)';
            break;
        case 'done':
            statusCell.innerHTML = '<span style="color: var(--success-color);">✅ Done</span>';
            row.style.background = 'rgba(16, 185, 129, 0.1)';
            break;
        case 'failed':
            statusCell.innerHTML = '<span style="color: var(--danger-color);">❌ Failed</span>';
            row.style.background = 'rgba(239, 68, 68, 0.1)';
            break;
        default:
            statusCell.innerHTML = '';
            row.style.background = '';
    }
}
