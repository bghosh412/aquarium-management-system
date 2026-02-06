// Static-preview copy of manage-devices.js — full interactive behavior requires the live hub
console.warn('Running static preview of manage-devices.js');

document.addEventListener('DOMContentLoaded', () => {
  const grid = document.getElementById('devicesGrid');
  if (!grid) return;
  grid.innerHTML = `
    <div class="card"><div style="padding:1rem;"><h3>🐟 Feeder_01</h3><div style="margin-top:0.5rem;"><button class="btn">🗓️ Schedule</button> <button class="btn">⚙️ Setup</button></div></div></div>
    <div class="card"><div style="padding:1rem;"><h3>💡 Light_01</h3><div style="margin-top:0.5rem;"><button class="btn">🗓️ Schedule</button> <button class="btn">🎛️ Control</button></div></div></div>
  `;
});
