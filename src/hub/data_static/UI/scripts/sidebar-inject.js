// Robustly injects the sidebar.html into <aside class="sidebar">, supports subdirectories, and highlights active nav
(function() {
    var sidebar = document.querySelector('aside.sidebar');
    if (!sidebar) return;

    var sidebarPath = '/sidebar.html';

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
            // Start sidebar footer updates (uptime + version)
            initSidebarFooter();
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
                + '<div class="info-item"><span class="info-label">Uptime</span><span class="info-value" id="sidebar-uptime">--:--:--</span></div>'
                + '<div class="info-item"><span class="info-label">Version</span><span class="info-value" id="sidebar-version">v—</span></div>'
                + '</div></div>';
            initSidebarFooter();
        });

    // ── Sidebar footer: real hub uptime + firmware version ──
    var _sidebarUptimeSec = null;   // last known hub uptime in seconds
    var _sidebarTickTimer = null;

    function initSidebarFooter() {
        fetchSidebarData();                        // first fetch immediately
        setInterval(fetchSidebarData, 30000);      // refresh from hub every 30s
        _sidebarTickTimer = setInterval(tickUptime, 1000); // local +1s tick
    }

    function fetchSidebarData() {
        // Fetch uptime from diagnostics
        fetch('/api/diagnostics')
            .then(function(r) { return r.json(); })
            .then(function(d) {
                if (typeof d.uptime === 'number') {
                    _sidebarUptimeSec = d.uptime;
                    renderUptime();
                }
            })
            .catch(function() {});

        // Fetch firmware version (one-time is fine, but 30s refresh won't hurt)
        fetch('/api/settings/ota-urls')
            .then(function(r) { return r.json(); })
            .then(function(d) {
                var el = document.getElementById('sidebar-version');
                if (el && d.hubFirmwareVersion) {
                    el.textContent = 'v' + d.hubFirmwareVersion;
                }
            })
            .catch(function() {});
    }

    function tickUptime() {
        if (_sidebarUptimeSec !== null) {
            _sidebarUptimeSec++;
            renderUptime();
        }
    }

    function renderUptime() {
        var el = document.getElementById('sidebar-uptime');
        if (!el || _sidebarUptimeSec === null) return;
        var s = _sidebarUptimeSec;
        var d = Math.floor(s / 86400); s %= 86400;
        var h = Math.floor(s / 3600);  s %= 3600;
        var m = Math.floor(s / 60);    s %= 60;
        var parts = [];
        if (d > 0) parts.push(d + 'd');
        parts.push(pad2(h) + ':' + pad2(m) + ':' + pad2(s));
        el.textContent = parts.join(' ');
    }

    function pad2(n) { return n < 10 ? '0' + n : '' + n; }
})();
