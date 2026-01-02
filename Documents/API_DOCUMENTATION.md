# Aquarium Management System - REST API Documentation

## Overview

The hub exposes a RESTful API for managing aquariums, devices, and schedules. All endpoints return JSON unless otherwise specified.

**Base URL**: `http://<hub-ip>/api` or `http://ams.local/api`

---

## Aquarium Endpoints

### 1. Get All Aquariums

**GET** `/api/aquariums`

Returns a list of all aquariums in the system.

**Response**:
```json
{
  "aquariums": [
    {
      "id": 1,
      "name": "Living Room Tank",
      "volumeLiters": 100,
      "tankType": "Planted",
      "location": "Living Room",
      "enabled": true,
      "deviceCount": 5,
      "waterParameters": {
        "temperature": { "min": 24, "max": 26 },
        "ph": { "min": 6.5, "max": 7.5 },
        "tds": { "min": 150, "max": 300 }
      },
      "currentReadings": {
        "temperature": 25.2,
        "ph": 7.1,
        "tds": 220
      }
    }
  ]
}
```

---

### 2. Get Single Aquarium

**GET** `/api/aquariums/{id}`

Returns details of a specific aquarium.

**Parameters**:
- `id` (path): Aquarium ID (1-255)

**Response**:
```json
{
  "id": 1,
  "name": "Living Room Tank",
  "volumeLiters": 100,
  "tankType": "Planted",
  "location": "Living Room",
  "description": "Main display tank",
  "enabled": true,
  "deviceCount": 5,
  "waterParameters": {
    "temperature": { "min": 24, "max": 26 },
    "ph": { "min": 6.5, "max": 7.5 },
    "tds": { "min": 150, "max": 300 }
  },
  "currentReadings": {
    "temperature": 25.2,
    "ph": 7.1,
    "tds": 220,
    "lastUpdate": 1234567890
  }
}
```

**Status Codes**:
- `200 OK`: Success
- `404 Not Found`: Aquarium not found

---

### 3. Create New Aquarium

**POST** `/api/aquariums`

Creates a new aquarium and saves it to the JSON file.

**Request Body**:
```json
{
  "name": "Living Room Tank",
  "volumeLiters": 100,
  "tankType": "Planted",
  "location": "Living Room",
  "description": "Main display tank (optional)",
  "thresholds": {
    "temperature": { "min": 24, "max": 26 },
    "ph": { "min": 6.5, "max": 7.5 },
    "tds": { "min": 150, "max": 300 }
  }
}
```

**Required Fields**:
- `name`: Aquarium name (string)
- `volumeLiters`: Tank volume (number)

**Optional Fields**:
- `tankType`: Type of tank (string) - e.g., "Planted", "Cichlids", "Shrimp"
- `location`: Physical location (string)
- `description`: Additional description (string)
- `thresholds`: Water parameter thresholds (object)

**Response**:
```json
{
  "success": true,
  "id": 3,
  "message": "Aquarium created successfully"
}
```

**Status Codes**:
- `201 Created`: Success
- `400 Bad Request`: Invalid JSON or missing required fields
- `507 Insufficient Storage`: No available IDs (max 255 aquariums)
- `500 Internal Server Error`: Failed to add to manager

---

### 4. Delete Aquarium

**DELETE** `/api/aquariums/{id}`

Deletes an aquarium and all associated devices.

**Parameters**:
- `id` (path): Aquarium ID (1-255)

**Response**:
```
Aquarium deleted successfully
```

**Status Codes**:
- `200 OK`: Success
- `404 Not Found`: Aquarium not found

---

## System Endpoints

### 1. System Status

**GET** `/api/status`

Returns hub system status.

**Response**:
```json
{
  "uptime": 123456,
  "heap_free": 234567,
  "psram_free": 5234567,
  "wifi_rssi": -45
}
```

---

### 2. Reboot Hub

**POST** `/api/reboot`

Reboots the hub controller.

**Response**:
```
Rebooting...
```

**Status Codes**:
- `200 OK`: Reboot initiated

---

## Error Responses

All error responses follow this format:

**Status**: 4xx or 5xx
**Body**: Plain text error message

Example:
```
Missing required fields: name, volumeLiters
```

---

## Data Persistence

- All aquarium data is stored in `/config/aquariums.json` on the ESP32 LittleFS filesystem
- Changes are saved immediately after create/update/delete operations
- Data persists across reboots
- Backup the filesystem before major updates

---

## Web Interface Integration

The web interface (`add-new-aquarium.html`) uses these API endpoints:

```javascript
// Create aquarium
fetch('/api/aquariums', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(aquariumData)
})
.then(response => response.json())
.then(data => {
    console.log('Created aquarium with ID:', data.id);
});
```

---

## Future Enhancements

- PUT `/api/aquariums/{id}` - Update existing aquarium
- GET `/api/aquariums/{id}/devices` - Get devices for aquarium
- POST `/api/aquariums/{id}/devices` - Add device to aquarium
- WebSocket support for real-time updates

---

**Last Updated**: January 2, 2026  
**API Version**: 1.0  
**Hub Firmware**: ESP32-S3 Central Controller
