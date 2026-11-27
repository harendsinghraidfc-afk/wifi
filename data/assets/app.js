'use strict';

/* ============================================================
   ESP-Router — admin panel (vanilla JS, no build step)
   ============================================================ */

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

const esc = (value) => String(value ?? '').replace(/[&<>"']/g, (c) => ({
  '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
}[c]));

const icon = (name, cls = '') => `<svg class="ic ${cls}" aria-hidden="true"><use href="#i-${name}"></use></svg>`;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function fmtUptime(seconds) {
  const s = Math.max(0, Math.floor(Number(seconds) || 0));
  if (s < 60) return `${s}s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m ${s % 60}s`;
  const h = Math.floor(m / 60);
  if (h < 24) return `${h}h ${m % 60}m`;
  return `${Math.floor(h / 24)}d ${h % 24}h`;
}

function fmtBytes(bytes) {
  const n = Number(bytes) || 0;
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / (1024 * 1024)).toFixed(2)} MB`;
}

function fmtAgo(seconds) {
  const s = Math.max(0, Math.floor(Number(seconds) || 0));
  if (s < 10) return 'just now';
  if (s < 60) return `${s}s ago`;
  if (s < 3600) return `${Math.floor(s / 60)}m ago`;
  if (s < 86400) return `${Math.floor(s / 3600)}h ago`;
  return `${Math.floor(s / 86400)}d ago`;
}

function fmtClock(ms) {
  const total = Math.floor((Number(ms) || 0) / 1000);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  return [h, m, s].map((v) => String(v).padStart(2, '0')).join(':');
}

function signalHtml(rssi) {
  if (!rssi) return '<span class="dim">—</span>';
  const level = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  const tone = rssi >= -65 ? '' : rssi >= -75 ? 'mid' : 'weak';
  const bars = [1, 2, 3, 4].map((i) => `<i class="${i <= level ? 'on' : ''}"></i>`).join('');
  return `<span class="signal ${tone}">${bars}</span><span class="signal-val">${rssi} dBm</span>`;
}

const RESET_REASONS = [
  'Unknown', 'Power-on', 'External pin', 'Software restart', 'Panic',
  'Interrupt watchdog', 'Task watchdog', 'Other watchdog', 'Deep sleep',
  'Brownout', 'SDIO', 'USB', 'JTAG', 'eFuse', 'Power glitch', 'CPU lockup',
];

/* ============================================================
   API layer
   ============================================================ */

class ApiError extends Error {
  constructor(message, status = 0, data = null) {
    super(message);
    this.status = status;
    this.data = data;
  }
}

const state = {
  setupRequired: false,
  auth: false,
  csrf: '',
  version: '',
  hostname: 'router',
  info: null,
  wifi: null,
  nat: null,
  stats: null,
  clients: null,
  logs: [],
  logSeqs: new Set(),
  logPaused: false,
  logNew: 0,
  ws: null,
  wsTries: 0,
  wsTimer: null,
  pollTimer: null,
  route: 'dashboard',
  wirelessTab: 'ap',
  autoScan: false,
  renderToken: 0,
};

async function api(path, { method = 'GET', body, silent401 = false, timeout = 25000 } = {}) {
  const headers = {};
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  if (state.csrf) headers['X-CSRF-Token'] = state.csrf;

  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeout);
  let res;
  try {
    res = await fetch(path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: controller.signal,
      cache: 'no-store',
    });
  } catch (err) {
    clearTimeout(timer);
    throw new ApiError('Network error — the router is unreachable', 0);
  }
  clearTimeout(timer);

  const text = await res.text();
  let data = null;
  if (text) {
    try { data = JSON.parse(text); } catch { data = text; }
  }

  if (!res.ok) {
    if (res.status === 401 && !silent401) {
      handleLoggedOut('Session expired. Please sign in again.');
    }
    const message = (data && data.error) || `Request failed (${res.status})`;
    throw new ApiError(message, res.status, data);
  }
  return data;
}

/* ============================================================
   Small UI helpers
   ============================================================ */

function toast(message, type = 'info', ms = 3800) {
  const wrap = $('#toasts');
  if (!wrap) return;
  const el = document.createElement('div');
  el.className = `toast ${type}`;
  el.innerHTML = `${icon(type === 'ok' ? 'check' : type === 'err' ? 'x' : 'router')}<div>${esc(message)}</div>`;
  wrap.appendChild(el);
  setTimeout(() => {
    el.classList.add('leaving');
    setTimeout(() => el.remove(), 240);
  }, ms);
}

function openModal({ title, body = '', okText = 'Save', cancelText = 'Cancel', danger = false, onOk = null, onOpen = null }) {
  const dlg = document.createElement('dialog');
  dlg.className = 'modal';
  dlg.innerHTML = `
    <div class="modal-card">
      <div class="modal-head">
        <h3>${esc(title)}</h3>
        <button type="button" class="icon-btn" data-close aria-label="Close">${icon('x')}</button>
      </div>
      <div class="modal-body">${body}</div>
      <div class="modal-foot">
        <button type="button" class="btn ghost" data-close>${esc(cancelText)}</button>
        <button type="button" class="btn ${danger ? 'danger' : 'primary'}" data-ok>${esc(okText)}</button>
      </div>
    </div>`;
  document.body.appendChild(dlg);

  const close = () => { try { dlg.close(); } catch {} dlg.remove(); };
  dlg.addEventListener('click', (e) => {
    if (e.target === dlg || e.target.closest('[data-close]')) close();
  });

  const okBtn = dlg.querySelector('[data-ok]');
  okBtn.addEventListener('click', async () => {
    if (!onOk) { close(); return; }
    const old = okBtn.innerHTML;
    okBtn.disabled = true;
    okBtn.textContent = 'Working…';
    try {
      const keep = await onOk(dlg);
      if (keep !== false) close();
    } catch (err) {
      toast(err.message || 'Something went wrong', 'err');
    } finally {
      okBtn.disabled = false;
      okBtn.innerHTML = old;
    }
  });

  dlg.showModal();
  if (onOpen) onOpen(dlg);
  return dlg;
}

function confirmAction(title, message, okText, danger, onOk) {
  return openModal({
    title,
    body: `<p>${message}</p>`,
    okText,
    danger,
    onOk,
  });
}

function setText(id, value) {
  const el = document.getElementById(id);
  if (el) el.textContent = value;
}

function setCaptiveBadge(on) {
  const el = document.getElementById('nav-captive-state');
  if (!el) return;
  el.textContent = on ? 'On' : 'Off';
  el.classList.toggle('warn', !!on);
}

function checked(sel) {
  const el = $(sel);
  return !!(el && el.checked);
}

function val(sel) {
  const el = $(sel);
  return el ? el.value.trim() : '';
}

function applyStats(stats) {
  state.stats = stats;

  const wanPill = $('#pill-wan');
  if (wanPill) {
    wanPill.className = `pill ${stats.staConnected ? 'ok' : 'err'}`;
    wanPill.innerHTML = `<span class="dot ${stats.staConnected ? 'ok' : 'err'}"></span>WAN ${stats.staConnected ? 'online' : 'offline'}`;
  }
  const clientsPill = $('#pill-clients');
  if (clientsPill) clientsPill.innerHTML = `${icon('users')} ${stats.onlineClients}`;
  const uptimePill = $('#pill-uptime');
  if (uptimePill) uptimePill.textContent = fmtUptime(stats.uptime);
  const navCount = $('#nav-clients');
  if (navCount) navCount.textContent = stats.onlineClients;

  const values = {
    uptime: fmtUptime(stats.uptime),
    heap: fmtBytes(stats.heap),
    clients: String(stats.onlineClients),
    apclients: String(stats.apClients),
    wanip: stats.staIp || '—',
    rssi: stats.staConnected ? `${stats.staRssi} dBm` : '—',
    channel: String(stats.channel),
    wanstate: stats.staConnected ? 'Online' : 'Offline',
    natstate: stats.natActive ? 'Active' : 'Inactive',
  };

  $$('[data-live]').forEach((el) => {
    const key = el.dataset.live;
    if (!(key in values)) return;
    el.textContent = values[key];
    if (key === 'wanstate' && el.classList.contains('badge')) {
      el.className = `badge ${stats.staConnected ? 'ok' : 'err'}`;
    }
    if (key === 'natstate' && el.classList.contains('badge')) {
      el.className = `badge ${stats.natActive ? 'ok' : 'warn'}`;
    }
  });
}

function applyWifiLive(wifi) {
  const sta = wifi.sta || {};
  const ap = wifi.ap || {};
  const values = {
    wanstate: sta.connected ? 'Online' : 'Offline',
    wanip: sta.connected ? (sta.ip || '—') : '—',
    rssi: sta.connected ? `${sta.rssi} dBm` : '—',
    apclients: String(ap.clients ?? 0),
    clients: String(ap.clients ?? 0),
    channel: String(ap.channel ?? '—'),
  };
  $$('[data-live]').forEach((el) => {
    const key = el.dataset.live;
    if (!(key in values)) return;
    el.textContent = values[key];
    if (key === 'wanstate' && el.classList.contains('badge')) {
      el.className = `badge ${sta.connected ? 'ok' : 'err'}`;
    }
  });
}

/* ============================================================
   WebSocket
   ============================================================ */

function setLive(tone, text) {
  const dot = $('#ws-dot');
  if (dot) dot.className = `dot ${tone}`;
  setText('ws-label', text);
}

function connectWs() {
  if (!state.auth) return;
  stopWs(false);
  try {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const ws = new WebSocket(`${proto}//${location.host}/ws`);
    state.ws = ws;

    ws.onopen = () => {
      state.wsTries = 0;
      setLive('ok', 'Live updates');
    };

    ws.onmessage = (event) => {
      let msg;
      try { msg = JSON.parse(event.data); } catch { return; }
      if (msg.type === 'stats') applyStats(msg);
      else if (msg.type === 'logs') appendLogs(msg.lines || []);
    };

    ws.onclose = () => {
      state.ws = null;
      setLive('err', 'Reconnecting…');
      const delay = Math.min(1000 * Math.pow(1.6, state.wsTries++), 12000);
      state.wsTimer = setTimeout(connectWs, delay);
      if (state.wsTries > 0 && state.wsTries % 5 === 0) checkSession();
    };

    ws.onerror = () => { try { ws.close(); } catch {} };
  } catch {
    setLive('err', 'Offline');
    state.wsTimer = setTimeout(connectWs, 5000);
  }
}

function stopWs(clearLabel = true) {
  clearTimeout(state.wsTimer);
  state.wsTimer = null;
  if (state.ws) {
    state.ws.onclose = null;
    try { state.ws.close(); } catch {}
    state.ws = null;
  }
  if (clearLabel) setLive('', 'Offline');
}

async function checkSession() {
  try {
    const s = await api('/api/auth/status', { silent401: true });
    if (!s.authenticated && !s.setupRequired) {
      handleLoggedOut('Session expired. Please sign in again.');
    }
  } catch {}
}

function startFallbackPoll() {
  if (state.pollTimer) return;
  state.pollTimer = setInterval(async () => {
    if (!state.auth) return;
    if (state.ws && state.ws.readyState === WebSocket.OPEN) return;
    try {
      const wifi = await api('/api/wifi/status');
      state.wifi = wifi;
      applyWifiLive(wifi);
    } catch {}
  }, 10000);
}

/* ============================================================
   Boot / auth screens
   ============================================================ */

async function boot() {
  hideOverlay();
  document.title = 'ESP-Router';
  $('#app').innerHTML = `<div class="boot"><div class="spinner"></div><p>Loading router…</p></div>`;
  try {
    const s = await api('/api/auth/status', { silent401: true });
    state.setupRequired = !!s.setupRequired;
    state.version = s.version || '';
    state.hostname = s.hostname || 'router';

    if (s.setupRequired) {
      renderSetup();
      return;
    }
    if (s.authenticated) {
      state.auth = true;
      state.csrf = s.csrf || '';
      await enterApp();
      return;
    }
    renderLogin();
  } catch (err) {
    renderFatal(err.message || 'Could not reach the router');
  }
}

function renderFatal(message) {
  $('#app').innerHTML = `
    <div class="auth-wrap">
      <div class="auth-bg"></div>
      <div class="card auth-card">
        <div class="auth-brand">
          <div class="brand-mark">${icon('router', 'lg')}</div>
          <div><strong>ESP-Router</strong><span>connection problem</span></div>
        </div>
        <h2>Router unreachable</h2>
        <p>${esc(message)}</p>
        <button class="btn primary block" id="fatal-retry">${icon('refresh')} Try again</button>
      </div>
    </div>`;
  $('#fatal-retry').addEventListener('click', boot);
}

function handleLoggedOut(message = '') {
  if (!state.auth) return;
  state.auth = false;
  state.csrf = '';
  stopWs();
  hideOverlay();
  renderLogin(message);
}

function renderLogin(message = '') {
  stopWs();
  state.auth = false;
  state.csrf = '';
  document.title = 'Sign in — ESP-Router';
  $('#app').innerHTML = `
    <div class="auth-wrap">
      <div class="auth-bg"></div>
      <div class="card auth-card">
        <div class="auth-brand">
          <div class="brand-mark">${icon('router', 'lg')}</div>
          <div><strong>ESP-Router</strong><span id="login-host">${esc(state.hostname || 'router')}</span></div>
        </div>
        <h2>Sign in</h2>
        <p>Enter the admin password to manage this router.</p>
        <form id="login-form" novalidate>
          <div class="field"><label for="login-user">Username</label>
            <input id="login-user" type="text" autocomplete="off" spellcheck="false" autocapitalize="none" required autofocus></div>
          <div class="field">
            <label for="login-pass">Admin password</label>
            <div class="input-wrap">
              <input id="login-pass" type="password" autocomplete="current-password" required>
              <button type="button" class="reveal" data-reveal="login-pass" aria-label="Show password">${icon('eye')}</button>
            </div>
          </div>
          <div class="form-error" id="login-error" ${message ? '' : 'hidden'}>${esc(message)}</div>
          <button class="btn primary block" id="login-btn" type="submit">Sign in</button>
        </form>
        <div class="auth-foot">ESP-Router <span id="login-ver">${esc(state.version || '')}</span></div>
      </div>
    </div>`;

  const form = $('#login-form');
  const error = $('#login-error');
  const button = $('#login-btn');

  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    error.hidden = true;
    const password = $('#login-pass').value;
    if (!password) {
      error.textContent = 'Password is required';
      error.hidden = false;
      return;
    }

    button.disabled = true;
    button.textContent = 'Signing in…';
    let locked = false;
    try {
      const data = await api('/api/auth/login', {
        method: 'POST',
        body: { username: $('#login-user').value, password },
        silent401: true,
      });
      state.auth = true;
      state.csrf = data.csrf || '';
      await enterApp();
    } catch (err) {
      if (err.status === 429) {
        const retry = (err.data && err.data.retryAfter) || 60;
        error.textContent = `Too many failed attempts. Try again in ${retry}s.`;
        error.hidden = false;
        locked = true;
        lockLoginButton(button, retry);
      } else {
        error.textContent = err.status === 401 ? 'Incorrect username or password' : err.message;
        error.hidden = false;
        $('#login-pass').select();
      }
    } finally {
      if (!locked) {
        button.disabled = false;
        button.textContent = 'Sign in';
      }
    }
  });
}

function lockLoginButton(button, seconds) {
  let left = seconds;
  button.disabled = true;
  const tick = () => {
    button.textContent = `Try again in ${left}s`;
    if (left <= 0) {
      button.disabled = false;
      button.textContent = 'Sign in';
      return;
    }
    left -= 1;
    setTimeout(tick, 1000);
  };
  tick();
}

async function enterApp() {
  if (!$('.shell')) {
    $('#app').innerHTML = shellHtml();
    bindShell();
  }
  setText('brand-host', state.hostname || 'router');
  setText('side-ver', state.version || '—');
  document.title = 'ESP-Router';
  connectWs();
  startFallbackPoll();
  api('/api/portal/settings').then((d) => setCaptiveBadge(d.enabled)).catch(() => {});
  await navigate();
}

function shellHtml() {
  return `
    <div class="shell">
      <div class="backdrop" id="backdrop"></div>
      <aside class="sidebar" id="sidebar">
        <div class="brand">
          <div class="brand-mark">${icon('router', 'lg')}</div>
          <div><strong>ESP-Router</strong><span id="brand-host">${esc(state.hostname || 'router')}</span></div>
        </div>
        <nav class="nav">
          <a href="#/dashboard" data-route="dashboard">${icon('grid')}<span>Dashboard</span></a>
          <a href="#/wireless" data-route="wireless">${icon('wifi')}<span>Wireless</span></a>
          <a href="#/clients" data-route="clients">${icon('users')}<span>Clients</span><span class="count" id="nav-clients">0</span></a>
          <a href="#/network" data-route="network">${icon('globe')}<span>DHCP &amp; NAT</span></a>
          <a href="#/portal" data-route="portal">${icon('lock')}<span>Captive portal</span><span class="count" id="nav-captive-state">Off</span></a>
          <a href="#/system" data-route="system">${icon('gear')}<span>System</span></a>
          <a href="#/logs" data-route="logs">${icon('list')}<span>Logs</span></a>
          <a href="#" id="nav-logout">${icon('logout')}<span>Sign out</span></a>
        </nav>
        <div class="side-foot">
          <div class="live"><span class="dot" id="ws-dot"></span><span id="ws-label">Connecting…</span></div>
          <div class="ver">firmware v<span id="side-ver">—</span></div>
        </div>
      </aside>
      <div class="main">
        <header class="topbar">
          <button class="icon-btn menu-btn" id="menu-btn" aria-label="Open menu">${icon('menu')}</button>
          <div class="title-wrap">
            <h1 id="page-title">Dashboard</h1>
            <div class="sub" id="page-sub"></div>
          </div>
          <div class="pills">
            <span class="pill" id="pill-wan"><span class="dot"></span>WAN</span>
            <span class="pill" id="pill-clients">${icon('users')} 0</span>
            <span class="pill optional" id="pill-uptime">—</span>
          </div>
        </header>
        <main class="page" id="page"></main>
      </div>
    </div>`;
}

function bindShell() {
  const sidebar = $('#sidebar');
  const backdrop = $('#backdrop');
  const close = () => {
    sidebar.classList.remove('open');
    backdrop.classList.remove('show');
  };
  $('#menu-btn').addEventListener('click', () => {
    sidebar.classList.toggle('open');
    backdrop.classList.toggle('show', sidebar.classList.contains('open'));
  });
  backdrop.addEventListener('click', close);

  $('#nav-logout').addEventListener('click', async (event) => {
    event.preventDefault();
    try {
      await api('/api/auth/logout', { method: 'POST' });
    } catch {}
    state.auth = false;
    state.csrf = '';
    stopWs();
    renderLogin();
  });
}

/* ============================================================
   Navigation
   ============================================================ */

const PAGE_META = {
  dashboard: ['Dashboard', 'Live overview of your network'],
  wireless: ['Wireless', 'Access point and upstream settings'],
  clients: ['Clients', 'Devices connected to your router'],
  network: ['DHCP & NAT', 'Address pool and internet sharing'],
  portal: ['Captive portal', 'Guest page and internet access'],
  system: ['System', 'Firmware, backups and maintenance'],
  logs: ['Logs', 'Live device log stream'],
};

function currentRoute() {
  if (location.hash === '#/network?portal=1') return 'portal';
  const hash = location.hash.replace(/^#\/?/, '').split('?')[0];
  return PAGE_META[hash] ? hash : 'dashboard';
}

function goto(route) {
  if (currentRoute() === route) navigate();
  else location.hash = `#/${route}`;
}

function skeletonPage() {
  return `
    <div class="grid stats">
      ${[1, 2, 3, 4].map(() => `<div class="card"><div class="skeleton" style="width:45%"></div><div class="skeleton" style="width:70%;height:24px;margin-top:12px"></div><div class="skeleton" style="width:55%;margin-top:10px"></div></div>`).join('')}
    </div>
    <div class="grid two" style="margin-top:16px">
      <div class="card"><div class="skeleton" style="width:30%"></div><div class="skeleton" style="width:90%;margin-top:14px"></div><div class="skeleton" style="width:80%;margin-top:9px"></div><div class="skeleton" style="width:85%;margin-top:9px"></div></div>
      <div class="card"><div class="skeleton" style="width:35%"></div><div class="skeleton" style="width:88%;margin-top:14px"></div><div class="skeleton" style="width:75%;margin-top:9px"></div><div class="skeleton" style="width:82%;margin-top:9px"></div></div>
    </div>`;
}

function errorCard(message) {
  return `<div class="card"><div class="empty">${icon('x')}<p>${esc(message)}</p>
    <button class="btn ghost" id="retry-btn" style="margin-top:14px">${icon('refresh')} Retry</button></div></div>`;
}

async function navigate() {
  if (!state.auth) return;
  const route = currentRoute();
  state.route = route;
  const token = ++state.renderToken;
  const meta = PAGE_META[route];

  setText('page-title', meta[0]);
  setText('page-sub', meta[1]);
  $$('.nav a[data-route]').forEach((a) => a.classList.toggle('active', a.dataset.route === route));
  $('#sidebar')?.classList.remove('open');
  $('#backdrop')?.classList.remove('show');
  $('#page').innerHTML = skeletonPage();

  try {
    await ROUTES[route](token);
  } catch (err) {
    if (token !== state.renderToken) return;
    if (err instanceof ApiError && err.status === 401) return;
    $('#page').innerHTML = errorCard(err.message || 'Failed to load page');
    $('#retry-btn')?.addEventListener('click', () => navigate());
  }
}

window.addEventListener('hashchange', () => {
  if (state.auth) navigate();
});

/* ============================================================
   Dashboard
   ============================================================ */

async function pageDashboard(token) {
  const [info, wifi, nat] = await Promise.all([
    api('/api/system/info'),
    api('/api/wifi/status'),
    api('/api/router/nat'),
  ]);
  if (token !== state.renderToken) return;

  state.info = info;
  state.wifi = wifi;
  state.nat = nat;
  state.version = info.version || state.version;
  state.hostname = info.hostname || state.hostname;
  setText('side-ver', state.version);
  setText('brand-host', state.hostname);

  const wan = wifi.sta || {};
  const ap = wifi.ap || {};
  const wanTone = wan.connected ? 'ok' : (wan.configured ? 'err' : '');
  const wanText = wan.connected ? 'Online' : (wan.configured ? 'Offline' : 'Not set');
  const natTone = nat.supported === false ? 'err' : nat.active ? 'ok' : (nat.enabled ? 'warn' : '');
  const natText = nat.supported === false ? 'Unsupported' : nat.active ? 'Active' : (nat.enabled ? 'Waiting' : 'Off');

  $('#page').innerHTML = `
    <div class="grid stats">
      <div class="card"><div class="stat">
        <div class="stat-icon ${wan.connected ? 'ok' : wan.configured ? 'err' : ''}">${icon('globe')}</div>
        <div><div class="stat-label">Internet</div>
          <div class="stat-value" data-live="wanstate">${wanText}</div>
          <div class="stat-sub">${wan.ssid ? esc(wan.ssid) : 'No upstream configured'}</div></div>
      </div></div>

      <div class="card"><div class="stat">
        <div class="stat-icon">${icon('users')}</div>
        <div><div class="stat-label">Clients</div>
          <div class="stat-value" data-live="clients">${ap.clients ?? 0}</div>
          <div class="stat-sub">connected to the access point</div></div>
      </div></div>

      <div class="card"><div class="stat">
        <div class="stat-icon">${icon('power')}</div>
        <div><div class="stat-label">Uptime</div>
          <div class="stat-value" data-live="uptime">${fmtUptime(info.uptime)}</div>
          <div class="stat-sub">heap <span data-live="heap">${fmtBytes(info.freeHeap)}</span> free</div></div>
      </div></div>

      <div class="card"><div class="stat">
        <div class="stat-icon ${nat.active ? 'ok' : nat.supported === false ? 'err' : ''}">${icon('globe')}</div>
        <div><div class="stat-label">NAT / Repeater</div>
          <div class="stat-value"><span class="badge ${natTone}" data-live="natstate">${natText}</span></div>
          <div class="stat-sub">${nat.supported === false ? 'NAPT not built into this firmware' : 'shares the upstream connection'}</div></div>
      </div></div>
    </div>

    <div class="grid two">
      <section class="card">
        <div class="card-head">
          <div><h2>Internet (WAN)</h2><div class="sub">Upstream WiFi connection</div></div>
          <div class="card-actions"><span class="badge ${wanTone}" id="dash-wan-badge" data-live="wanstate">${wanText}</span></div>
        </div>
        <dl class="kv">
          <dt>Network</dt><dd>${wan.ssid ? esc(wan.ssid) : '<span class="dim">Not configured</span>'}</dd>
          <dt>IP address</dt><dd class="mono" data-live="wanip">${wan.connected ? esc(wan.ip || '—') : '—'}</dd>
          <dt>Gateway</dt><dd class="mono">${wan.connected ? esc(wan.gateway || '—') : '—'}</dd>
          <dt>DNS</dt><dd class="mono">${wan.connected ? esc(wan.dns || '—') : '—'}</dd>
          <dt>Signal</dt><dd data-live="rssi">${wan.connected ? `${wan.rssi} dBm` : '—'}</dd>
          <dt>Channel</dt><dd>${wan.connected ? (wan.channel ?? '—') : '—'}</dd>
        </dl>
        <div class="form-actions"><a class="btn sm ghost" href="#/wireless">Manage upstream ${icon('chevron', 'sm')}</a></div>
      </section>

      <section class="card">
        <div class="card-head">
          <div><h2>Access point</h2><div class="sub">Local WiFi for your devices</div></div>
          <div class="card-actions"><span class="badge ${ap.running ? 'ok' : 'err'}">${ap.running ? 'Broadcasting' : 'Down'}</span></div>
        </div>
        <dl class="kv">
          <dt>Network</dt><dd>${esc(ap.ssid || '—')}${ap.hidden ? ' <span class="badge">hidden</span>' : ''}</dd>
          <dt>Panel address</dt><dd class="mono">http://${esc(ap.ip || '192.168.4.1')}</dd>
          <dt>Clients</dt><dd data-live="apclients">${ap.clients ?? 0} / ${ap.maxClients ?? '—'}</dd>
          <dt>Channel</dt><dd data-live="channel">${ap.channel ?? '—'}</dd>
          <dt>Band</dt><dd>${ap.band === '5' ? '5 GHz' : ap.band === 'auto' ? 'Auto' : '2.4 GHz'}</dd>
          <dt>Security</dt><dd>${ap.passwordSet ? 'WPA2-PSK' : '<span class="badge warn">Open</span>'}</dd>
        </dl>
        <div class="form-actions"><a class="btn sm ghost" href="#/wireless">Edit access point ${icon('chevron', 'sm')}</a></div>
      </section>
    </div>

    <div class="grid two">
      <section class="card">
        <div class="card-head"><div><h2>Internet sharing</h2><div class="sub">NAT between the AP and upstream</div></div></div>
        <dl class="kv">
          <dt>Configuration</dt><dd>${nat.enabled ? 'Enabled' : 'Disabled'}</dd>
          <dt>NAPT state</dt><dd>${nat.active ? '<span class="badge ok">Active</span>' : nat.supported === false ? '<span class="badge err">Unsupported</span>' : '<span class="badge warn">Inactive</span>'}</dd>
          <dt>Upstream</dt><dd>${nat.upstreamConnected ? '<span class="badge ok">Connected</span>' : '<span class="badge err">Disconnected</span>'}</dd>
        </dl>
        ${nat.supported === false ? `<div class="note danger" style="margin-top:12px">This firmware build was compiled without NAPT. Clients can join the AP but won't reach the internet. See the README for the build option.</div>` : ''}
        ${nat.error ? `<div class="note warn" style="margin-top:12px">${esc(nat.error)}</div>` : ''}
        <div class="form-actions"><a class="btn sm ghost" href="#/network">Manage NAT ${icon('chevron', 'sm')}</a></div>
      </section>

      <section class="card">
        <div class="card-head"><div><h2>Quick actions</h2><div class="sub">Common tasks</div></div></div>
        <div class="form-actions" style="margin-top:0">
          <button class="btn ghost" id="qa-scan">${icon('wifi')} Scan networks</button>
          <a class="btn ghost" href="#/clients">${icon('users')} View clients</a>
          <a class="btn ghost" href="#/logs">${icon('list')} Live logs</a>
          <button class="btn ghost" id="qa-reboot">${icon('power')} Reboot</button>
        </div>
      </section>
    </div>`;

  $('#qa-scan')?.addEventListener('click', () => {
    state.wirelessTab = 'wan';
    state.autoScan = true;
    goto('wireless');
  });

  $('#qa-reboot')?.addEventListener('click', () => {
    confirmAction('Reboot router', 'The router will restart and all WiFi clients will disconnect briefly. Continue?', 'Reboot', false, async () => {
      await api('/api/system/reboot', { method: 'POST' });
      waitForDevice();
    });
  });
}

/* ============================================================
   Wireless
   ============================================================ */

function channelOptions(band, selected) {
  const list = band === '5'
    ? [36, 40, 44, 48, 149, 153, 157, 161, 165]
    : [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13];
  const current = list.includes(Number(selected)) ? Number(selected) : list[0];
  return list.map((c) => `<option value="${c}" ${c === current ? 'selected' : ''}>${c}</option>`).join('');
}

function bandLabel(band) {
  if (band === '5') return '5 GHz';
  if (band === 'auto') return 'Auto (2.4 + 5)';
  return '2.4 GHz';
}

function apFormHtml(wifi, info) {
  const ap = wifi.ap || {};
  const band = ap.band || '2.4';
  const bandSupported = !!info.bandSupported;
  return `
    <section class="card">
      <div class="card-head">
        <div><h2>Access point</h2><div class="sub">The WiFi network your devices connect to</div></div>
        <div class="card-actions"><span class="badge ${ap.running ? 'ok' : 'err'}">${ap.running ? 'Running' : 'Stopped'}</span></div>
      </div>

      <form id="ap-form" class="form-grid">
        <div class="field span-2">
          <label for="ap-ssid">Network name (SSID)</label>
          <input id="ap-ssid" maxlength="32" required value="${esc(ap.ssid || '')}">
        </div>

        <div class="field">
          <label for="ap-pass">WiFi password</label>
          <div class="input-wrap">
            <input id="ap-pass" type="password" autocomplete="new-password" placeholder="${ap.passwordSet ? 'Leave blank to keep current' : 'Open network — no password'}">
            <button type="button" class="reveal" data-reveal="ap-pass" aria-label="Show password">${icon('eye')}</button>
          </div>
          <div class="hint">8–63 characters. Blank keeps the current password.</div>
        </div>

        <div class="field">
          <label for="ap-band">Band</label>
          <select id="ap-band" ${bandSupported ? '' : 'disabled'}>
            <option value="2.4" ${band === '2.4' ? 'selected' : ''}>2.4 GHz</option>
            <option value="5" ${band === '5' ? 'selected' : ''}>5 GHz</option>
            <option value="auto" ${band === 'auto' ? 'selected' : ''}>Auto (2.4 + 5)</option>
          </select>
          <div class="hint">${bandSupported ? '5 GHz requires the ESP32-C5 radio.' : 'This board is 2.4 GHz only.'}</div>
        </div>

        <div class="field">
          <label for="ap-channel">Channel</label>
          <select id="ap-channel">${channelOptions(band, ap.channel)}</select>
          <div class="hint">Pick a quiet channel; clients reconnect after a change.</div>
        </div>

        <div class="field">
          <label for="ap-max">Maximum clients</label>
          <input id="ap-max" type="number" min="1" max="10" value="${ap.maxClients ?? 8}">
          <div class="hint">Between 1 and 10 devices.</div>
        </div>

        <div class="field">
          <label for="ap-tx">Transmit power (dBm)</label>
          <input id="ap-tx" type="number" min="5" max="20" value="${ap.txPower ?? 20}">
          <div class="hint">Lower it to reduce interference.</div>
        </div>

        <div class="field">
          <label for="ap-ip">Router IP address</label>
          <input id="ap-ip" class="mono" value="${esc(ap.ip || '192.168.4.1')}">
          <div class="hint">The panel is served from this address.</div>
        </div>

        <div class="field">
          <label for="ap-subnet">Subnet mask</label>
          <input id="ap-subnet" class="mono" value="${esc(ap.subnet || '255.255.255.0')}">
          <div class="hint">Keep the DHCP pool inside this subnet.</div>
        </div>

        <div class="field span-2">
          <label class="toggle"><input type="checkbox" id="ap-hidden" ${ap.hidden ? 'checked' : ''}><span class="track"></span> Hide network name (SSID)</label>
        </div>

        <div class="form-actions span-2" style="margin-top:2px">
          <button class="btn primary" type="submit">${icon('check')} Save access point</button>
          <button class="btn ghost" type="reset">Reset</button>
        </div>
      </form>

      <div class="note warn" style="margin-top:14px">Changing the SSID, password, channel or IP restarts the access point. Connected devices will drop and may need to reconnect.</div>
      <div class="note info" style="margin-top:10px">Changing the router IP or subnet automatically moves the DHCP pool to <span class="mono">x.x.x.10 – x.x.x.100</span> inside the new subnet.</div>
    </section>`;
}

function wanStatusHtml(wifi) {
  const sta = wifi.sta || {};
  const tone = sta.connected ? 'ok' : sta.configured ? 'err' : '';
  const text = sta.connected ? 'Connected' : sta.configured ? 'Disconnected' : 'Not configured';
  return `
    <section class="card">
      <div class="card-head">
        <div><h2>Upstream connection</h2><div class="sub">WiFi network that provides internet</div></div>
        <div class="card-actions"><span class="badge ${tone}">${text}</span></div>
      </div>
      <dl class="kv">
        <dt>Network</dt><dd>${sta.ssid ? esc(sta.ssid) : '<span class="dim">—</span>'}</dd>
        <dt>IP address</dt><dd class="mono">${sta.connected ? esc(sta.ip || '—') : '—'}</dd>
        <dt>Gateway</dt><dd class="mono">${sta.connected ? esc(sta.gateway || '—') : '—'}</dd>
        <dt>DNS</dt><dd class="mono">${sta.connected ? esc(sta.dns || '—') : '—'}</dd>
        <dt>Signal</dt><dd>${sta.connected ? signalHtml(sta.rssi) : '—'}</dd>
        <dt>Channel</dt><dd>${sta.connected ? (sta.channel ?? '—') : '—'}</dd>
      </dl>
      ${sta.configured ? `<div class="form-actions"><button class="btn sm danger" id="wan-disconnect" type="button">${icon('power')} Disconnect</button></div>` : ''}
    </section>`;
}

function wanFormHtml(wifi) {
  const sta = wifi.sta || {};
  return `
    <section class="card">
      <div class="card-head">
        <div><h2>Upstream settings</h2><div class="sub">Connect the router to your existing WiFi</div></div>
      </div>

      <form id="wan-form" class="form-grid">
        <div class="field span-2">
          <label class="toggle"><input type="checkbox" id="wan-enabled" ${sta.configured ? 'checked' : ''}><span class="track"></span> Connect to an upstream network</label>
        </div>

        <div class="field">
          <label for="wan-ssid">Upstream SSID</label>
          <input id="wan-ssid" maxlength="32" placeholder="Home WiFi or hotspot" value="${esc(sta.ssid || '')}">
          <div class="hint">Scan and pick a network, or type the name.</div>
        </div>

        <div class="field">
          <label for="wan-pass">Upstream password</label>
          <div class="input-wrap">
            <input id="wan-pass" type="password" autocomplete="new-password" placeholder="${sta.passwordSet ? 'Leave blank to keep current' : 'Network password'}">
            <button type="button" class="reveal" data-reveal="wan-pass" aria-label="Show password">${icon('eye')}</button>
          </div>
          <div class="hint">Leave blank to keep the saved password.</div>
        </div>

        <div class="field span-2">
          <button class="btn ghost sm" id="scan-btn" type="button">${icon('refresh', 'sm')} Scan for networks</button>
          <div id="scan-list" class="scan-list"></div>
        </div>

        <div class="field span-2">
          <label class="toggle"><input type="checkbox" id="wan-static" ${sta.useStatic ? 'checked' : ''}><span class="track"></span> Use a static IP address</label>
        </div>

        <div class="span-2" id="wan-static-fields" ${sta.useStatic ? '' : 'hidden'}>
          <div class="form-grid">
            <div class="field">
              <label for="wan-ip">IP address</label>
              <input id="wan-ip" class="mono" placeholder="192.168.1.50" value="${esc(sta.ip && sta.useStatic ? sta.ip : '')}">
            </div>
            <div class="field">
              <label for="wan-gw">Gateway</label>
              <input id="wan-gw" class="mono" placeholder="192.168.1.1" value="${esc(sta.gateway && sta.useStatic ? sta.gateway : '')}">
            </div>
            <div class="field">
              <label for="wan-mask">Subnet mask</label>
              <input id="wan-mask" class="mono" value="255.255.255.0">
            </div>
            <div class="field">
              <label for="wan-dns1">DNS 1</label>
              <input id="wan-dns1" class="mono" placeholder="Optional">
            </div>
            <div class="field">
              <label for="wan-dns2">DNS 2</label>
              <input id="wan-dns2" class="mono" placeholder="Optional">
            </div>
          </div>
        </div>

        <div class="form-actions span-2" style="margin-top:2px">
          <button class="btn primary" type="submit">${icon('check')} Save &amp; connect</button>
        </div>
      </form>

      <div class="note info" style="margin-top:14px">If the upstream is on a different channel, the ESP32 keeps the AP channel and shares the connection through NAT. 5 GHz upstream is only available on the ESP32-C5.</div>
    </section>`;
}

async function pageWireless(token) {
  const [wifi, info] = await Promise.all([
    api('/api/wifi/status'),
    api('/api/system/info'),
  ]);
  if (token !== state.renderToken) return;
  state.wifi = wifi;
  state.info = info;

  const tab = state.wirelessTab;
  $('#page').innerHTML = `
    <div class="tabs">
      <button class="tab ${tab === 'ap' ? 'active' : ''}" data-tab="ap">Access point</button>
      <button class="tab ${tab === 'wan' ? 'active' : ''}" data-tab="wan">Upstream (WAN)</button>
    </div>
    <div id="tab-body"></div>`;

  $$('.tab').forEach((btn) => {
    btn.addEventListener('click', () => {
      state.wirelessTab = btn.dataset.tab;
      navigate();
    });
  });

  const body = $('#tab-body');
  if (tab === 'ap') {
    body.innerHTML = apFormHtml(wifi, info);
    bindApForm(wifi);
  } else {
    body.innerHTML = wanStatusHtml(wifi) + wanFormHtml(wifi);
    bindWanForm(wifi);
    if (state.autoScan) {
      state.autoScan = false;
      doScan();
    }
  }
}

function bindApForm(wifi) {
  const ap = wifi.ap || {};
  const band = $('#ap-band');
  band.addEventListener('change', () => {
    $('#ap-channel').innerHTML = channelOptions(band.value, Number($('#ap-channel').value));
  });

  $('#ap-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    const password = val('#ap-pass');
    const body = {
      ssid: val('#ap-ssid'),
      channel: parseInt(val('#ap-channel'), 10),
      hidden: checked('#ap-hidden'),
      maxClients: parseInt(val('#ap-max'), 10),
      txPower: parseInt(val('#ap-tx'), 10),
      band: band.value,
      ip: val('#ap-ip'),
      subnet: val('#ap-subnet'),
    };
    if (!body.ssid) { toast('Network name is required', 'err'); return; }
    if (password && (password.length < 8 || password.length > 63)) {
      toast('WiFi password must be 8–63 characters', 'err');
      return;
    }
    if (password) body.password = password;

    try {
      await api('/api/wifi/ap', { method: 'POST', body });
      toast('Access point updated — radio restarting', 'ok');
      if (body.ssid !== ap.ssid) {
        toast(`Reconnect to “${body.ssid}” if you were on this WiFi`, 'info', 7000);
      }
      if (body.ip !== ap.ip) {
        toast(`Panel will move to http://${body.ip}/`, 'info', 8000);
      }
      setTimeout(() => { if (state.auth) navigate(); }, 3500);
    } catch (err) {
      toast(err.message, 'err');
    }
  });
}

function bindWanForm(wifi) {
  $('#wan-static').addEventListener('change', (event) => {
    $('#wan-static-fields').hidden = !event.target.checked;
  });

  $('#scan-btn').addEventListener('click', doScan);

  $('#wan-disconnect')?.addEventListener('click', async () => {
    try {
      await api('/api/wifi/wan/disconnect', { method: 'POST' });
      toast('Upstream disconnected', 'ok');
      setTimeout(() => { if (state.auth) navigate(); }, 800);
    } catch (err) {
      toast(err.message, 'err');
    }
  });

  $('#wan-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    const password = val('#wan-pass');
    const body = {
      ssid: val('#wan-ssid'),
      enabled: checked('#wan-enabled'),
      useStatic: checked('#wan-static'),
      ip: val('#wan-ip'),
      gateway: val('#wan-gw'),
      subnet: val('#wan-mask'),
      dns1: val('#wan-dns1'),
      dns2: val('#wan-dns2'),
    };
    if (body.enabled && !body.ssid) { toast('Upstream SSID is required', 'err'); return; }
    if (password.length > 63) { toast('Password must be at most 63 characters', 'err'); return; }
    if (password) body.password = password;

    try {
      await api('/api/wifi/wan', { method: 'POST', body });
      toast(body.enabled ? 'Connecting to upstream…' : 'Upstream disabled', 'ok');
      setTimeout(() => { if (state.auth) navigate(); }, 4000);
    } catch (err) {
      toast(err.message, 'err');
    }
  });
}

async function doScan() {
  const list = $('#scan-list');
  const btn = $('#scan-btn');
  if (!list || !btn) return;

  btn.disabled = true;
  btn.innerHTML = `${icon('refresh', 'sm')} Scanning…`;
  list.innerHTML = '<div class="scan-empty">Scanning for nearby networks…</div>';

  try {
    const data = await api('/api/wifi/scan');
    const networks = (data.networks || [])
      .filter((n) => n.ssid)
      .sort((a, b) => (b.rssi || -100) - (a.rssi || -100));

    if (!networks.length) {
      list.innerHTML = '<div class="scan-empty">No networks found. Try again.</div>';
      return;
    }

    list.innerHTML = networks.map((n) => {
      const level = n.rssi >= -55 ? 4 : n.rssi >= -65 ? 3 : n.rssi >= -75 ? 2 : 1;
      const bars = [1, 2, 3, 4].map((i) => `<i class="${i <= level ? 'on' : ''}"></i>`).join('');
      return `<button type="button" class="scan-item" data-ssid="${esc(n.ssid)}">
        <span class="ssid">${esc(n.ssid)}</span>
        ${n.band === '5' ? '<span class="badge accent">5G</span>' : ''}
        ${n.secure ? `<span class="dim">${icon('lock', 'sm')}</span>` : '<span class="badge">open</span>'}
        <span class="signal">${bars}</span>
        <span class="meta">ch ${n.channel} · ${n.rssi} dBm</span>
      </button>`;
    }).join('');

    $$('.scan-item', list).forEach((item) => {
      item.addEventListener('click', () => {
        $('#wan-ssid').value = item.dataset.ssid;
        $('#wan-pass').focus();
        toast(`Selected “${item.dataset.ssid}”`, 'info', 1800);
      });
    });
  } catch (err) {
    list.innerHTML = `<div class="scan-empty">${esc(err.message)}</div>`;
  } finally {
    btn.disabled = false;
    btn.innerHTML = `${icon('refresh', 'sm')} Scan for networks`;
  }
}

/* ============================================================
   Clients
   ============================================================ */

async function pageClients(token) {
  const data = await api('/api/wifi/clients');
  if (token !== state.renderToken) return;
  state.clients = data;

  const clients = (data.clients || []).slice().sort((a, b) => {
    if (!!a.blocked !== !!b.blocked) return a.blocked ? 1 : -1;
    if (!!a.online !== !!b.online) return a.online ? -1 : 1;
    return (a.name || a.mac).localeCompare(b.name || b.mac);
  });

  const rows = clients.map((c) => {
    const label = c.name || c.hostname || `Device · ${c.mac.slice(-8).toUpperCase()}`;
    const status = c.blocked
      ? '<span class="badge err">Blocked</span>'
      : c.online
        ? '<span class="badge ok">Online</span>'
        : '<span class="badge">Offline</span>';
    const uptime = c.online ? fmtUptime(c.connectedSec) : fmtAgo(c.lastSeenSec);
    return `<tr>
      <td>
        <div class="strong">${esc(label)}</div>
        <div class="cell-sub mono">${esc(c.mac)}</div>
      </td>
      <td class="mono">${c.ip ? esc(c.ip) : '<span class="dim">—</span>'}</td>
      <td>${signalHtml(c.rssi)}</td>
      <td class="nowrap">${uptime}</td>
      <td>${status}</td>
      <td class="right nowrap">
        <button class="btn sm ghost" data-act="rename" data-mac="${esc(c.mac)}">${icon('gear', 'sm')} Rename</button>
        <button class="btn sm ${c.blocked ? 'ghost' : 'danger'}" data-act="block" data-mac="${esc(c.mac)}">${c.blocked ? 'Unblock' : 'Block'}</button>
      </td>
    </tr>`;
  }).join('');

  $('#page').innerHTML = `
    <section class="card">
      <div class="card-head">
        <div><h2>Connected devices</h2><div class="sub">${data.online || 0} online · ${clients.length} known</div></div>
        <div class="card-actions">
          <button class="btn sm ghost" id="clients-refresh">${icon('refresh', 'sm')} Refresh</button>
        </div>
      </div>
      ${clients.length ? `
        <div class="table-wrap">
          <table>
            <thead><tr><th>Device</th><th>IP</th><th>Signal</th><th>Connected</th><th>Status</th><th class="right">Actions</th></tr></thead>
            <tbody>${rows}</tbody>
          </table>
        </div>` : `<div class="empty">${icon('users')}<p>No devices have connected yet.</p></div>`}
    </section>

    <div class="note info" style="margin-top:16px">Blocking a device removes it from the WiFi immediately. It can still see the network name, but cannot join until you unblock it. Names are stored on the router.</div>`;

  $('#clients-refresh')?.addEventListener('click', () => navigate());

  $$('[data-act]').forEach((btn) => {
    btn.addEventListener('click', () => {
      const mac = btn.dataset.mac;
      const client = clients.find((c) => c.mac === mac);
      if (!client) return;
      if (btn.dataset.act === 'rename') renameClient(client);
      else toggleBlock(client);
    });
  });
}

function renameClient(client) {
  openModal({
    title: 'Rename device',
    body: `<div class="field"><label for="rename-input">Device name</label>
      <input id="rename-input" maxlength="32" value="${esc(client.name || '')}" placeholder="${esc(client.hostname || client.mac)}">
      <div class="hint">Give this device a friendly name.</div></div>`,
    okText: 'Save name',
    onOpen: (dlg) => {
      const input = dlg.querySelector('#rename-input');
      input.focus();
      input.select();
    },
    onOk: async (dlg) => {
      const name = dlg.querySelector('#rename-input').value.trim();
      await api('/api/wifi/clients/rename', { method: 'POST', body: { mac: client.mac, name } });
      toast('Device renamed', 'ok');
      setTimeout(() => { if (state.auth) navigate(); }, 250);
    },
  });
}

function toggleBlock(client) {
  const blocking = !client.blocked;
  confirmAction(
    blocking ? 'Block device' : 'Unblock device',
    blocking
      ? `Block <strong>${esc(client.name || client.mac)}</strong> from joining this WiFi?`
      : `Allow <strong>${esc(client.name || client.mac)}</strong> to join this WiFi again?`,
    blocking ? 'Block' : 'Unblock',
    blocking,
    async () => {
      await api('/api/wifi/clients/block', { method: 'POST', body: { mac: client.mac, blocked: blocking } });
      toast(blocking ? 'Device blocked' : 'Device unblocked', 'ok');
      setTimeout(() => { if (state.auth) navigate(); }, 900);
    },
  );
}

/* ============================================================
   DHCP & NAT
   ============================================================ */

async function pageNetwork(token) {
  const [dhcp, nat] = await Promise.all([
    api('/api/dhcp'),
    api('/api/router/nat'),
  ]);
  if (token !== state.renderToken) return;
  state.nat = nat;
  setCaptiveBadge(!!dhcp.captivePortal);

  $('#page').innerHTML = `
    <div class="grid two">
      <section class="card">
        <div class="card-head">
          <div><h2>DHCP server</h2><div class="sub">Addresses handed to WiFi clients</div></div>
          <div class="card-actions"><span class="badge ${dhcp.applied ? 'ok' : 'err'}">${dhcp.applied ? 'Running' : 'Stopped'}</span></div>
        </div>
        <form id="dhcp-form" class="form-grid">
          <div class="field">
            <label for="dhcp-start">Pool start</label>
            <input id="dhcp-start" class="mono" value="${esc(dhcp.start)}">
          </div>
          <div class="field">
            <label for="dhcp-end">Pool end</label>
            <input id="dhcp-end" class="mono" value="${esc(dhcp.end)}">
          </div>
          <div class="field">
            <label for="dhcp-lease">Lease time (minutes)</label>
            <input id="dhcp-lease" type="number" min="1" max="10080" value="${dhcp.leaseMinutes}">
            <div class="hint">1 minute to 7 days.</div>
          </div>
          <div class="field">
            <label>DNS advertised</label>
            <input class="mono" value="${esc(dhcp.dns || '—')}" disabled>
            <div class="hint">Captive portal mode advertises the router as DNS.</div>
          </div>
          <div class="form-actions span-2" style="margin-top:2px">
            <button class="btn primary" type="submit">${icon('check')} Save DHCP settings</button>
          </div>
        </form>
        <div class="note info" style="margin-top:14px">The pool must sit inside the access point subnet and must not include the router's own IP. Saving restarts the DHCP server, so clients renew their addresses.</div>
      </section>

      <section class="card">
        <div class="card-head">
          <div><h2>NAT / Repeater</h2><div class="sub">Share the upstream connection</div></div>
          <div class="card-actions"><span class="badge ${nat.active ? 'ok' : nat.supported === false ? 'err' : 'warn'}">${nat.active ? 'Active' : nat.supported === false ? 'Unsupported' : 'Inactive'}</span></div>
        </div>

        <label class="toggle" style="margin-bottom:14px">
          <input type="checkbox" id="nat-toggle" ${nat.enabled ? 'checked' : ''} ${nat.supported === false ? 'disabled' : ''}>
          <span class="track"></span> Enable internet sharing (NAPT)
        </label>

        <dl class="kv">
          <dt>Upstream</dt><dd>${nat.upstreamConnected ? '<span class="badge ok">Connected</span>' : '<span class="badge err">Disconnected</span>'}</dd>
          <dt>NAPT state</dt><dd>${nat.active ? 'Active' : 'Inactive'}</dd>
          <dt>Firmware support</dt><dd>${nat.supported ? 'Available' : 'Missing'}</dd>
        </dl>

        ${nat.supported === false ? `<div class="note danger" style="margin-top:12px">This build does not include lwIP NAPT. Rebuild with <span class="mono">CONFIG_LWIP_IPV4_NAPT=y</span> to enable repeater mode.</div>` : ''}
        ${nat.error ? `<div class="note warn" style="margin-top:12px">${esc(nat.error)}</div>` : ''}

        <div class="note info" style="margin-top:12px">When NAT is off, the router works as a standalone access point: clients can reach the panel but not the internet. Turn it on to repeat the upstream WiFi.</div>
      </section>
    </div>`;

  $('#nat-toggle')?.addEventListener('change', async (event) => {
    const enabled = event.target.checked;
    event.target.disabled = true;
    try {
      await api('/api/router/nat', { method: 'POST', body: { enabled } });
      toast(enabled ? 'NAT enabled' : 'NAT disabled', 'ok');
      setTimeout(() => { if (state.auth) navigate(); }, 1200);
    } catch (err) {
      event.target.checked = !enabled;
      event.target.disabled = false;
      toast(err.message, 'err');
    }
  });

  $('#dhcp-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    const body = {
      start: val('#dhcp-start'),
      end: val('#dhcp-end'),
      leaseMinutes: parseInt(val('#dhcp-lease'), 10),
    };
    try {
      await api('/api/dhcp', { method: 'POST', body });
      toast('DHCP settings applied', 'ok');
      setTimeout(() => { if (state.auth) navigate(); }, 800);
    } catch (err) {
      toast(err.message, 'err');
    }
  });
}

/* ============================================================
   System
   ============================================================ */

function deviceInfoHtml(info) {
  const reset = RESET_REASONS[info.resetReason] || 'Unknown';
  return `
    <dl class="kv">
      <dt>Firmware</dt><dd>v${esc(info.version)}</dd>
      <dt>Build</dt><dd class="mono">${esc(info.build)}</dd>
      <dt>Chip</dt><dd>${esc(info.chip)} rev ${info.chipRevision} · ${info.cores} cores</dd>
      <dt>MAC address</dt><dd class="mono">${esc(info.mac)}</dd>
      <dt>Flash</dt><dd>${fmtBytes(info.flashSize)}</dd>
      <dt>SDK</dt><dd class="mono">${esc(info.sdk)}</dd>
      <dt>Uptime</dt><dd>${fmtUptime(info.uptime)}</dd>
      <dt>Free heap</dt><dd>${fmtBytes(info.freeHeap)} <span class="dim">(min ${fmtBytes(info.minFreeHeap)})</span></dd>
      <dt>Reset reason</dt><dd>${esc(reset)}</dd>
      <dt>Panel address</dt><dd class="mono">http://${esc(info.apIp)}</dd>
      <dt>Upstream IP</dt><dd class="mono">${info.staIp && info.staIp !== '0.0.0.0' ? esc(info.staIp) : '—'}</dd>
      <dt>NAPT</dt><dd>${info.naptSupported ? (info.naptActive ? '<span class="badge ok">Active</span>' : '<span class="badge warn">Available</span>') : '<span class="badge err">Unsupported</span>'}</dd>
    </dl>`;
}

async function pageSystem(token) {
  const [info, time] = await Promise.all([
    api('/api/system/info'),
    api('/api/system/time'),
  ]);
  if (token !== state.renderToken) return;
  state.info = info;

  const tz = time.tzOffsetMinutes ?? 0;
  const tzLabel = `${tz >= 0 ? '+' : ''}${(tz / 60).toFixed(tz % 60 ? 1 : 0)} h`;

  $('#page').innerHTML = `
    <div class="grid two">
      <section class="card">
        <div class="card-head"><div><h2>Device information</h2><div class="sub">Hardware and firmware details</div></div></div>
        ${deviceInfoHtml(info)}
      </section>

      <section class="card">
        <div class="card-head"><div><h2>Firmware update</h2><div class="sub">Upload a compiled .bin over the air</div></div></div>
        <div class="file-row">
          <input type="file" id="ota-file" accept=".bin,application/octet-stream">
          <button class="btn primary" id="ota-upload" type="button">${icon('upload')} Upload</button>
        </div>
        <div class="progress" id="ota-progress" style="margin-top:14px"><span></span></div>
        <div class="hint" id="ota-status" style="margin-top:8px">Select the firmware .bin built for this board. The router reboots when the update finishes.</div>
        <div class="note warn" style="margin-top:14px">Never power off the router during an update. A failed upload keeps the current firmware running.</div>
      </section>
    </div>

    <div class="grid two">
      <section class="card">
        <div class="card-head"><div><h2>Time &amp; schedule</h2><div class="sub">NTP clock and automatic reboot</div></div></div>
        <form id="time-form" class="form-grid">
          <div class="field">
            <label for="tz-offset">Timezone offset (minutes)</label>
            <input id="tz-offset" type="number" min="-720" max="840" step="15" value="${tz}">
            <div class="hint">IST = 330 · UTC = 0 · currently ${tzLabel}</div>
          </div>
          <div class="field">
            <label for="ntp-server">NTP server</label>
            <input id="ntp-server" value="${esc(time.ntpServer || 'pool.ntp.org')}">
          </div>
          <div class="field span-2">
            <label class="toggle"><input type="checkbox" id="ntp-enabled" ${time.ntpEnabled ? 'checked' : ''}><span class="track"></span> Synchronise time over the internet</label>
          </div>
          <div class="field span-2">
            <label class="toggle"><input type="checkbox" id="sched-enabled" ${time.scheduledReboot ? 'checked' : ''}><span class="track"></span> Reboot automatically every day</label>
          </div>
          <div class="field">
            <label for="sched-time">Reboot time</label>
            <input id="sched-time" type="time" value="${esc(time.scheduledRebootTime || '03:00')}">
          </div>
          <div class="field" style="align-self:end">
            <div class="hint">Router clock: ${time.synced ? esc(time.local) : 'not synced yet'}</div>
          </div>
          <div class="form-actions span-2" style="margin-top:2px">
            <button class="btn primary" type="submit">${icon('check')} Save time settings</button>
          </div>
        </form>
      </section>

      <section class="card">
        <div class="card-head"><div><h2>Backup &amp; restore</h2><div class="sub">Move settings between routers</div></div></div>
        <div class="form-actions" style="margin-top:0">
          <button class="btn ghost" id="backup-download">${icon('download')} Download config</button>
          <button class="btn ghost" id="backup-secrets">${icon('lock')} Include secrets</button>
        </div>
        <div class="divider"></div>
        <div class="field">
          <label for="restore-file">Restore from backup</label>
          <input type="file" id="restore-file" accept="application/json,.json">
          <div class="hint">The router reboots after restoring a configuration file.</div>
        </div>
        <div class="note warn" style="margin-top:14px">A backup with secrets contains your WiFi passwords and admin hash. Store it somewhere safe.</div>
      </section>
    </div>

    <div class="grid two">
      <section class="card">
        <div class="card-head"><div><h2>Admin password</h2><div class="sub">Used to sign in to this panel</div></div></div>
        <form id="password-form" class="form-grid">
          <div class="field span-2">
            <label for="pw-current">Current password</label>
            <input id="pw-current" type="password" autocomplete="current-password">
          </div>
          <div class="field">
            <label for="pw-new">New password</label>
            <input id="pw-new" type="password" autocomplete="new-password">
          </div>
          <div class="field">
            <label for="pw-confirm">Confirm new password</label>
            <input id="pw-confirm" type="password" autocomplete="new-password">
          </div>
          <div class="form-actions span-2" style="margin-top:2px">
            <button class="btn primary" type="submit">${icon('lock')} Change password</button>
          </div>
        </form>
      </section>

      <section class="card">
        <div class="card-head"><div><h2>Maintenance</h2><div class="sub">Restart or wipe the router</div></div></div>
        <div class="form-actions" style="margin-top:0">
          <button class="btn ghost" id="sys-reboot">${icon('power')} Reboot</button>
          <button class="btn danger" id="sys-reset">${icon('x')} Factory reset</button>
        </div>
        <div class="note danger" style="margin-top:14px">A factory reset restores the open Lesh network and the default panel login (Lesh / admin).</div>
        <div class="divider"></div>
        <div class="kv">
          <dt>Panel</dt><dd class="mono">http://${esc(info.apIp)}</dd>
          <dt>mDNS</dt><dd class="mono">http://${esc(info.hostname)}.local</dd>
        </div>
      </section>
    </div>`;

  bindSystemForms(info);
}

function bindSystemForms(info) {
  $('#ota-upload').addEventListener('click', () => {
    const file = $('#ota-file').files[0];
    if (!file) { toast('Choose a firmware .bin file first', 'err'); return; }
    if (!file.name.toLowerCase().endsWith('.bin')) { toast('Firmware must be a .bin file', 'err'); return; }
    uploadFirmware(file);
  });

  $('#backup-download').addEventListener('click', async () => {
    try {
      await download('/api/system/config', 'esp-router-backup.json');
    } catch (err) { toast(err.message, 'err'); }
  });

  $('#backup-secrets').addEventListener('click', () => {
    confirmAction(
      'Backup with secrets',
      'The file will include WiFi passwords and the admin password hash. Keep it private. Continue?',
      'Download',
      false,
      async () => { await download('/api/system/config?secrets=1', 'esp-router-backup-secrets.json'); },
    );
  });

  $('#restore-file').addEventListener('change', async (event) => {
    const file = event.target.files[0];
    event.target.value = '';
    if (!file) return;
    let parsed;
    try {
      parsed = JSON.parse(await file.text());
    } catch {
      toast('That file is not valid JSON', 'err');
      return;
    }
    confirmAction(
      'Restore configuration',
      'This overwrites the current configuration and reboots the router. Continue?',
      'Restore & reboot',
      false,
      async () => {
        await api('/api/system/config', { method: 'POST', body: parsed });
        waitForDevice('Configuration restored. Rebooting…');
      },
    );
  });

  $('#time-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    const body = {
      tzOffsetMinutes: parseInt(val('#tz-offset'), 10) || 0,
      ntpEnabled: checked('#ntp-enabled'),
      ntpServer: val('#ntp-server'),
      scheduledReboot: checked('#sched-enabled'),
      scheduledRebootTime: val('#sched-time') || '03:00',
    };
    try {
      await api('/api/system/time', { method: 'POST', body });
      toast('Time settings saved', 'ok');
    } catch (err) {
      toast(err.message, 'err');
    }
  });

  $('#password-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    const current = $('#pw-current').value;
    const next = $('#pw-new').value;
    const confirm = $('#pw-confirm').value;
    if (next.length < 8 || next.length > 64) { toast('New password must be 8–64 characters', 'err'); return; }
    if (next !== confirm) { toast('New passwords do not match', 'err'); return; }
    try {
      await api('/api/auth/password', { method: 'POST', body: { current, newPassword: next } });
      toast('Admin password changed', 'ok');
      event.target.reset();
    } catch (err) {
      toast(err.message, 'err');
    }
  });

  $('#sys-reboot').addEventListener('click', () => {
    confirmAction('Reboot router', 'All WiFi clients will disconnect briefly. Continue?', 'Reboot', false, async () => {
      await api('/api/system/reboot', { method: 'POST' });
      waitForDevice();
    });
  });

  $('#sys-reset').addEventListener('click', () => {
    openModal({
      title: 'Factory reset',
      body: `<p>This erases saved settings and passwords, then reboots with the default login.</p>
        <div class="field"><label for="reset-confirm">Type <span class="mono">RESET</span> to confirm</label>
        <input id="reset-confirm" class="mono" autocomplete="off" placeholder="RESET"></div>`,
      okText: 'Erase everything',
      danger: true,
      onOpen: (dlg) => dlg.querySelector('#reset-confirm').focus(),
      onOk: async (dlg) => {
        const text = dlg.querySelector('#reset-confirm').value.trim();
        if (text !== 'RESET') {
          toast('Type RESET exactly to confirm', 'err');
          return false;
        }
        await api('/api/system/factory-reset', { method: 'POST', body: { confirm: 'RESET' } });
        waitForDevice('Factory reset complete. Reconnect to Lesh.');
      },
    });
  });
}

function uploadFirmware(file) {
  const btn = $('#ota-upload');
  const bar = $('#ota-progress > span');
  const status = $('#ota-status');
  btn.disabled = true;
  bar.style.width = '0%';
  status.textContent = 'Starting upload…';

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/system/ota');
  xhr.setRequestHeader('X-CSRF-Token', state.csrf);

  xhr.upload.onprogress = (event) => {
    if (!event.lengthComputable) return;
    const pct = Math.round((event.loaded / event.total) * 100);
    bar.style.width = `${pct}%`;
    status.textContent = `Uploading firmware… ${pct}%`;
  };

  xhr.onload = () => {
    btn.disabled = false;
    let data = null;
    try { data = JSON.parse(xhr.responseText); } catch {}
    if (xhr.status === 200 && data && data.ok) {
      bar.style.width = '100%';
      status.textContent = 'Update installed. Rebooting…';
      waitForDevice('Firmware updated. The router is rebooting…');
    } else {
      const message = (data && data.error) || `Upload failed (${xhr.status})`;
      status.textContent = message;
      bar.style.width = '0%';
      toast(message, 'err');
    }
  };

  xhr.onerror = () => {
    btn.disabled = false;
    status.textContent = 'Upload failed — connection lost';
    bar.style.width = '0%';
    toast('Firmware upload failed', 'err');
  };

  const form = new FormData();
  form.append('firmware', file, file.name);
  xhr.send(form);
}

async function download(path, filename) {
  const res = await fetch(path, { cache: 'no-store' });
  if (!res.ok) {
    let message = `Download failed (${res.status})`;
    try { const data = await res.json(); if (data.error) message = data.error; } catch {}
    throw new Error(message);
  }
  const blob = await res.blob();
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 5000);
  toast('Backup downloaded', 'ok');
}

/* ============================================================
   Logs
   ============================================================ */

async function pageLogs(token) {
  $('#page').innerHTML = `
    <section class="card">
      <div class="card-head">
        <div><h2>Device log</h2><div class="sub">Live output from the firmware</div></div>
        <div class="card-actions"><span class="badge" id="log-count">0 lines</span></div>
      </div>
      <div class="log-toolbar">
        <button class="btn sm ghost" id="log-pause" type="button">${icon('x', 'sm')} Pause</button>
        <button class="btn sm ghost" id="log-clear" type="button">Clear</button>
        <label class="toggle"><input type="checkbox" id="log-scroll" checked><span class="track"></span> Auto-scroll</label>
        <span class="new-logs" id="log-new" hidden></span>
        <div class="spacer"></div>
        <button class="btn sm ghost" id="log-download" type="button">${icon('download', 'sm')} Download</button>
      </div>
      <div class="logview" id="logview"></div>
    </section>`;

  state.logPaused = false;
  state.logNew = 0;

  $('#log-pause').addEventListener('click', () => {
    state.logPaused = !state.logPaused;
    const btn = $('#log-pause');
    btn.innerHTML = state.logPaused ? `${icon('check', 'sm')} Resume` : `${icon('x', 'sm')} Pause`;
    if (!state.logPaused) {
      state.logNew = 0;
      updateLogNew();
      renderLogs();
    }
  });

  $('#log-clear').addEventListener('click', () => {
    state.logs = [];
    state.logSeqs.clear();
    state.logNew = 0;
    updateLogNew();
    renderLogs();
  });

  $('#log-download').addEventListener('click', () => {
    if (!state.logs.length) { toast('No log lines to download', 'err'); return; }
    const text = state.logs.map((l) => `${fmtClock(l.ms)} [${l.level || 'I'}] ${l.msg || ''}`).join('\n');
    const blob = new Blob([text], { type: 'text/plain' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'esp-router.log';
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 5000);
  });

  try {
    const data = await api('/api/system/logs?since=0');
    if (token !== state.renderToken) return;
    state.logs = [];
    state.logSeqs.clear();
    appendLogs(data.lines || []);
    renderLogs();
  } catch (err) {
    if (token !== state.renderToken) return;
    $('#logview').innerHTML = `<div class="empty">${esc(err.message)}</div>`;
  }
}

function updateLogNew() {
  const el = $('#log-new');
  if (!el) return;
  el.hidden = state.logNew <= 0;
  el.textContent = state.logNew > 0 ? `${state.logNew} new line${state.logNew === 1 ? '' : 's'}` : '';
}

function logLineEl(line) {
  const row = document.createElement('div');
  row.className = `logline ${line.level || 'I'}`;
  const t = document.createElement('span');
  t.className = 't';
  t.textContent = fmtClock(line.ms || 0);
  const lvl = document.createElement('span');
  lvl.className = 'lvl';
  lvl.textContent = line.level || 'I';
  const msg = document.createElement('span');
  msg.className = 'msg';
  msg.textContent = line.msg || '';
  row.append(t, lvl, msg);
  return row;
}

function renderLogs() {
  const view = $('#logview');
  if (!view) return;
  const frag = document.createDocumentFragment();
  state.logs.forEach((line) => frag.appendChild(logLineEl(line)));
  view.innerHTML = '';
  view.appendChild(frag);
  view.scrollTop = view.scrollHeight;
  setText('log-count', `${state.logs.length} line${state.logs.length === 1 ? '' : 's'}`);
  updateLogNew();
}

function appendLogs(lines) {
  if (!lines || !lines.length) return;
  const fresh = [];
  lines.forEach((line) => {
    if (line.seq && state.logSeqs.has(line.seq)) return;
    if (line.seq) state.logSeqs.add(line.seq);
    state.logs.push(line);
    fresh.push(line);
  });
  while (state.logs.length > 600) {
    const removed = state.logs.shift();
    if (removed && removed.seq) state.logSeqs.delete(removed.seq);
  }
  setText('log-count', `${state.logs.length} line${state.logs.length === 1 ? '' : 's'}`);

  if (state.route !== 'logs' || !fresh.length) return;
  if (state.logPaused) {
    state.logNew += fresh.length;
    updateLogNew();
    return;
  }
  const view = $('#logview');
  if (!view) return;
  const frag = document.createDocumentFragment();
  fresh.forEach((line) => frag.appendChild(logLineEl(line)));
  view.appendChild(frag);
  while (view.children.length > 600) view.removeChild(view.firstChild);
  if ($('#log-scroll')?.checked) view.scrollTop = view.scrollHeight;
}

/* ============================================================
   Setup wizard
   ============================================================ */

function renderSetup() {
  stopWs();
  state.auth = false;
  state.csrf = '';
  document.title = 'Setup — ESP-Router';

  const data = {
    adminPassword: '',
    adminPassword2: '',
    apSsid: 'ESP-Router',
    apPassword: '',
    apPassword2: '',
    wanSsid: '',
    wanPassword: '',
    nat: true,
  };
  let step = 1;

  $('#app').innerHTML = `
    <div class="setup-wrap">
      <div class="auth-bg"></div>
      <div class="card setup-card">
        <div class="setup-head">
          <div class="brand-mark">${icon('router', 'lg')}</div>
          <div><h2>Set up your router</h2><p>First boot · this takes about a minute</p></div>
        </div>
        <div class="steps" id="setup-steps"></div>
        <div class="wizard-body" id="wizard-body"></div>
        <div class="wizard-foot">
          <button class="btn ghost" id="wiz-back" type="button" hidden>Back</button>
          <div class="spacer"></div>
          <button class="btn primary" id="wiz-next" type="button">Continue ${icon('chevron')}</button>
        </div>
      </div>
    </div>`;

  const body = $('#wizard-body');
  const backBtn = $('#wiz-back');
  const nextBtn = $('#wiz-next');
  const stepsEl = $('#setup-steps');

  const stepHtml = (n) => {
    if (n === 1) {
      return `
        <div class="note info" style="margin-bottom:16px">Create the admin account for the router panel. You will use this password every time you sign in.</div>
        <div class="form-grid">
          <div class="field span-2">
            <label for="su-admin">Admin password</label>
            <div class="input-wrap">
              <input id="su-admin" type="password" autocomplete="new-password" value="${esc(data.adminPassword)}" placeholder="At least 8 characters">
              <button type="button" class="reveal" data-reveal="su-admin" aria-label="Show password">${icon('eye')}</button>
            </div>
          </div>
          <div class="field span-2">
            <label for="su-admin2">Confirm password</label>
            <input id="su-admin2" type="password" autocomplete="new-password" value="${esc(data.adminPassword2)}">
          </div>
        </div>`;
    }
    if (n === 2) {
      return `
        <div class="note info" style="margin-bottom:16px">Name the WiFi network this router will broadcast. Devices connect to it to reach the internet and this panel.</div>
        <div class="form-grid">
          <div class="field span-2">
            <label for="su-ssid">WiFi name (SSID)</label>
            <input id="su-ssid" maxlength="32" value="${esc(data.apSsid)}">
          </div>
          <div class="field">
            <label for="su-pass">WiFi password</label>
            <div class="input-wrap">
              <input id="su-pass" type="password" autocomplete="new-password" value="${esc(data.apPassword)}" placeholder="8–63 characters">
              <button type="button" class="reveal" data-reveal="su-pass" aria-label="Show password">${icon('eye')}</button>
            </div>
          </div>
          <div class="field">
            <label for="su-pass2">Confirm password</label>
            <input id="su-pass2" type="password" autocomplete="new-password" value="${esc(data.apPassword2)}">
          </div>
        </div>
        <div class="note" style="margin-top:14px">WPA2-PSK is used. After setup you can switch to an open network or change the band from the Wireless page.</div>`;
    }
    return `
      <div class="note info" style="margin-bottom:16px">Optionally connect this router to an existing WiFi network so it can share the internet. Skip this for a standalone access point.</div>
      <div class="form-grid">
        <div class="field span-2">
          <label for="su-wan">Upstream WiFi name</label>
          <input id="su-wan" maxlength="32" value="${esc(data.wanSsid)}" placeholder="Home WiFi / hotspot (optional)">
        </div>
        <div class="field span-2">
          <label for="su-wanpass">Upstream password</label>
          <div class="input-wrap">
            <input id="su-wanpass" type="password" autocomplete="new-password" value="${esc(data.wanPassword)}" placeholder="Leave empty for an open network">
            <button type="button" class="reveal" data-reveal="su-wanpass" aria-label="Show password">${icon('eye')}</button>
          </div>
        </div>
        <div class="field span-2">
          <label class="toggle"><input type="checkbox" id="su-nat" ${data.nat ? 'checked' : ''}><span class="track"></span> Share the internet connection (NAT)</label>
          <div class="hint">Requires the upstream network above. NAT is recommended for repeater mode.</div>
        </div>
      </div>`;
  };

  const drawSteps = () => {
    stepsEl.innerHTML = [1, 2, 3].map((n) => {
      const cls = n === step ? 'active' : n < step ? 'done' : '';
      const num = n < step ? icon('check', 'sm') : n;
      return `<div class="step ${cls}"><span class="num">${num}</span><span>${n === 1 ? 'Admin' : n === 2 ? 'WiFi' : 'Internet'}</span></div>${n < 3 ? '<div class="step-line"></div>' : ''}`;
    }).join('');
  };

  const readStep = (n) => {
    if (n === 1) {
      data.adminPassword = $('#su-admin').value;
      data.adminPassword2 = $('#su-admin2').value;
      if (data.adminPassword.length < 8) return 'Admin password must be at least 8 characters';
      if (data.adminPassword.length > 64) return 'Admin password must be at most 64 characters';
      if (data.adminPassword !== data.adminPassword2) return 'Admin passwords do not match';
    } else if (n === 2) {
      data.apSsid = val('#su-ssid');
      data.apPassword = $('#su-pass').value;
      data.apPassword2 = $('#su-pass2').value;
      if (!data.apSsid) return 'WiFi name is required';
      if (data.apSsid.length > 32) return 'WiFi name must be at most 32 characters';
      if (data.apPassword.length < 8 || data.apPassword.length > 63) return 'WiFi password must be 8–63 characters';
      if (data.apPassword !== data.apPassword2) return 'WiFi passwords do not match';
    } else {
      data.wanSsid = val('#su-wan');
      data.wanPassword = $('#su-wanpass').value;
      data.nat = checked('#su-nat') && !!data.wanSsid;
      if (data.wanSsid.length > 32) return 'Upstream name must be at most 32 characters';
      if (data.wanPassword.length > 63) return 'Upstream password must be at most 63 characters';
    }
    return null;
  };

  const showStep = (n) => {
    step = n;
    drawSteps();
    body.innerHTML = stepHtml(n);
    backBtn.hidden = n === 1;
    nextBtn.innerHTML = n === 3 ? 'Finish &amp; reboot' : `Continue ${icon('chevron')}`;
    const first = body.querySelector('input, select');
    if (first) first.focus();
  };

  const submit = async () => {
    nextBtn.disabled = true;
    nextBtn.textContent = 'Saving…';
    try {
      await api('/api/setup', {
        method: 'POST',
        silent401: true,
        body: {
          adminPassword: data.adminPassword,
          apSsid: data.apSsid,
          apPassword: data.apPassword,
          wanSsid: data.wanSsid,
          wanPassword: data.wanPassword,
          nat: data.nat,
        },
      });
      showOverlay(
        'Settings saved',
        `The router is restarting. Reconnect your device to <strong>${esc(data.apSsid)}</strong>, then open <span class="mono">http://192.168.4.1/</span> to sign in.`,
      );
      await sleep(4500);
      const deadline = Date.now() + 120000;
      while (Date.now() < deadline) {
        try {
          const res = await fetch('/api/auth/status', { cache: 'no-store' });
          if (res.ok) {
            const s = await res.json();
            if (!s.setupRequired) {
              location.hash = '';
              state.auth = false;
              state.csrf = '';
              hideOverlay();
              boot();
              return;
            }
          }
        } catch {}
        await sleep(2500);
      }
    } catch (err) {
      toast(err.message, 'err');
      nextBtn.disabled = false;
      nextBtn.innerHTML = 'Finish &amp; reboot';
    }
  };

  nextBtn.addEventListener('click', () => {
    const error = readStep(step);
    if (error) { toast(error, 'err'); return; }
    if (step < 3) {
      showStep(step + 1);
    } else {
      submit();
    }
  });

  backBtn.addEventListener('click', () => {
    if (step > 1) showStep(step - 1);
  });

  showStep(1);
}

/* ============================================================
   Reboot / overlay helpers
   ============================================================ */

function showOverlay(title, html = '') {
  hideOverlay();
  const el = document.createElement('div');
  el.className = 'overlay';
  el.id = 'reboot-overlay';
  el.innerHTML = `
    <div class="overlay-card">
      <div class="spinner big"></div>
      <h2>${esc(title)}</h2>
      <p>${html || 'The device is restarting. This page refreshes automatically.'}</p>
    </div>`;
  document.body.appendChild(el);
}

function hideOverlay() {
  $('#reboot-overlay')?.remove();
}

async function waitForDevice(message = '') {
  showOverlay('Waiting for the router…', message || 'The device is restarting. This page refreshes automatically.');
  await sleep(5000);
  const deadline = Date.now() + 120000;

  while (Date.now() < deadline) {
    try {
      const res = await fetch('/api/auth/status', { cache: 'no-store' });
      if (res.ok) {
        const s = await res.json();
        state.auth = false;
        state.csrf = '';
        hideOverlay();
        if (s.authenticated && s.csrf) {
          state.auth = true;
          state.csrf = s.csrf;
          state.version = s.version || state.version;
          state.hostname = s.hostname || state.hostname;
          await enterApp();
        } else {
          boot();
        }
        return;
      }
    } catch {}
    await sleep(2000);
  }

  showOverlay(
    'Still offline',
    'The router did not come back yet. Make sure you are connected to its WiFi network, then reload this page.',
  );
}

/* ============================================================
   Global event wiring
   ============================================================ */

document.addEventListener('click', (event) => {
  const reveal = event.target.closest('[data-reveal]');
  if (!reveal) return;
  const input = document.getElementById(reveal.dataset.reveal);
  if (!input) return;
  input.type = input.type === 'password' ? 'text' : 'password';
  reveal.setAttribute('aria-label', input.type === 'password' ? 'Show password' : 'Hide password');
});

document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible' && state.auth && !state.ws) {
    connectWs();
  }
});

async function pagePortal(token) {
  const [p, sessions] = await Promise.all([api('/api/portal/settings'), api('/api/portal/authorized')]);
  if (token !== state.renderToken) return;
  setCaptiveBadge(p.enabled);
  $('#page').innerHTML = `<div class="grid two"><section class="card"><h2>Guest access</h2>
    <form id="portal-settings" class="form-grid">
    <label class="toggle span-2"><input type="checkbox" name="enabled" ${p.enabled ? 'checked' : ''}><span class="track"></span> Require authorization</label>
    <label class="field">Page title<input name="title" maxlength="96" value="${esc(p.title)}"></label>
    <label class="field">Message<input name="message" maxlength="512" value="${esc(p.message)}"></label>
    <label class="field">Access method<select name="mode">
    ${['click','code','webhook'].map(m => `<option ${m === p.mode ? 'selected' : ''}>${m}</option>`).join('')}</select></label>
    <label class="field">Session (minutes)<input type="number" name="minutes" min="1" max="10080" required value="${p.minutes}"></label>
    <label class="field">Access code<input type="password" name="code" maxlength="64" placeholder="Keep current"></label>
    <label class="field">Webhook URL<input name="webhook" value="${esc(p.webhook)}" placeholder="http://server/decision"></label>
    <label class="field">Webhook bearer secret<input type="password" name="secret" placeholder="Keep current"></label>
    <label class="field">Grant API key<input type="password" name="apiKey" placeholder="Keep current"></label>
    <button class="btn primary">Save and clear sessions</button></form></section>
    <section class="card"><h2>Guest HTML</h2><p>${p.custom ? 'Custom page installed' : 'Default page installed'}</p>
    <input type="file" id="guest-file" accept=".html,text/html">
    <button id="upload-page" class="btn primary">Upload</button> <button id="reset-page" class="btn ghost">Restore default</button>
    <p><a href="/portal" target="_blank" rel="noopener">HTTP preview</a> ·
    <a href="https://${esc(p.apIp)}/portal" target="_blank" rel="noopener">HTTPS guest page</a> ·
    <a href="/api/portal/certificate" download="lesh-portal.crt">Download router certificate</a></p>
    <p>Use a single HTML file (up to 8 KiB). POST JSON to <code>/api/portal/authorize</code> and check <code>authorized</code>.</p>
    <h2>Authorized clients</h2>${sessions.clients.map(c => `<p>${esc(c.name || c.hostname || c.mac)} · ${esc(c.ip)}
      <button class="btn ghost" data-revoke="${esc(c.ip)}">Revoke</button></p>`).join('') || '<p>No active sessions.</p>'}</section></div>`;
  const save = async (path, body) => {
    try {
      await api('/api/portal/' + path, {method:'POST', body});
      if (token === state.renderToken) await navigate();
    } catch (err) { toast(err.message, 'err'); }
  };
  $('#portal-settings').onsubmit = event => {
    event.preventDefault();
    const body = Object.fromEntries(new FormData(event.target));
    body.enabled = event.target.elements.enabled.checked;
    body.minutes = Number(body.minutes);
    for (const key of ['code','apiKey','secret']) if (!body[key]) delete body[key];
    save('settings', body);
  };
  $('#upload-page').onclick = async () => {
    const file = $('#guest-file').files[0];
    if (!file || !file.size || file.size > 8192) return toast('Select an HTML file up to 8 KiB', 'err');
    await save('page', {html: await file.text()});
  };
  $('#reset-page').onclick = () => save('page', {reset:true});
  $$('[data-revoke]').forEach(btn => { btn.onclick = () => save('revoke', {ip:btn.dataset.revoke}); });
}

const ROUTES = {
  dashboard: pageDashboard,
  wireless: pageWireless,
  clients: pageClients,
  network: pageNetwork,
  portal: pagePortal,
  system: pageSystem,
  logs: pageLogs,
};

/* Go */
boot();
