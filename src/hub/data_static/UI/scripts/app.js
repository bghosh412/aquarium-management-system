// Aquarium Management System - Professional Dashboard JavaScript
// Communicates with ESP32 hub via WebSocket

let ws = null;
let currentTankId = 1;
let nodes = {};
let reconnectInterval = null;
let uptimeInterval = null;
let startTime = Date.now();

// Add activity log entry
function addActivityLog(message, type = 'info') {
    const activityList = document.getElementById('activity-list');
    if (!activityList) return;

    const activityItem = document.createElement('div');
    activityItem.className = 'activity-item';

    const iconColor = {
        'success': '#10b981',
        'error': '#ef4444',
        'warning': '#f59e0b',
        'info': '#0ea5e9'
    }[type] || '#0ea5e9';

    activityItem.innerHTML = `
        <div class="activity-icon">
            <span class="activity-dot" style="background: ${iconColor}"></span>
        </div>
        <div class="activity-details">
            <div class="activity-title">${message}</div>
            <div class="activity-time">${new Date().toLocaleTimeString()}</div>
        </div>
    `;

    // Insert at the beginning
    activityList.insertBefore(activityItem, activityList.firstChild);

    // Keep only last 10 entries
    while (activityList.children.length > 10) {
        activityList.removeChild(activityList.lastChild);
    }
}

// Initialize on page load
document.addEventListener('DOMContentLoaded', () => {
    initWebSocket();
    setupEventListeners();
    updateUptime();
    loadDashboardData();  // Load data from backend
    addActivityLog('System initialized', 'success');
    
    // Update uptime every second
    uptimeInterval = setInterval(updateUptime, 1000);
    
    // Update stats every 5 seconds — only on the dashboard page
    const pathname = window.location.pathname || '/';
    if (pathname === '/' || pathname.endsWith('/index.html')) {
        setInterval(() => {
            updateSystemStats();
            loadDashboardData();  // Refresh dashboard data
        }, 5000);
    }
});

// WebSocket connection management
function initWebSocket() {
    const wsUrl = `ws://${window.location.hostname}/ws`;
    addActivityLog(`Connecting to hub...`, 'info');
    
    ws = new WebSocket(wsUrl);
    window.ws = ws;
    
    ws.onopen = () => {
        addActivityLog('Connected to hub', 'success');
        updateConnectionStatus(true);
        clearInterval(reconnectInterval);
        
        // Request initial data
        sendCommand({ type: 'GET_NODES', tankId: currentTankId });
        sendCommand({ type: 'GET_STATUS' });
    };
    
    ws.onclose = () => {
        addActivityLog('Disconnected from hub', 'error');
        updateConnectionStatus(false);
        
        // Attempt reconnection every 5 seconds
        if (!reconnectInterval) {
            reconnectInterval = setInterval(() => {
                addActivityLog('Attempting to reconnect...', 'warning');
                initWebSocket();
            }, 5000);
        }
    };
    
    ws.onerror = (error) => {
        addActivityLog(`WebSocket error: ${error}`, 'error');
    };
    
    ws.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            handleMessage(data);
        } catch (e) {
            addActivityLog(`Failed to parse message: ${e}`, 'error');
        }
    };
}

// Handle incoming WebSocket messages
function handleMessage(data) {
    switch(data.type) {
        case 'NODE_LIST':
            updateNodesList(data.nodes);
            break;
        case 'NODE_STATUS':
            updateNodeStatus(data.nodeType, data.status);
            break;
        case 'SENSOR_DATA':
            updateSensorData(data.data);
            break;
        case 'COMMAND_ACK':
            logMessage(`Command acknowledged: ${data.commandId}`, 'success');
            break;
        case 'HUB_STATUS':
            updateHubStatus(data.uptime);
            break;
        case 'ERROR':
            logMessage(`Error: ${data.message}`, 'error');
            break;
        default:
            logMessage(`Unknown message type: ${data.type}`, 'warning');
    }
}

// Send command to hub
function sendCommand(command) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(command));
        logMessage(`Sent: ${command.type}`);
    } else {
        logMessage('Cannot send command: not connected', 'error');
    }
}
// Update connection status indicator
function updateConnectionStatus(connected) {
    const statusDot = document.querySelector('.status-dot');
    const statusText = document.querySelector('.status-text');
    
    if (statusDot && statusText) {
        if (connected) {
            statusDot.classList.add('online');
            statusDot.classList.remove('offline');
            statusText.textContent = 'System Online';
        } else {
            statusDot.classList.remove('online');
            statusDot.classList.add('offline');
            statusText.textContent = 'System Offline';
        }
    }
}

// Update hub uptime display
function updateHubStatus(uptime) {
    const uptimeEl = document.getElementById('hub-uptime');
    const hours = Math.floor(uptime / 60);
    const minutes = uptime % 60;
    uptimeEl.textContent = `Uptime: ${hours}h ${minutes}m`;
}

// Update nodes grid
function updateNodesList(nodeList) {
    nodes = {};
    const grid = document.getElementById('nodes-grid');
    grid.innerHTML = '';
    
    nodeList.forEach(node => {
        nodes[node.type] = node;
        
        const card = document.createElement('div');
        card.className = `node-card ${node.online ? 'online' : 'offline'}`;
        card.innerHTML = `
            <h3>${getNodeIcon(node.type)} ${node.name}</h3>
            <p class="node-status ${node.online ? 'online' : 'offline'}">
                ${node.online ? '● Online' : '○ Offline'}
            </p>
            <p style="font-size: 0.8rem; color: var(--text-secondary); margin-top: 5px;">
                Last seen: ${node.lastSeen || 'Never'}
            </p>
        `;
        
        card.addEventListener('click', () => showNodePanel(node.type));
        grid.appendChild(card);
    });
}

// Update individual node status
function updateNodeStatus(nodeType, status) {
    if (nodes[nodeType]) {
        nodes[nodeType].online = status.online;
        nodes[nodeType].lastSeen = status.lastSeen;
        updateNodesList(Object.values(nodes));
    }
}

// Show control panel for selected node
function showNodePanel(nodeType) {
    // Hide all panels
    document.querySelectorAll('.control-panel').forEach(panel => {
        panel.style.display = 'none';
    });
    
    // Show selected panel
    const panelMap = {
        'LIGHT': 'lighting-panel',
        'CO2': 'co2-panel',
        'FISH_FEEDER': 'feeder-panel',
        'HEATER': 'heater-panel',
        'SENSOR': 'sensor-panel'
    };
    
    const panelId = panelMap[nodeType];
    if (panelId) {
        document.getElementById(panelId).style.display = 'block';
        logMessage(`Opened ${nodeType} control panel`);
    }
}

// Get emoji icon for node type
function getNodeIcon(nodeType) {
    const icons = {
        'LIGHT': '💡',
        'CO2': '🫧',
        'FISH_FEEDER': '🐟',
        'HEATER': '🌡️',
        'SENSOR': '💧',
        'DOSER': '💊',
        'FILTER': '🌀'
    };
    return icons[nodeType] || '📦';
}

// Update sensor data display
function updateSensorData(data) {
    if (data.ph !== undefined) {
        document.getElementById('sensor-ph').textContent = data.ph.toFixed(2);
    }
    if (data.tds !== undefined) {
        document.getElementById('sensor-tds').textContent = data.tds;
    }
    if (data.temp !== undefined) {
        document.getElementById('sensor-temp').textContent = data.temp.toFixed(1);
        document.getElementById('heater-current-temp').textContent = data.temp.toFixed(1);
    }
}

// Log message to system log
function logMessage(message, type = 'info') {
    const logContent = document.getElementById('system-log');
    if (!logContent) return; // Avoid errors on pages without a system log element
    const entry = document.createElement('p');
    entry.className = `log-entry ${type}`;
    
    const timestamp = new Date().toLocaleTimeString();
    entry.textContent = `[${timestamp}] ${message}`;
    
    logContent.appendChild(entry);
    logContent.scrollTop = logContent.scrollHeight;
    
    // Limit log entries to 100
    while (logContent.children.length > 100) {
        logContent.removeChild(logContent.firstChild);
    }
}

// Setup event listeners for UI controls
function setupEventListeners() {
    // Add any dynamic event listeners here
    // Most functionality is now handled via specific page controls
}

// Update uptime display
function updateUptime() {
    const uptimeEl = document.getElementById('uptime');
    if (uptimeEl) {
        const elapsed = Date.now() - startTime;
        const hours = Math.floor(elapsed / 3600000);
        const minutes = Math.floor((elapsed % 3600000) / 60000);
        const seconds = Math.floor((elapsed % 60000) / 1000);
        uptimeEl.textContent = `${padZero(hours)}:${padZero(minutes)}:${padZero(seconds)}`;
    }
}

function padZero(num) {
    return num.toString().padStart(2, '0');
}

// ============================================================================
// DASHBOARD API INTEGRATION
// ============================================================================

function updateSystemStats() {
    // This function is now only for hub-specific stats like memory and WiFi.
    // Aquarium and device counts are handled by loadDashboardData().
    
    // Update memory status
    fetch('/api/status')
        .then(response => response.json())
        .then(data => {
            const elem = document.getElementById('memory-status');
            if (elem && data.memory) {
                const usedPct = ((data.memory.heapUsed / data.memory.heapTotal) * 100).toFixed(1);
                elem.textContent = `${usedPct}%`;
                
                // Update badge color based on usage
                elem.className = 'badge';
                if (usedPct < 50) {
                    elem.classList.add('badge-success');
                } else if (usedPct < 75) {
                    elem.classList.add('badge-warning');
                } else {
                    elem.classList.add('badge-danger');
                }
            }
            
            // Update WiFi status (use wifi_rssi directly from API)
            const wifiElem = document.getElementById('wifi-status');
            if (wifiElem && data.wifi_rssi !== undefined) {
                const rssi = data.wifi_rssi;
                if (rssi > -50) {
                    wifiElem.textContent = 'Excellent';
                    wifiElem.className = 'badge badge-success';
                } else if (rssi > -70) {
                    wifiElem.textContent = 'Good';
                    wifiElem.className = 'badge badge-success';
                } else if (rssi > -85) {
                    wifiElem.textContent = 'Fair';
                    wifiElem.className = 'badge badge-warning';
                } else {
                    wifiElem.textContent = 'Weak';
                    wifiElem.className = 'badge badge-danger';
                }
            }
        })
        .catch(error => console.error('Error fetching status:', error));
}

// Load dashboard data from backend
function loadDashboardData() {
    // Load aquariums
    fetch('/api/aquariums')
        .then(response => response.json())
        .then(data => {
            if (data.aquariums && Array.isArray(data.aquariums)) {
                const activeAquariums = data.aquariums.filter(a => a.enabled).length;
                
                // Update dashboard stats - using correct element ID
                const aquariumCountElem = document.getElementById('aquarium-count');
                if (aquariumCountElem) aquariumCountElem.textContent = activeAquariums;
                
                // Store in localStorage for other pages
                localStorage.setItem('aquariums', JSON.stringify(data.aquariums));
            }
        })
        .catch(error => console.error('Error loading aquariums:', error));
    
    // Load devices
    fetch('/api/devices')
        .then(response => response.json())
        .then(data => {
            if (data.devices && Array.isArray(data.devices)) {
                // Use the backend-injected `online` boolean for consistency with Manage Devices
                const totalDevices = data.devices.length;
                const onlineDevices = data.devices.filter(d => d.online).length;
                const scheduleCount = data.devices.filter(d => d.schedules && d.schedules.length > 0).length;

                // Update dashboard stats
                const deviceCountElem = document.getElementById('device-count');
                if (deviceCountElem) deviceCountElem.textContent = onlineDevices;

                const scheduleCountElem = document.getElementById('schedule-count');
                if (scheduleCountElem) scheduleCountElem.textContent = scheduleCount;

                // Store in localStorage for other pages
                localStorage.setItem('devices', JSON.stringify(data.devices));
            }
        })
        .catch(error => console.error('Error loading devices:', error));
}
