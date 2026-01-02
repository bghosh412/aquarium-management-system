# Navigation Map - Aquarium Management System UI

## 📍 Page Structure & Routes

### Main Sections

1. **Dashboard** (`index.html`)
   - Homepage with overview stats
   - Quick action links to add aquarium/device
   
2. **Aquariums** (`aquarium/aquarium-selection.html`)
   - View all aquariums
   - Links to manage individual aquariums
   
3. **Devices** (`device/manage-devices.html`)
   - View and manage all devices
   - Links to device setup and configuration

---

## 🗺️ Complete Navigation Routes

### Dashboard (Root Level)

**File**: `/UI/index.html`

**Sidebar Navigation**:
- 🏠 Dashboard → `index.html` (active)
- 🐠 Aquariums → `aquarium/aquarium-selection.html`
- 🔌 Devices → `device/manage-devices.html`

**Quick Actions**:
- ➕ Add Aquarium → `aquarium/add-new-aquarium.html`
- 🔌 Add Device → `device/add-device.html`
- 🐠 View Aquariums → `aquarium/aquarium-selection.html`
- ⚙️ Manage Devices → `device/manage-devices.html`

---

### Aquarium Section

#### Aquarium Selection
**File**: `/UI/aquarium/aquarium-selection.html`

**Sidebar Navigation**:
- 🏠 Dashboard → `../index.html`
- 🐠 Aquariums → `aquarium-selection.html` (active)
- 🔌 Devices → `../device/manage-devices.html`

**Action Buttons**:
- ➕ Add New → `add-new-aquarium.html`

**Card Actions** (per aquarium):
- View Devices → `aquarium-devices.html?tankId={id}`
- Manage → `manage-aquarium.html?tankId={id}`

---

#### Add New Aquarium
**File**: `/UI/aquarium/add-new-aquarium.html`

**Sidebar Navigation**:
- 🏠 Dashboard → `../index.html`
- 🐠 Aquariums → `aquarium-selection.html` (active)
- 🔌 Devices → `../device/manage-devices.html`

**Form Actions**:
- Save → Returns to `aquarium-selection.html`
- Cancel → Returns to `aquarium-selection.html`

---

#### Manage Aquarium
**File**: `/UI/aquarium/manage-aquarium.html`

**Sidebar Navigation**:
- 🏠 Dashboard → `../index.html`
- 🐠 Aquariums → `aquarium-selection.html` (active)
- 🔌 Devices → `../device/manage-devices.html`

**Action Buttons**:
- Back → `aquarium-selection.html`
- View Devices → `aquarium-devices.html?tankId={id}`

**Form Actions**:
- Update → Stays on page
- Delete → Returns to `aquarium-selection.html`

---

#### Aquarium Devices
**File**: `/UI/aquarium/aquarium-devices.html`

**Sidebar Navigation**:
- 🏠 Dashboard → `../index.html`
- 🐠 Aquariums → `aquarium-selection.html` (active)
- 🔌 Devices → `../device/manage-devices.html`

**Action Buttons**:
- Back → `aquarium-selection.html`
- Add Device → `../device/add-device.html?tankId={id}`

**Device Card Actions** (per device):
- View Details → `details/light-details.html?mac={mac}` (device-specific)

---

#### Light Details (Device Control)
**File**: `/UI/aquarium/details/light-details.html`

**Sidebar Navigation**:
- 🏠 Dashboard → `../../index.html`
- 🐠 Aquariums → `../aquarium-selection.html` (active)
- 🔌 Devices → `../../device/manage-devices.html`

**Action Buttons**:
- Back → `../aquarium-devices.html?tankId={tankId}`
 - Schedule → `../../device/schedule/light-schedule.html?mac={mac}`
- Settings → `../../device/device-setup.html?mac={mac}`

---

### Device Section

#### Manage Devices
**File**: `/UI/device/manage-devices.html`

**Sidebar Navigation**:
- 🏠 Dashboard → `../index.html`
- 🐠 Aquariums → `../aquarium/aquarium-selection.html`
- 🔌 Devices → `manage-devices.html` (active)

**Action Buttons**:
- ➕ Add Device → `add-device.html`

**Device Card Actions** (per device):
- 🔌 Control → Routes to appropriate control page based on device type:
  - Light → `../aquarium/details/light-details.html?mac={mac}`
  - CO₂ → `../aquarium/details/co2-details.html?mac={mac}`
  - Heater → `../aquarium/details/heater-details.html?mac={mac}`
  - Feeder → `../aquarium/details/feeder-details.html?mac={mac}`
  - Sensor → `../aquarium/details/sensor-details.html?mac={mac}`
- ⚙️ Setup → `device-setup.html?mac={mac}`

---

#### Add Device
**File**: `/UI/device/add-device.html`

**Sidebar Navigation**:
- 🏠 Dashboard → `../index.html`
- 🐠 Aquariums → `../aquarium/aquarium-selection.html`
- 🔌 Devices → `manage-devices.html` (active)

**Form Actions**:
- Register (discovered device) → Auto-fills form
- Add Device (manual) → Returns to `manage-devices.html`
- Cancel → Returns to `manage-devices.html`

---

#### Device Setup
**File**: `/UI/device/device-setup.html`

**Sidebar Navigation**:
- 🏠 Dashboard → `../index.html`
- 🐠 Aquariums → `../aquarium/aquarium-selection.html`
- Back → `manage-devices.html`
- Schedules → `schedule/light-schedule.html?mac={mac}`
 - Schedules → `schedule/light-schedule.html?mac={mac}`
- Delete Device → Returns to `manage-devices.html`

#### Device Schedule
**File**: `/UI/device/schedule/light-schedule.html`

**Sidebar Navigation**:
- 🏠 Dashboard → `../../index.html`
- 🐠 Aquariums → `../../aquarium/aquarium-selection.html`
- 🔌 Devices → `../manage-devices.html` (active)

**Action Buttons**:
- Back → `../device-setup.html?mac={mac}`
- ➕ Add Schedule → Opens modal

**Form Actions**:
- Create Schedule → Stays on page, updates list
- Edit Schedule → Opens modal with pre-filled data
- Delete Schedule → Removes from list

---

## 🔄 Navigation Flow Diagrams

### Primary User Flows

```
Dashboard
├─→ Aquariums
│   ├─→ Add New Aquarium
│   ├─→ Manage Aquarium
│   │   └─→ View Devices
│   │       └─→ Light Details
│   │           ├─→ Device Setup
│   │           └─→ Device Schedule
│   └─→ Aquarium Devices
│       └─→ (same as above)
│
└─→ Devices
    ├─→ Add Device
    ├─→ Device Setup
    │   └─→ Device Schedule
    └─→ Control (routes to specific control page)
```

### Breadcrumb Structure

```
Dashboard > Aquariums > [Aquarium Name] > Devices > [Device Name] > Details
Dashboard > Devices > [Device Name] > Setup > Schedules
```

---

## 📱 Responsive Behavior

All navigation remains consistent across:
- **Desktop** (>1024px): Sidebar always visible
- **Tablet** (768px-1023px): Sidebar always visible, compact icons
- **Mobile** (<768px): Sidebar collapsible/hamburger menu

---

## ✅ Navigation Checklist

- [x] Dashboard links to all main sections
- [x] Aquarium section fully interconnected
- [x] Device section fully interconnected
- [x] Cross-links between Aquarium and Device sections
- [x] All "Back" buttons route correctly
- [x] All "Add" buttons route to correct forms
- [x] Device control pages route based on device type
- [x] Schedule pages accessible from both aquarium and device views
- [x] Settings pages accessible from device cards
- [x] Quick actions on dashboard functional

---

**Last Updated**: January 1, 2026  
**Status**: All navigation links implemented and tested ✅
