// Static preview stub for app.js
console.warn('Static app.js loaded — interactive features require running hub.');
function updateConnectionStatus(connected){ const el=document.querySelector('.status-dot'); if(el){el.style.opacity=connected?1:0.4;} }
document.addEventListener('DOMContentLoaded', ()=>{
  updateConnectionStatus(true);
});
