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
            sidebar.innerHTML = '<nav class="sidebar-nav"><a class="nav-item" href="/index.html">Dashboard</a></nav>';
        });
})();
