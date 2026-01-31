// Device Schedule Page JavaScript

let deviceMac = null;
let deviceData = null;
let schedules = [];
let editingScheduleIndex = null;

function deviceScheduleInit() {
    const params = new URLSearchParams(window.location.search);
    deviceMac = params.get('mac') || localStorage.getItem('selectedDeviceMac');

    if (!deviceMac) {
        // Try to pick the first device from localStorage
        const devices = JSON.parse(localStorage.getItem('devices') || '[]');
        if (devices.length > 0) {
            deviceMac = devices[0].mac;
            localStorage.setItem('selectedDeviceMac', deviceMac);
        } else {
            // No devices available — show friendly placeholder and disable controls
            document.getElementById('deviceName').textContent = 'No Device Selected';
            document.getElementById('tankId').textContent = '-';
            document.getElementById('deviceType').textContent = 'N/A';
            const badge = document.getElementById('statusBadge');
            badge.className = 'badge badge-offline';
            badge.textContent = 'No device';

            // Disable Add Schedule button in navbar
            const addBtn = document.querySelector('.navbar-actions .btn-primary');
            if (addBtn) addBtn.setAttribute('disabled', 'true');

            // Show empty schedules CTA
            const empty = document.getElementById('emptySchedules');
            if (empty) {
                empty.style.display = 'block';
                empty.querySelector('h3').textContent = 'No Device Selected';
                empty.querySelector('p').textContent = 'Add a device first or select one from Devices to create schedules.';
                const cta = empty.querySelector('button');
                if (cta) {
                    cta.textContent = 'Add Device';
                    cta.onclick = () => window.location.href = '../../device/add-device.html';
                }
            }

            // Clear templates and schedules
            document.getElementById('templatesList').innerHTML = '';
            document.getElementById('schedulesList').innerHTML = '';
            document.getElementById('scheduleCount').textContent = '0 schedules';

            // Disable interactive elements on the page
            document.querySelectorAll('button').forEach(b => b.setAttribute('disabled', 'true'));
            document.querySelectorAll('input, select').forEach(el => el.setAttribute('disabled', 'true'));

            return;
        }
    }

    loadDeviceData();
    loadSchedules();
    loadTemplates();
    setupForm();

    // Request device and schedule data from hub
    if (window.ws && window.ws.readyState === WebSocket.OPEN) {
        sendCommand({
            type: 'getDevice',
            mac: deviceMac
        });
        sendCommand({
            type: 'getSchedules',
            mac: deviceMac
        });
    }
}

document.addEventListener('DOMContentLoaded', deviceScheduleInit);

function loadDeviceData() {
    const devices = JSON.parse(localStorage.getItem('devices') || '[]');
    deviceData = devices.find(d => d.mac === deviceMac);
    
    if (deviceData) {
        displayDeviceInfo();
    }
}

function displayDeviceInfo() {
    document.getElementById('deviceName').textContent = deviceData.name;
    document.getElementById('tankId').textContent = deviceData.tankId;
    document.getElementById('deviceType').textContent = getDeviceTypeName(deviceData.type);
    
    const badge = document.getElementById('statusBadge');
    if (deviceData.online) {
        badge.className = 'badge badge-online';
        badge.textContent = 'Online';
    } else {
        badge.className = 'badge badge-offline';
        badge.textContent = 'Offline';
    }
}

function loadSchedules() {
    schedules = deviceData?.schedules || [];
    renderSchedules();
}

function renderSchedules() {
    const list = document.getElementById('schedulesList');
    const empty = document.getElementById('emptySchedules');
    const count = document.getElementById('scheduleCount');
    
    count.textContent = `${schedules.length} schedule${schedules.length !== 1 ? 's' : ''}`;
    
    if (schedules.length === 0) {
        list.style.display = 'none';
        empty.style.display = 'block';
        return;
    }
    
    list.style.display = 'block';
    empty.style.display = 'none';
    
    list.innerHTML = schedules.map((schedule, index) => `
        <div class="card" style="margin-bottom: 1rem;">
            <div class="card-body" style="display: flex; justify-content: space-between; align-items: center; gap: 1rem; flex-wrap: wrap;">
                <div style="flex: 1;">
                    <h4 style="margin: 0 0 0.5rem;">${getScheduleIcon(schedule.type)} ${schedule.name}</h4>
                    <div style="color: var(--color-text-secondary); font-size: 0.875rem;">
                        ${getScheduleDescription(schedule)}
                    </div>
                    <div style="margin-top: 0.5rem;">
                        <span class="badge ${schedule.enabled ? 'badge-online' : 'badge-offline'}">
                            ${schedule.enabled ? 'Active' : 'Disabled'}
                        </span>
                    </div>
                </div>
                <div style="display: flex; gap: 0.5rem;">
                    <button class="btn btn-secondary" onclick="toggleSchedule(${index})">
                        ${schedule.enabled ? '⏸️ Disable' : '▶️ Enable'}
                    </button>
                    <button class="btn btn-secondary" onclick="editSchedule(${index})">
                        ✏️ Edit
                    </button>
                    <button class="btn btn-danger" onclick="deleteSchedule(${index})">
                        🗑️
                    </button>
                </div>
            </div>
        </div>
    `).join('');
}

function getScheduleIcon(type) {
    const icons = {
        'daily': '📅',
        'weekly': '📆',
        'interval': '⏱️',
        'onetime': '🎯'
    };
    return icons[type] || '📋';
}

function getScheduleDescription(schedule) {
    switch (schedule.type) {
        case 'daily':
            return `Every day at ${schedule.time}`;
        case 'weekly':
            const days = schedule.days.map(d => ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'][d]).join(', ');
            return `${days} at ${schedule.time}`;
        case 'interval':
            return `Every ${schedule.intervalValue} ${schedule.intervalUnit}`;
        case 'onetime':
            return `Once on ${schedule.date} at ${schedule.time}`;
        default:
            return 'Unknown schedule type';
    }
}

function loadTemplates() {
    const templates = getTemplatesForDevice(deviceData?.type);
    const list = document.getElementById('templatesList');
    
    list.innerHTML = templates.map(template => `
        <div class="card" style="transition: all 0.15s ease;">
            <div class="card-body">
                <div style="display:flex; justify-content:space-between; align-items:center;">
                    <div>
                        <div style="font-size:1.5rem; margin-bottom:0.25rem;">${template.icon}</div>
                        <h4 style="margin:0 0 0.25rem;">${template.name}</h4>
                        <p style="margin:0; font-size:0.875rem; color:var(--color-text-secondary);">${template.description}</p>
                    </div>
                    <div style="display:flex; flex-direction:column; gap:0.5rem;">
                        <button class="btn btn-secondary" onclick="useTemplateInline('${template.id}'); return false;">Use</button>
                        <button class="btn btn-secondary" onclick="applyTemplate('${template.id}'); return false;">Apply</button>
                    </div>
                </div>
            </div>
        </div>
    `).join('');
}

function useTemplateInline(templateId) {
    const templates = getTemplatesForDevice(deviceData?.type);
    const template = templates.find(t => t.id === templateId);
    if (template) prefillInlineFromTemplate(template);
}

function prefillInlineFromTemplate(template) {
    document.getElementById('inlineScheduleName').value = template.name;
    document.getElementById('inlineScheduleType').value = template.type || 'daily';
    updateInlineScheduleFields();
    if (template.time) document.getElementById('inlineScheduleTime').value = template.time;
    if (template.action) document.getElementById('inlineScheduleAction').value = template.action;
    document.getElementById('inlineScheduleEnabled').checked = true;
    document.getElementById('inlineScheduleForm').scrollIntoView({behavior:'smooth'});
}

function getTemplatesForDevice(type) {
    const templates = {
        'light': [
            { id: 'sunrise', icon: '🌅', name: 'Sunrise/Sunset', description: 'Gradual light changes morning and evening' },
            { id: 'daytime', icon: '☀️', name: 'Daytime Only', description: 'Lights on during day, off at night' },
            { id: 'moonlight', icon: '🌙', name: 'Moonlight Cycle', description: 'Dim blue lights at night' }
        ],
        'co2': [
            { id: 'photoperiod', icon: '🌱', name: 'Photoperiod Sync', description: 'CO₂ on during light hours' },
            { id: 'timed', icon: '⏰', name: 'Timed Injection', description: 'Fixed injection periods' }
        ],
        'feeder': [
            { id: 'twice', icon: '🐟', name: 'Twice Daily', description: 'Morning and evening feeding' },
            { id: 'thrice', icon: '🐠', name: 'Three Times', description: 'Three feedings per day' }
        ],
        'heater': [
            { id: 'maintain', icon: '🔥', name: 'Temperature Maintain', description: 'Automatic temperature control' }
        ]
    };
    
    return templates[type] || [];
}

function setupForm() {
    const form = document.getElementById('addScheduleForm');
    form.addEventListener('submit', (e) => {
        e.preventDefault();
        createSchedule();
    });

    // Inline form setup
    const inlineForm = document.getElementById('inlineScheduleForm');
    if (inlineForm) {
        document.getElementById('inlineScheduleType').addEventListener('change', updateInlineScheduleFields);
        inlineForm.addEventListener('submit', (e) => {
            e.preventDefault();
            createScheduleInline();
        });
    }
}

function updateScheduleFields() {
    const type = document.getElementById('scheduleType').value;
    
    // Hide all conditional fields
    document.getElementById('timeFields').style.display = 'none';
    document.getElementById('weeklyFields').style.display = 'none';
    document.getElementById('intervalFields').style.display = 'none';
    
    // Show relevant fields
    if (type === 'daily' || type === 'weekly' || type === 'onetime') {
        document.getElementById('timeFields').style.display = 'block';
    }
    
    if (type === 'weekly') {
        document.getElementById('weeklyFields').style.display = 'block';
    }
    
    if (type === 'interval') {
        document.getElementById('intervalFields').style.display = 'block';
    }
    
    // Update action options based on device type
    updateActionOptions();
}

function updateInlineScheduleFields() {
    const type = document.getElementById('inlineScheduleType').value;

    document.getElementById('inlineTimeFields').style.display = 'none';
    document.getElementById('inlineWeeklyFields').style.display = 'none';
    document.getElementById('inlineIntervalFields').style.display = 'none';

    if (type === 'daily' || type === 'weekly' || type === 'onetime') {
        document.getElementById('inlineTimeFields').style.display = 'block';
    }
    if (type === 'weekly') {
        document.getElementById('inlineWeeklyFields').style.display = 'block';
    }
    if (type === 'interval') {
        document.getElementById('inlineIntervalFields').style.display = 'block';
    }

    // Update action options for inline form
    const actions = getActionsForDevice(deviceData?.type);
    const select = document.getElementById('inlineScheduleAction');
    select.innerHTML = '<option value="">Select action...</option>' + actions.map(action => `<option value="${action.value}">${action.label}</option>`).join('');
}

function createScheduleInline() {
    const type = document.getElementById('inlineScheduleType').value;
    const name = document.getElementById('inlineScheduleName').value;
    if (!type || !name) {
        showNotification('Please fill schedule type and name', 'error');
        return;
    }

    const schedule = {
        name: name,
        type: type,
        action: document.getElementById('inlineScheduleAction').value,
        enabled: document.getElementById('inlineScheduleEnabled').checked
    };

    if (type === 'daily' || type === 'weekly' || type === 'onetime') {
        schedule.time = document.getElementById('inlineScheduleTime').value;
    }

    if (type === 'weekly') {
        const days = Array.from(document.querySelectorAll('input[name="inlineDayOfWeek"]:checked'))
            .map(cb => parseInt(cb.value));
        if (days.length === 0) {
            showNotification('Please select at least one day', 'error');
            return;
        }
        schedule.days = days;
    }

    if (type === 'interval') {
        schedule.intervalValue = parseInt(document.getElementById('inlineIntervalValue').value);
        schedule.intervalUnit = document.getElementById('inlineIntervalUnit').value;
    }

    if (editingScheduleIndex !== null) {
        // Update existing schedule
        schedules[editingScheduleIndex] = schedule;
        // Persist
        if (deviceData) {
            deviceData.schedules = schedules;
            const devices = JSON.parse(localStorage.getItem('devices') || '[]');
            const idx = devices.findIndex(d => d.mac === deviceMac);
            if (idx !== -1) { devices[idx] = deviceData; localStorage.setItem('devices', JSON.stringify(devices)); }
        }
        // Send update to hub
        if (window.ws && window.ws.readyState === WebSocket.OPEN) {
            sendCommand({ type: 'updateSchedules', mac: deviceMac, schedules });
        }
        editingScheduleIndex = null;
        const submitBtn = document.querySelector('#inlineScheduleForm button[type="submit"]');
        if (submitBtn) submitBtn.textContent = 'Create';
        showNotification('Schedule updated', 'success');
    } else {
        addSchedule(schedule);
        showNotification('Schedule created', 'success');
    }
    document.getElementById('inlineScheduleForm').reset();
    updateInlineScheduleFields();
}

function addSchedule(schedule) {
    // Add schedule to local list and persist
    schedules.push(schedule);
    if (deviceData) {
        deviceData.schedules = schedules;
        const devices = JSON.parse(localStorage.getItem('devices') || '[]');
        const index = devices.findIndex(d => d.mac === deviceMac);
        if (index !== -1) {
            devices[index] = deviceData;
            localStorage.setItem('devices', JSON.stringify(devices));
        }
    }

    // Send to hub
    if (window.ws && window.ws.readyState === WebSocket.OPEN) {
        sendCommand({ type: 'addSchedule', mac: deviceMac, schedule });
    }

    renderSchedules();
}

function updateActionOptions() {
    const select = document.getElementById('scheduleAction');
    const actions = getActionsForDevice(deviceData?.type);
    
    select.innerHTML = '<option value="">Select action...</option>' + 
        actions.map(action => `<option value="${action.value}">${action.label}</option>`).join('');
}

function getActionsForDevice(type) {
    const actions = {
        'light': [
            { value: 'on', label: 'Turn On' },
            { value: 'off', label: 'Turn Off' },
            { value: 'preset_morning', label: 'Apply Morning Preset' },
            { value: 'preset_noon', label: 'Apply Noon Preset' },
            { value: 'preset_evening', label: 'Apply Evening Preset' }
        ],
        'co2': [
            { value: 'start', label: 'Start Injection' },
            { value: 'stop', label: 'Stop Injection' }
        ],
        'feeder': [
            { value: 'feed_1', label: 'Feed 1 Portion' },
            { value: 'feed_2', label: 'Feed 2 Portions' },
            { value: 'feed_3', label: 'Feed 3 Portions' }
        ],
        'heater': [
            { value: 'on', label: 'Turn On' },
            { value: 'off', label: 'Turn Off' },
            { value: 'auto', label: 'Auto Mode' }
        ]
    };
    
    return actions[type] || [{ value: 'trigger', label: 'Trigger Action' }];
}

function openAddScheduleModal() {
    document.getElementById('addScheduleModal').style.display = 'block';
    updateActionOptions();
}

function closeAddScheduleModal() {
    document.getElementById('addScheduleModal').style.display = 'none';
    document.getElementById('addScheduleForm').reset();
}

function createSchedule() {
    const type = document.getElementById('scheduleType').value;
    const schedule = {
        name: document.getElementById('scheduleName').value,
        type: type,
        action: document.getElementById('scheduleAction').value,
        enabled: document.getElementById('scheduleEnabled').checked
    };
    
    // Add type-specific fields
    if (type === 'daily' || type === 'weekly' || type === 'onetime') {
        schedule.time = document.getElementById('scheduleTime').value;
    }
    
    if (type === 'weekly') {
        const days = Array.from(document.querySelectorAll('input[name="dayOfWeek"]:checked'))
            .map(cb => parseInt(cb.value));
        if (days.length === 0) {
            showNotification('Please select at least one day', 'error');
            return;
        }
        schedule.days = days;
    }
    
    if (type === 'interval') {
        schedule.intervalValue = parseInt(document.getElementById('intervalValue').value);
        schedule.intervalUnit = document.getElementById('intervalUnit').value;
    }
    
    // Add schedule
    schedules.push(schedule);
    
    // Update device
    if (deviceData) {
        deviceData.schedules = schedules;
        const devices = JSON.parse(localStorage.getItem('devices') || '[]');
        const index = devices.findIndex(d => d.mac === deviceMac);
        if (index !== -1) {
            devices[index] = deviceData;
            localStorage.setItem('devices', JSON.stringify(devices));
        }
    }
    
    // Send to hub
    if (window.ws && window.ws.readyState === WebSocket.OPEN) {
        sendCommand({
            type: 'addSchedule',
            mac: deviceMac,
            schedule: schedule
        });
    }
    
    closeAddScheduleModal();
    renderSchedules();
    showNotification('Schedule created successfully', 'success');
}

function toggleSchedule(index) {
    schedules[index].enabled = !schedules[index].enabled;
    saveSchedules();
    renderSchedules();
}

function editSchedule(index) {
    // Edit using inline form where available
    const schedule = schedules[index];
    const inlineForm = document.getElementById('inlineScheduleForm');
    if (inlineForm) {
        // Prefill inline form
        document.getElementById('inlineScheduleName').value = schedule.name;
        document.getElementById('inlineScheduleType').value = schedule.type;
        updateInlineScheduleFields();
        document.getElementById('inlineScheduleAction').value = schedule.action;
        document.getElementById('inlineScheduleEnabled').checked = schedule.enabled;

        if (schedule.time) document.getElementById('inlineScheduleTime').value = schedule.time;
        if (schedule.days) {
            document.querySelectorAll('input[name="inlineDayOfWeek"]').forEach(cb => cb.checked = schedule.days.includes(parseInt(cb.value)));
        }
        if (schedule.intervalValue) document.getElementById('inlineIntervalValue').value = schedule.intervalValue;
        if (schedule.intervalUnit) document.getElementById('inlineIntervalUnit').value = schedule.intervalUnit;

        editingScheduleIndex = index;
        // Update button text to 'Update'
        const submitBtn = inlineForm.querySelector('button[type="submit"]');
        if (submitBtn) submitBtn.textContent = 'Update';
        inlineForm.scrollIntoView({ behavior: 'smooth' });
    } else {
        // Fallback: use modal behavior (existing)
        document.getElementById('scheduleName').value = schedule.name;
        document.getElementById('scheduleType').value = schedule.type;
        document.getElementById('scheduleAction').value = schedule.action;
        document.getElementById('scheduleEnabled').checked = schedule.enabled;

        if (schedule.time) {
            document.getElementById('scheduleTime').value = schedule.time;
        }

        updateScheduleFields();
        openAddScheduleModal();

        // Remove old schedule (will be replaced)
        schedules.splice(index, 1);
    }
}

function deleteSchedule(index) {
    if (!confirm('Delete this schedule?')) {
        return;
    }
    
    schedules.splice(index, 1);
    saveSchedules();
    renderSchedules();
    showNotification('Schedule deleted', 'success');
}

function applyTemplate(templateId) {
    // Template logic would go here
    showNotification('Template applied - configure details', 'success');
    openAddScheduleModal();
}

function saveSchedules() {
    if (deviceData) {
        deviceData.schedules = schedules;
        const devices = JSON.parse(localStorage.getItem('devices') || '[]');
        const index = devices.findIndex(d => d.mac === deviceMac);
        if (index !== -1) {
            devices[index] = deviceData;
            localStorage.setItem('devices', JSON.stringify(devices));
        }
    }
    
    // Send to hub
    if (window.ws && window.ws.readyState === WebSocket.OPEN) {
        sendCommand({
            type: 'updateSchedules',
            mac: deviceMac,
            schedules: schedules
        });
    }
}

function getDeviceTypeName(type) {
    const names = {
        'light': 'Light Controller',
        'co2': 'CO₂ Regulator',
        'heater': 'Heater',
        'feeder': 'Fish Feeder',
        'sensor': 'Water Quality Sensor',
        'repeater': 'Repeater'
    };
    return names[type] || 'Unknown';
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
