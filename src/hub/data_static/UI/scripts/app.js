// Aquarium Management System - Professional Dashboard JavaScript
// Communicates with ESP32 hub via WebSocket

let ws = null;
let currentTankId = 1;
let nodes = {};
let reconnectInterval = null;
let uptimeInterval = null;

// Add activity log entry (Console only now)
function addActivityLog(message, type = 'info') {
    console.log(`[WS ${type.toUpperCase()}] ${message}`);
}

// Initialize on page load
document.addEventListener('DOMContentLoaded', () => {
    initWebSocket();
    setupEventListeners();
    loadDashboardData();  // Load data from backend
    addActivityLog('System initialized', 'success');
    
    // Update stats every 5 seconds — only on the dashboard page
    const pathname = window.location.pathname || '/';
    if (pathname === '/' || pathname.endsWith('/index.html')) {
        setInterval(() => {
            updateSystemStats();
            loadDashboardData();  // Refresh dashboard data
            fetchActivityLog();   // Refresh activity log
        }, 5000);
        fetchActivityLog();  // Initial load
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
    if (grid) grid.innerHTML = '';
    
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
    const clearBtn = document.getElementById('clear-activity-btn');
    if (clearBtn) {
        clearBtn.addEventListener('click', clearActivityLog);
    }
}

// padZero kept for any remaining callers
function padZero(num) {
    return num.toString().padStart(2, '0');
}

// ============================================================================
// DASHBOARD API INTEGRATION
// ============================================================================

function updateSystemStats() {
    // This function handles hub-specific stats like memory and WiFi.
    fetch('/api/status')
        .then(response => response.json())
        .then(data => {
            const elem = document.getElementById('memory-status');
            if (elem && data.memory) {
                const usedPct = ((data.memory.heapUsed / data.memory.heapTotal) * 100).toFixed(1);
                elem.textContent = `${usedPct}%`;
                
                elem.className = 'badge';
                if (usedPct < 50) elem.classList.add('badge-success');
                else if (usedPct < 75) elem.classList.add('badge-warning');
                else elem.classList.add('badge-danger');
            }
            
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
async function loadDashboardData() {
    updateSystemStats();
    
    // Load aquariums
    fetch('/api/aquariums')
        .then(response => response.json())
        .then(data => {
            if (data.aquariums && Array.isArray(data.aquariums)) {
                const activeAquariums = data.aquariums.filter(a => a.enabled).length;
                const aquariumCountElem = document.getElementById('aquarium-count');
                if (aquariumCountElem) aquariumCountElem.textContent = activeAquariums;
                localStorage.setItem('aquariums', JSON.stringify(data.aquariums));
            }
        })
        .catch(error => console.error('Error loading aquariums:', error));
    
    // Load devices
    fetch('/api/devices')
        .then(response => response.json())
        .then(data => {
            if (data.devices && Array.isArray(data.devices)) {
                // Use same logic as manage-devices.js
                const onlineDevices = data.devices.filter(d => d.online).length;
                const totalDevices = data.devices.length;
                const scheduleCount = data.devices.filter(d => d.schedules && d.schedules.length > 0).length;
                const offlineDevices = totalDevices - onlineDevices;

                const deviceCountElem = document.getElementById('device-count');
                if (deviceCountElem) deviceCountElem.textContent = onlineDevices;

                const scheduleCountElem = document.getElementById('schedule-count');
                if (scheduleCountElem) scheduleCountElem.textContent = scheduleCount;
                
                const alertCountElem = document.getElementById('alert-count');
                if (alertCountElem) alertCountElem.textContent = offlineDevices;

                localStorage.setItem('devices', JSON.stringify(data.devices));
            }
        })
        .catch(error => console.error('Error loading devices:', error));

    // Load tasks
    fetchUpcomingTasks();
}

async function fetchUpcomingTasks() {
    const container = document.getElementById('tasks-list');
    const countBadge = document.getElementById('task-count');
    if (!container || !countBadge) return; // Not on dashboard page

    try {
        const response = await fetch('/api/next-tasks');
        if (!response.ok) throw new Error('Tasks not found');
        const data = await response.json();
        
        if (!data.tasks || data.tasks.length === 0) {
            container.innerHTML = `
                <div style="text-align: center; padding: 2rem; color: var(--text-secondary);">
                    <div style="font-size: 2rem; margin-bottom: 0.5rem;">📅</div>
                    <p>No tasks scheduled for today.</p>
                </div>`;
            countBadge.textContent = '0 Pending';
            return;
        }

        const now = new Date();
        const currentTime = now.getHours() * 60 + now.getMinutes();

        const tasksHtml = data.tasks.map(task => {
            const timeStr = task.scheduledTime ? formatTimeFromEpoch(task.scheduledTime) : '--:--';
            const [hours, minutes] = timeStr.split(':').map(Number);
            const taskMinutes = hours * 60 + minutes;
            const isPending = taskMinutes >= currentTime;
            const desc = task.taskDesc || task.name || 'Unnamed Task';
            
            return `
                <div class="task-item">
                    <div class="task-time">${timeStr}</div>
                    <div class="task-info">
                        <div class="task-name">${desc}</div>
                        <div class="task-tank">${task.tankName || ('Tank ' + task.tankId)}</div>
                    </div>
                    <div class="task-status">
                        <span class="badge ${isPending ? 'badge-warning' : 'badge-success'}">
                            ${isPending ? 'Pending' : 'Done'}
                        </span>
                    </div>
                </div>
            `;
        }).join('');

        container.innerHTML = tasksHtml;
        const pendingCount = data.tasks.filter(t => {
            const timeStr = t.scheduledTime ? formatTimeFromEpoch(t.scheduledTime) : '00:00';
            const [h, m] = timeStr.split(':').map(Number);
            return (h * 60 + m) >= currentTime;
        }).length;
        
        countBadge.textContent = `${pendingCount} Pending`;
        
    } catch (error) {
        console.error('Error fetching tasks:', error);
    }
}

// ============================================================================
// ACTIVITY LOG
// ============================================================================

const ACTIVITY_ICONS = {
    light:     '💡',
    feeder:    '🐟',
    wavemaker: '🌊',
    device:    '📦',
};

function formatActivityTime(ts) {
    const d = new Date(ts * 1000);
    const now = new Date();
    const diffMs = now - d;
    const diffMin = Math.floor(diffMs / 60000);
    if (diffMin < 1) return 'just now';
    if (diffMin < 60) return `${diffMin}m ago`;
    const diffHr = Math.floor(diffMin / 60);
    if (diffHr < 24) return `${diffHr}h ago`;
    const diffDay = Math.floor(diffHr / 24);
    return `${diffDay}d ago`;
}

// Convert unix epoch seconds to HH:MM local time string
function formatTimeFromEpoch(sec) {
    const d = new Date(sec * 1000);
    const h = d.getHours().toString().padStart(2, '0');
    const m = d.getMinutes().toString().padStart(2, '0');
    return `${h}:${m}`;
}

async function fetchActivityLog() {
    const container = document.getElementById('activity-list');
    const badge = document.getElementById('activity-count');
    if (!container || !badge) return;

    try {
        const resp = await fetch('/api/activity-log?limit=30');
        if (!resp.ok) throw new Error('Activity log not found');
        const data = await resp.json();

        const entries = data.entries || [];
        if (entries.length === 0) {
            renderEmptyActivityState(container, badge);
            return;
        }

        // Show newest first
        const reversed = [...entries].reverse();
        badge.textContent = `${entries.length} events`;

        container.innerHTML = reversed.map(e => {
            const icon = ACTIVITY_ICONS[e.cat] || '📋';
            const srcBadge = e.src === 'scheduled'
                ? '<span class="badge badge-info" style="font-size:0.65rem;padding:2px 6px;">scheduled</span>'
                : '<span class="badge badge-warning" style="font-size:0.65rem;padding:2px 6px;">ad-hoc</span>';
            return `
                <div class="task-item">
                    <div class="task-time" style="font-size:1.1rem;min-width:32px;background:none;">${icon}</div>
                    <div class="task-info" style="flex:1;">
                        <div class="task-name">${e.action}</div>
                        <div class="task-tank">${e.device} · ${e.aquarium}</div>
                    </div>
                    <div style="text-align:right;">
                        <div style="font-size:0.75rem;color:var(--text-secondary);margin-bottom:2px;">${formatActivityTime(e.ts)}</div>
                        ${srcBadge}
                    </div>
                </div>`;
        }).join('');
    } catch (err) {
        console.error('Error fetching activity log:', err);
    }
}

function renderEmptyActivityState(container, badge) {
    container.innerHTML = `
        <div style="text-align: center; padding: 2rem; color: var(--text-secondary);">
            <div style="font-size: 2rem; margin-bottom: 0.5rem;">📋</div>
            <p>No recent activity.</p>
        </div>`;
    badge.textContent = '0 events';
}

async function clearActivityLog() {
    const clearBtn = document.getElementById('clear-activity-btn');
    const container = document.getElementById('activity-list');
    const badge = document.getElementById('activity-count');
    if (!clearBtn || !container || !badge) return;

    if (!confirm('Clear all recent activity entries?')) {
        return;
    }

    const originalText = clearBtn.textContent;
    clearBtn.disabled = true;
    clearBtn.textContent = 'Clearing...';

    try {
        const resp = await fetch('/api/activity-log', { method: 'DELETE' });
        if (!resp.ok) throw new Error('Failed to clear activity log');

        renderEmptyActivityState(container, badge);
        addActivityLog('Recent activity cleared', 'success');
        await fetchActivityLog();
    } catch (err) {
        console.error('Error clearing activity log:', err);
        alert('Unable to clear activity log. Please try again.');
    } finally {
        clearBtn.disabled = false;
        clearBtn.textContent = originalText;
    }
}
