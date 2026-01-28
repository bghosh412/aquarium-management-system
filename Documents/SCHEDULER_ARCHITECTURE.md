# Scheduler Architecture

## Overview

The hub scheduler controls time-based light automation via a FreeRTOS task running on Core 1. It now uses a persistent `next-task.json` file to handle power outages and missed tasks.

## Files
- **Scheduler Task**: `src/main.cpp` - `schedulerTask()` function
- **Schedule Storage**: `/config/schedule/light-schedule.json` (LittleFS)
- **Next Task Cache**: `/config/schedule/next-task.json` (LittleFS)
- **UI**: `src/hub/data/UI/device/schedule/light-schedule.html`
- **API Endpoints**: `/api/light-schedule` (GET/POST)

## Schedule JSON Format
```json
{
  "schedules": [{
    "mac": "DC:4F:22:65:9A:19",
    "tankId": 1,
    "deviceName": "Living Room Light",
    "schedule": {
      "morning": {
        "channel1": {"start": {"hour": 8, "minute": 0}, "offTime": {"hour": 11, "minute": 0}},
        "channel2": {"start": {"hour": 8, "minute": 5}, "offTime": {"hour": 10, "minute": 55}},
        "channel3": {"start": {"hour": 8, "minute": 10}, "offTime": {"hour": 10, "minute": 50}}
      },
      "evening": { ... }
    }
  }]
}
```

## Next Task JSON Format
```json
{
  "tasks": [{
    "mac": "DC:4F:22:65:9A:19",
    "channel": 1,
    "actionOn": true,
    "scheduledTime": 1706504400,
    "period": "morning"
  }],
  "updatedAt": 1706504300
}
```

## Command Codes
- `10/11`: Channel 1 OFF/ON
- `20/21`: Channel 2 OFF/ON  
- `30/31`: Channel 3 OFF/ON

## Power Outage Recovery

### Problem Solved
If power goes out before a scheduled event and returns after, the event would be missed with the old polling approach.

### Solution
1. **Persistent next-task.json**: Stores all upcoming tasks with Unix timestamps
2. **Startup Recovery**: On boot, checks for past-due tasks and executes them
3. **Node Offline Retry**: If target node is offline, retries every 60 seconds

### Update Triggers
1. **Schedule save (POST /api/light-schedule)**: Calls `rebuildNextTasks()`
2. **Task execution**: After executing, recalculates next occurrences
3. **Periodic refresh**: Every 12 hours to handle day rollover

## Scheduler Task Flow

```
START
  │
  ├── Wait for NTP sync (30s timeout)
  │
  ├── rebuildNextTasks() - Initial build from schedules
  │
  ├── loadNextTasks() - Load from file
  │
  ├── processPastDueTasks() - Execute any missed tasks
  │
  └── MAIN LOOP:
        │
        ├── Check for periodic refresh (every 12 hours)
        │
        ├── findNextTask() - Get soonest upcoming task
        │     │
        │     ├── If task is due NOW:
        │     │     ├── executeTask()
        │     │     │     ├── Check if node online
        │     │     │     ├── Send ESP-NOW command
        │     │     │     └── Update channel state tracking
        │     │     │
        │     │     ├── If success: rebuildNextTasks()
        │     │     └── If offline: retry in 60s
        │     │
        │     └── If task is in future:
        │           └── Sleep until task (max 30s)
        │
        ├── If no tasks: Sleep 30s, then rebuild
        │
        └── processPastDueTasks() - Handle any accumulated tasks
```

## Node Offline Handling

When a scheduled task targets an offline node:
1. Task is not executed
2. Scheduled time is updated to `now + 60 seconds`
3. Task is kept in the queue for retry
4. Retry continues until node comes online
5. Multiple retries are logged for debugging

## Key Functions

- `rebuildNextTasks()`: Recalculate all future tasks from schedules
- `loadNextTasks()`: Load tasks from next-task.json
- `saveNextTasks()`: Persist tasks to next-task.json
- `calculateNextTasks()`: Extract tasks from a schedule entry
- `findNextTask()`: Find soonest upcoming task
- `executeTask()`: Send command to node
- `processPastDueTasks()`: Handle all overdue tasks

## Testing

1. **Schedule Update Test**: POST to `/api/light-schedule`, verify next-task.json updated
2. **Power Outage Simulation**: Reboot hub during scheduled window, verify task executes on boot
3. **Offline Node Test**: Schedule task, take node offline, verify retry behavior
4. **Multiple Tasks Test**: Multiple devices with overlapping schedules

