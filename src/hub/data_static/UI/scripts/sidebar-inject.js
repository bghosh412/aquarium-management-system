// Robustly injects the sidebar.html into <aside class="sidebar">, supports subdirectories, and highlights active nav
(function() {
    var sidebar = document.querySelector('aside.sidebar');
    if (!sidebar) return;

    // Compute sidebar path relative to current location
    var path = window.location.pathname;
    var sidebarPath = '/sidebar.html';
    if (!path.startsWith('/')) path = '/' + path;
    // If we're in a subdirectory, always fetch from root

    fetch(sidebarPath)
        .then(function(resp) {
            if (!resp.ok) throw new Error('Sidebar fetch failed');
            return resp.text();
        })
        .then(function(html) {
            sidebar.innerHTML = html;
            // Highlight active nav item by comparing path only
            var navs = sidebar.querySelectorAll('.nav-item');
            var cur = window.location.pathname.replace(/\/index\.html$/, '/');
            navs.forEach(function(nav) {
                var navPath = nav.getAttribute('href');
                if (navPath === cur || (navPath !== '/' && cur.startsWith(navPath))) {
                    nav.classList.add('active');
                }
            });
        })
        .catch(function() {
            sidebar.innerHTML = ''
                + '<div class="sidebar-header"><h3>Navigation</h3></div>'
                + '<nav class="sidebar-nav">'
                + '<a class="nav-item" href="/index.html">Dashboard</a>'
                + '<a class="nav-item" href="/aquarium/aquarium-selection.html">Aquariums</a>'
                + '<a class="nav-item" href="/device/manage-devices.html">Devices</a>'
                + '<a class="nav-item" href="/manage-schedule.html">Schedule</a>'
                + '<a class="nav-item" href="/settings/download-upload.html">Settings</a>'
                + '</nav>'
                + '<div class="sidebar-footer">'
                + '<div class="system-info">'
                + '<div class="info-item"><span class="info-label">Uptime</span><span class="info-value" id="uptime">--:--:--</span></div>'
                + '<div class="info-item"><span class="info-label">Version</span><span class="info-value">v1.0.0</span></div>'
                + '</div></div>';
        });
})();
