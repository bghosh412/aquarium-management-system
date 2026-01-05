// Injects the sidebar.html into the <aside class="sidebar"> element on page load
(function() {
    var sidebar = document.querySelector('aside.sidebar');
    if (!sidebar) return;
    var xhr = new XMLHttpRequest();
    xhr.open('GET', '/sidebar.html', true);
    xhr.onreadystatechange = function() {
        if (xhr.readyState === 4 && xhr.status === 200) {
            sidebar.innerHTML = xhr.responseText;
            // Optionally, highlight the active nav item
            var navs = sidebar.querySelectorAll('.nav-item');
            navs.forEach(function(nav) {
                if (nav.href && nav.href === window.location.href) {
                    nav.classList.add('active');
                }
            });
        }
    };
    xhr.send();
})();
