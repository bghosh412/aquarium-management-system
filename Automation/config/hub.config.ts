/**
 * Hub Configuration for tests
 * Override with environment variables if needed
 */
export const config = {
  // Hub base URL - override with HUB_URL env var
  hubUrl: process.env.HUB_URL || 'http://192.168.1.53',
  
  // API endpoints
  api: {
    status: '/api/status',
    aquariums: '/api/aquariums',
    aquariumUpdate: '/api/aquarium/update',
    aquariumDelete: '/api/aquarium/delete',
    devices: '/api/devices',
    unmappedDevices: '/api/unmapped-devices',
    provisionDevice: '/api/provision-device',
    deleteDevice: '/api/delete-device',
    unmapDevice: '/api/unmap-device',
    lightSchedule: '/api/light-schedule',
    lightStatus: '/api/light-status',
    peers: '/api/peers',
    hubMacs: '/api/hub-macs',
    settingsFiles: '/api/settings/files',
    settingsDownload: '/api/settings/download',
    settingsUpload: '/api/settings/upload',
    settingsOtaUrls: '/api/settings/ota-urls',
    hubOtaCheck: '/api/hub/ota/check',
    otaFirmware: '/api/ota/firmware',
    otaLittlefs: '/api/ota/littlefs',
    otaAll: '/api/ota/all',
    // Generic device OTA endpoints (replace {type} with device type: light, co2, heater, etc.)
    nodesDeviceList: (type: string) => `/api/nodes/${type}/list`,
    nodesDeviceCheckUpdate: (type: string) => `/api/nodes/${type}/check-update`,
    nodesDeviceApplyUpdate: (type: string) => `/api/nodes/${type}/apply-update`,
    nodesDeviceOtaStatus: (type: string) => `/api/nodes/${type}/ota-status`,
    // Legacy endpoints (for backwards compatibility)
    nodesLightVersion: '/api/nodes/light/version',
    nodesLightList: '/api/nodes/light/list',
    reboot: '/api/reboot',
  },
  
  // UI pages
  pages: {
    home: '/',
    dashboard: '/index.html',
    aquariumSelection: '/aquarium/aquarium-selection.html',
    addAquarium: '/aquarium/add-new-aquarium.html',
    manageAquarium: '/aquarium/manage-aquarium.html',
    aquariumDevices: '/aquarium/aquarium-devices.html',
    manageDevices: '/device/manage-devices.html',
    addDevice: '/device/add-device.html',
    deviceSetup: '/device/device-setup.html',
    lightControl: '/device/control/light-control.html',
    lightSchedule: '/device/schedule/light-schedule.html',
    lightDetails: '/aquarium/details/light-details.html',
    feederSchedule: '/device/schedule/feeder-schedule.html',
    settingsWifi: '/settings/wifi-settings.html',
    settingsBackup: '/settings/backup-restore.html',
    settingsDownloadUpload: '/settings/download-upload.html',
    settingsUpdateSoftware: '/settings/update-software.html',
    settingsDiagnostics: '/settings/diagnostics.html',
  },
  
  // Test timeouts
  timeouts: {
    short: 5000,
    medium: 10000,
    long: 30000,
    apiResponse: 15000,
  },
  
  // Test data
  testData: {
    aquariumName: 'Test Aquarium',
    aquariumNameUpdated: 'Updated Aquarium',
    deviceName: 'Test Light',
  }
};
