let scheduleTasks = [];
let sortedScheduleTasks = [];
let devicesCache = [];
let aquariumsCache = [];
let lastUpdatedAt = 0;
let selectedTaskIndices = new Set();

document.addEventListener('DOMContentLoaded', () => {
    const refreshBtn = document.getElementById('refreshSchedules');
    if (refreshBtn) {
        refreshBtn.addEventListener('click', () => loadAllSchedules());
    }
    
    const selectAllCheckbox = document.getElementById('selectAllCheckbox');
    if (selectAllCheckbox) {
        selectAllCheckbox.addEventListener('change', (e) => {
            toggleSelectAll(e.target.checked);
        });
    }
    
    const deleteSelectedBtn = document.getElementById('deleteSelectedBtn');
    if (deleteSelectedBtn) {
        deleteSelectedBtn.addEventListener('click', () => deleteSelectedTasks());
    }
    
    loadAllSchedules();
});

async function loadAllSchedules() {
    const [tasksData, devicesData, aquariumsData] = await Promise.all([
        fetchJson('/config/schedule/next-task.json', { tasks: [], updatedAt: 0 }),
        fetchJson('/api/devices', { devices: [] }),
        fetchJson('/api/aquariums', { aquariums: [] })
    ]);

    scheduleTasks = Array.isArray(tasksData.tasks) ? tasksData.tasks : [];
    devicesCache = Array.isArray(devicesData.devices) ? devicesData.devices : [];
    aquariumsCache = Array.isArray(aquariumsData.aquariums) ? aquariumsData.aquariums : [];
    lastUpdatedAt = tasksData.updatedAt || 0;

    localStorage.setItem('nextTasks', JSON.stringify(tasksData));
    localStorage.setItem('devices', JSON.stringify(devicesCache));
    localStorage.setItem('aquariums', JSON.stringify(aquariumsCache));

    // Clear selection on refresh
    selectedTaskIndices.clear();
    updateSelectionUI();
    
    renderScheduleTable(lastUpdatedAt);
}

async function fetchJson(url, fallback) {
    try {
        const response = await fetch(url, { cache: 'no-store' });
        if (!response.ok) throw new Error('Request failed');
        return await response.json();
    } catch (error) {
        if (url.includes('next-task')) {
            return JSON.parse(localStorage.getItem('nextTasks') || JSON.stringify(fallback));
        }
        if (url.includes('/api/devices')) {
            return { devices: JSON.parse(localStorage.getItem('devices') || '[]') };
        }
        if (url.includes('/api/aquariums')) {
            return { aquariums: JSON.parse(localStorage.getItem('aquariums') || '[]') };
        }
        return fallback;
    }
}

function renderScheduleTable(updatedAt) {
    const tbody = document.getElementById('scheduleTableBody');
    const emptyState = document.getElementById('emptyState');
    const badge = document.getElementById('scheduleBadge');
    const countElem = document.getElementById('taskCount');
    const updatedElem = document.getElementById('tasksUpdated');
    const selectAllCheckbox = document.getElementById('selectAllCheckbox');

    const tasks = [...scheduleTasks].sort((a, b) => (a.scheduledTime || 0) - (b.scheduledTime || 0));
    sortedScheduleTasks = tasks;
    const devicesByMac = new Map(devicesCache.map(d => [String(d.mac || '').toLowerCase(), d]));
    const aquariumsById = new Map(aquariumsCache.map(a => [String(a.id), a]));

    if (countElem) countElem.textContent = tasks.length;
    if (badge) badge.textContent = tasks.length + ' task' + (tasks.length === 1 ? '' : 's');
    if (updatedElem) updatedElem.textContent = updatedAt ? formatDateTime(updatedAt * 1000) : '--';
    if (selectAllCheckbox) selectAllCheckbox.checked = false;

    if (!tbody) return;

    if (tasks.length === 0) {
        tbody.innerHTML = '';
        if (emptyState) emptyState.style.display = 'block';
        return;
    }

    if (emptyState) emptyState.style.display = 'none';

    tbody.innerHTML = tasks.map((task, index) => {
        const mac = String(task.mac || '').toLowerCase();
        const device = devicesByMac.get(mac);
        const aquarium = device ? aquariumsById.get(String(device.tankId)) : null;
        const aquariumName = aquarium ? aquarium.name : (device ? 'Tank ' + device.tankId : 'Unknown Aquarium');
        const deviceName = device ? device.name : (task.mac || 'Unknown Device');
        const statusText = device ? (device.online ? 'Online' : 'Offline') : 'Unknown';
        const statusClass = device ? (device.online ? 'online' : 'offline') : 'offline';
        const detailText = formatTaskDetails(task);
        const scheduleLink = buildScheduleLink(task, device);
        const scheduleId = task.scheduleId || '--';
        const isSelected = selectedTaskIndices.has(index);

        return '<tr data-index="' + index + '">' +
            '<td><input type="checkbox" class="task-checkbox" data-index="' + index + '" ' + (isSelected ? 'checked' : '') + ' onchange="toggleTaskSelection(' + index + ', this.checked)"></td>' +
            '<td>' + escapeHtml(aquariumName) + '</td>' +
            '<td>' + escapeHtml(deviceName) + '</td>' +
            '<td><span class="status-badge ' + statusClass + '">' + statusText + '</span></td>' +
            '<td><div>' + escapeHtml(detailText) + '</div><div style="font-size: 0.75rem; color: var(--color-text-secondary);">ID: ' + escapeHtml(scheduleId) + '</div></td>' +
            '<td><div style="display: flex; gap: 0.5rem;">' +
                '<button class="btn-icon" title="Delete task" onclick="deleteScheduleTask(' + index + ')">' +
                    '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' +
                        '<polyline points="3 6 5 6 21 6"/>' +
                        '<path d="M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/>' +
                        '<path d="M10 11v6"/><path d="M14 11v6"/>' +
                        '<path d="M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/>' +
                    '</svg>' +
                '</button>' +
                (scheduleLink ? '<a class="btn-icon" title="Open device schedule" href="' + scheduleLink + '">' +
                    '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' +
                        '<rect x="3" y="4" width="18" height="18" rx="2"/>' +
                        '<line x1="16" y1="2" x2="16" y2="6"/><line x1="8" y1="2" x2="8" y2="6"/>' +
                        '<line x1="3" y1="10" x2="21" y2="10"/>' +
                    '</svg></a>' : '') +
            '</div></td></tr>';
    }).join('');
}

function toggleTaskSelection(index, checked) {
    if (checked) {
        selectedTaskIndices.add(index);
    } else {
        selectedTaskIndices.delete(index);
    }
    updateSelectionUI();
}

function toggleSelectAll(checked) {
    selectedTaskIndices.clear();
    if (checked) {
        for (let i = 0; i < sortedScheduleTasks.length; i++) {
            selectedTaskIndices.add(i);
        }
    }
    
    const checkboxes = document.querySelectorAll('.task-checkbox');
    checkboxes.forEach(cb => cb.checked = checked);
    
    updateSelectionUI();
}

function updateSelectionUI() {
    const deleteBtn = document.getElementById('deleteSelectedBtn');
    const countSpan = document.getElementById('selectedCount');
    const selectAllCheckbox = document.getElementById('selectAllCheckbox');
    
    const count = selectedTaskIndices.size;
    
    if (deleteBtn) {
        deleteBtn.style.display = count > 0 ? 'inline-flex' : 'none';
    }
    if (countSpan) {
        countSpan.textContent = count;
    }
    if (selectAllCheckbox) {
        selectAllCheckbox.checked = count > 0 && count === sortedScheduleTasks.length;
        selectAllCheckbox.indeterminate = count > 0 && count < sortedScheduleTasks.length;
    }
}

async function deleteSelectedTasks() {
    if (selectedTaskIndices.size === 0) return;
    
    const count = selectedTaskIndices.size;
    const confirmed = confirm('Delete ' + count + ' selected task' + (count > 1 ? 's' : '') + '? This will remove them from the schedule queue and rebuild tasks from the source schedules.');
    if (!confirmed) return;
    
    const scheduleIdsToDelete = [];
    for (const index of selectedTaskIndices) {
        const task = sortedScheduleTasks[index];
        if (task && task.scheduleId) {
            scheduleIdsToDelete.push(task.scheduleId);
        }
    }
    
    try {
        const response = await fetch('/api/next-task/delete-selected', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ scheduleIds: scheduleIdsToDelete })
        });
        
        if (!response.ok) {
            throw new Error('Delete request failed');
        }
        
        const result = await response.json();
        if (result.success) {
            selectedTaskIndices.clear();
            await loadAllSchedules();
        } else {
            alert('Failed to delete tasks: ' + (result.error || 'Unknown error'));
        }
    } catch (error) {
        console.error('Delete error:', error);
        alert('Failed to delete tasks. Please try again.');
    }
}

function buildScheduleLink(task, device) {
    const mac = task && task.mac;
    if (!mac) return '';
    const type = task.taskType;
    const deviceType = String(device && device.type || '').toLowerCase();

    if (type === 1 || deviceType === 'feeder' || deviceType === 'fish_feeder') {
        return '/device/schedule/feeder-schedule.html?mac=' + encodeURIComponent(mac);
    }
    return '/device/schedule/light-schedule.html?mac=' + encodeURIComponent(mac);
}

function formatTaskDetails(task) {
    if (!task) return 'Unknown task';

    const when = task.scheduledTime ? formatDateTime(task.scheduledTime * 1000) : 'Unknown time';
    if (task.taskType === 1) {
        const pwm = task.pwmValue || 0;
        const duration = task.durationMs || 0;
        return 'Feeder • PWM ' + pwm + ' • ' + duration + ' ms • ' + when;
    }

    const channel = task.channel || '-';
    const action = task.actionOn ? 'ON' : 'OFF';
    const period = task.period ? task.period.charAt(0).toUpperCase() + task.period.slice(1) : 'Schedule';
    return period + ' • Channel ' + channel + ' • ' + action + ' • ' + when;
}

function formatDateTime(timestampMs) {
    try {
        return new Date(timestampMs).toLocaleString();
    } catch (error) {
        return '--';
    }
}

function escapeHtml(value) {
    return String(value)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, "&#39;");
}

async function deleteScheduleTask(index) {
    const task = sortedScheduleTasks[index];
    if (!task) return;

    const confirmed = confirm('Delete this scheduled task? It will be removed from the queue.');
    if (!confirmed) return;

    try {
        const response = await fetch('/api/next-task/delete', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ scheduleId: task.scheduleId })
        });

        if (!response.ok) {
            throw new Error('Delete request failed');
        }

        const result = await response.json();
        if (result.success) {
            scheduleTasks = scheduleTasks.filter(t => t.scheduleId !== task.scheduleId);
            selectedTaskIndices.clear();
            renderScheduleTable(lastUpdatedAt);
        } else {
            alert('Failed to delete task: ' + (result.error || 'Unknown error'));
        }
    } catch (error) {
        console.error('Delete error:', error);
        alert('Failed to delete task. Please try again.');
    }
}
