(function () {
  'use strict';

  var STORAGE_KEY = 'robocraft-docs-theme';

  /* ── Theme toggle ── */
  function initTheme() {
    var btn = document.getElementById('theme-toggle');
    if (!btn) return;

    btn.addEventListener('click', function () {
      var root = document.documentElement;
      var next = root.getAttribute('data-theme') === 'dark' ? 'light' : 'dark';
      root.setAttribute('data-theme', next);
      localStorage.setItem(STORAGE_KEY, next);
    });
  }

  /* ── Slug for heading anchors ── */
  function slugify(text) {
    return text
      .trim()
      .toLowerCase()
      .replace(/[^\w\u4e00-\u9fff\s-]/g, '')
      .replace(/\s+/g, '-')
      .replace(/-+/g, '-')
      .replace(/^-|-$/g, '') || 'section';
  }

  function uniqueId(base, used) {
    var id = base;
    var n = 2;
    while (used[id]) {
      id = base + '-' + n++;
    }
    used[id] = true;
    return id;
  }

  /* ── Build TOC from h2/h3 ── */
  function buildToc() {
    var content = document.getElementById('main-content');
    var nav = document.getElementById('toc-nav');
    if (!content || !nav) return;

    var headings = content.querySelectorAll('h2, h3');
    if (!headings.length) {
      nav.innerHTML = '<p class="sidebar__empty">本页无章节目录</p>';
      return;
    }

    var used = {};
    var items = [];

    headings.forEach(function (h) {
      var text = h.textContent.replace(/\s+/g, ' ').trim();
      if (!text) return;

      var id = h.id || uniqueId(slugify(text), used);
      h.id = id;

      items.push({
        level: h.tagName.toLowerCase(),
        id: id,
        text: text,
      });
    });

    var ul = document.createElement('ul');
    items.forEach(function (item) {
      var li = document.createElement('li');
      if (item.level === 'h3') li.className = 'toc-h3';
      var a = document.createElement('a');
      a.href = '#' + item.id;
      a.textContent = item.text;
      li.appendChild(a);
      ul.appendChild(li);
    });

    nav.innerHTML = '';
    nav.appendChild(ul);

    initTocScrollSpy(nav.querySelectorAll('a'));
  }

  /* ── Highlight active TOC entry ── */
  function initTocScrollSpy(links) {
    if (!links.length) return;

    var headings = Array.from(links).map(function (a) {
      return document.getElementById(a.getAttribute('href').slice(1));
    }).filter(Boolean);

    function update() {
      var scrollY = window.scrollY + 100;
      var current = headings[0];

      headings.forEach(function (h) {
        if (h.offsetTop <= scrollY) current = h;
      });

      links.forEach(function (a) {
        a.classList.toggle('active', current && a.getAttribute('href') === '#' + current.id);
      });
    }

    window.addEventListener('scroll', update, { passive: true });
    update();
  }

  /* ── Mobile sidebar drawers ── */
  function initMobileSidebars() {
    var tocSidebar = document.getElementById('sidebar-toc');
    var navSidebar = document.querySelector('.sidebar--right');
    var backdrop = document.getElementById('sidebar-backdrop');
    var fabToc = document.getElementById('fab-toc');
    var fabNav = document.getElementById('fab-nav');

    if (!backdrop) return;

    function closeAll() {
      if (tocSidebar) tocSidebar.classList.remove('is-open');
      if (navSidebar) navSidebar.classList.remove('is-open');
      backdrop.hidden = true;
    }

    function open(sidebar) {
      closeAll();
      sidebar.classList.add('is-open');
      backdrop.hidden = false;
    }

    if (fabToc && tocSidebar) {
      fabToc.addEventListener('click', function () {
        if (tocSidebar.classList.contains('is-open')) closeAll();
        else open(tocSidebar);
      });
    }

    if (fabNav && navSidebar) {
      fabNav.addEventListener('click', function () {
        if (navSidebar.classList.contains('is-open')) closeAll();
        else open(navSidebar);
      });
    }

    backdrop.addEventListener('click', closeAll);

    document.querySelectorAll('#toc-nav a, .sidebar-nav a').forEach(function (a) {
      a.addEventListener('click', function () {
        if (window.innerWidth <= 1100) closeAll();
      });
    });
  }

  document.addEventListener('DOMContentLoaded', function () {
    initTheme();
    buildToc();
    initMobileSidebars();
  });
})();
