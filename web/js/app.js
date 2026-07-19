// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          web/js/app.js
// Purpose:       NightWatcher2 web UI: login flow + dashboard, sensor/weather
//                CRUD, readings query + graph, events, users, and database
//                maintenance, all against the /api/v1 API.
// Created:       2026-07-18
// Last Modified: 2026-07-18
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
'use strict';

const state = { user: null, view: 'dashboard' };
let main = null;
let editingSensor = null;
let editingWeather = null;

// ---- tiny DOM helper -------------------------------------------------------
function el(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === null || v === undefined) continue;
    if (k === 'class') e.className = v;
    else if (k.startsWith('on') && typeof v === 'function') e.addEventListener(k.slice(2), v);
    else e.setAttribute(k, v);
  }
  for (const kid of kids.flat()) {
    if (kid === null || kid === undefined) continue;
    e.append(kid.nodeType ? kid : document.createTextNode(String(kid)));
  }
  return e;
}
function msg(kind, text) { return el('div', { class: 'msg ' + kind }, text); }
function fmtNum(x) { return x == null ? '—' : (Math.round(x * 10) / 10).toString(); }
function fmtMag(x) { return x == null ? '—' : Number(x).toFixed(2); }

function table(cols, rows) {
  return el('table', { class: 'tbl' },
    el('thead', {}, el('tr', {}, ...cols.map(c => el('th', {}, c.label)))),
    el('tbody', {}, ...rows.map(r =>
      el('tr', {}, ...cols.map(c => el('td', {}, c.render ? c.render(r) : r[c.key]))))));
}
function field(label, name, value, type, step) {
  const attrs = { name, type: type || 'text', value: value == null ? '' : value };
  // Number inputs default to step="any" so decimals (e.g. lat/lon) are accepted.
  if (type === 'number') attrs.step = step || 'any';
  return el('label', {}, label, el('input', attrs));
}

// ---- API -------------------------------------------------------------------
async function api(method, path, body, opts) {
  opts = opts || {};
  const init = { method, headers: {} };
  if (body !== undefined) { init.headers['Content-Type'] = 'application/json'; init.body = JSON.stringify(body); }
  const res = await fetch('/api/v1' + path, init);
  const text = await res.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch (_) { /* non-JSON */ }
  if (res.status === 401 && !opts.noAuthRedirect) { state.user = null; showLogin(); throw new Error('unauthorized'); }
  if (!res.ok) throw new Error((data && data.error) || res.statusText);
  return data;
}
const isAdmin = () => state.user && state.user.role === 'admin';

// ---- graph -----------------------------------------------------------------
function drawGraph(container, readings) {
  const rs = [...readings].reverse();  // API returns newest-first
  const xs = rs.map(r => Date.parse(r.ts_utc.replace(' ', 'T') + 'Z') / 1000);
  const mag = rs.map(r => r.mag_arcsec2);
  const temp = rs.map(r => r.temp_c);
  const opts = {
    width: Math.max(320, container.clientWidth - 20), height: 320,
    scales: { x: { time: true } },
    series: [
      {},
      { label: 'mag/arcsec²', stroke: '#4ea1ff', width: 2, scale: 'mag' },
      { label: 'temp °C', stroke: '#ff8c42', width: 1, scale: 'temp' },
    ],
    axes: [
      { stroke: '#8fa0c0', grid: { stroke: '#26324f' }, ticks: { stroke: '#26324f' } },
      { scale: 'mag', stroke: '#8fa0c0', grid: { stroke: '#26324f' }, ticks: { stroke: '#26324f' } },
      { scale: 'temp', side: 1, stroke: '#8fa0c0', grid: { show: false } },
    ],
  };
  container.innerHTML = '';
  // eslint-disable-next-line no-undef
  new uPlot(opts, [xs, mag, temp], container);
}

// ---- views -----------------------------------------------------------------
async function viewDashboard() {
  const frag = document.createDocumentFragment();
  frag.append(el('h2', {}, 'Dashboard'));
  const health = await api('GET', '/health');
  frag.append(el('div', { class: 'row', style: 'margin-bottom:1rem' },
    el('span', { class: 'pill ' + (health.db === 'ok' ? 'ok' : 'suspect') }, 'database: ' + health.db),
    el('span', { class: 'muted' }, 'nightwatcherd ' + health.version)));

  const sensors = await api('GET', '/sensors');
  if (!sensors.length) {
    frag.append(el('p', { class: 'muted' }, 'No sensors registered yet — add one under Sensors.'));
    return frag;
  }
  const grid = el('div', { class: 'grid' });
  for (const s of sensors) {
    let latest = null;
    try { const r = await api('GET', `/sensors/${encodeURIComponent(s.id)}/readings?limit=1`); latest = r[0]; } catch (_) { /* ignore */ }
    grid.append(el('div', { class: 'card' },
      el('h3', {}, s.name || s.id, el('span', { class: 'pill ' + (s.status || '') }, s.status || '')),
      el('div', { class: 'muted', style: 'margin-bottom:.5rem' }, s.id + (s.site ? ' · ' + s.site : '')),
      latest
        ? el('div', {},
          el('div', { class: 'big' }, fmtMag(latest.mag_arcsec2), el('span', { class: 'unit' }, ' mag/arcsec²')),
          el('div', { class: 'row', style: 'margin-top:.35rem' },
            el('span', { class: 'pill ' + (latest.quality || '') }, latest.quality || ''),
            el('span', { class: 'muted' }, fmtNum(latest.temp_c) + ' °C'),
            el('span', { class: 'muted' }, latest.ts_utc + ' UTC')))
        : el('div', { class: 'muted' }, 'no readings yet'),
      isAdmin()
        ? el('div', { style: 'margin-top:.7rem' },
          el('button', {
            class: 'btn sm', onclick: async () => {
              try { await api('POST', `/sensors/${encodeURIComponent(s.id)}/poll`); renderView(); }
              catch (e) { alert(e.message); }
            }
          }, 'Poll now'))
        : null));
  }
  frag.append(grid);
  return frag;
}

function sensorForm(s) {
  const editing = !!s;
  const f = el('form', { class: 'form card', style: 'margin-bottom:1rem' });
  f.append(el('h3', {}, editing ? 'Edit ' + s.id : 'Add sensor'));
  const g = el('div', { class: 'form-grid' });
  if (!editing) g.append(field('ID *', 'id', ''));
  g.append(field('TCP host:port', 'tcp', editing ? s.address : ''));
  g.append(field('Name', 'name', editing ? s.name : ''));
  g.append(field('Site', 'site', editing ? s.site : ''));
  g.append(field('Latitude', 'latitude', editing ? s.latitude : '', 'number'));
  g.append(field('Longitude', 'longitude', editing ? s.longitude : '', 'number'));
  g.append(field('Elevation (m)', 'elevation_m', editing ? s.elevation_m : '', 'number'));
  g.append(field('Timezone', 'timezone', editing ? s.timezone : ''));
  g.append(field('Poll interval (s)', 'poll_interval_s', editing ? s.poll_interval_s : 300, 'number', '1'));
  g.append(field('Model', 'model', editing ? s.model : ''));
  const statusSel = el('select', { name: 'status' },
    ...['active', 'inactive', 'retired'].map(o => el('option', { value: o, selected: editing && s.status === o ? 'selected' : null }, o)));
  g.append(el('label', {}, 'Status', statusSel));
  f.append(g);
  const err = el('div');
  f.append(err);
  f.append(el('div', { class: 'row' },
    el('button', { class: 'btn', type: 'submit' }, editing ? 'Save' : 'Add'),
    editing ? el('button', { class: 'btn ghost', type: 'button', onclick: () => { editingSensor = null; renderView(); } }, 'Cancel') : null));
  f.addEventListener('submit', async e => {
    e.preventDefault();
    const fd = new FormData(f);
    const body = {};
    for (const [k, v] of fd.entries()) {
      if (v === '') continue;
      body[k] = ['latitude', 'longitude', 'elevation_m', 'poll_interval_s'].includes(k) ? Number(v) : v;
    }
    try {
      if (editing) { delete body.id; await api('PATCH', `/sensors/${encodeURIComponent(s.id)}`, body); }
      else { if (!body.id) { err.innerHTML = ''; err.append(msg('err', 'ID is required')); return; } await api('POST', '/sensors', body); }
      editingSensor = null; renderView();
    } catch (ex) { err.innerHTML = ''; err.append(msg('err', ex.message)); }
  });
  return f;
}

async function viewSensors() {
  const frag = document.createDocumentFragment();
  frag.append(el('h2', {}, 'Sensors'));
  if (isAdmin()) frag.append(sensorForm(editingSensor));
  const sensors = await api('GET', '/sensors');
  const cols = [
    { label: 'ID', render: s => s.id },
    { label: 'Name', render: s => s.name || '—' },
    { label: 'Site', render: s => s.site || '—' },
    { label: 'Connection', render: s => `${s.transport} ${s.address}` },
    { label: 'Lat, Lon', render: s => s.latitude != null ? `${s.latitude}, ${s.longitude}` : '—' },
    { label: 'Elev', render: s => s.elevation_m != null ? s.elevation_m + ' m' : '—' },
    { label: 'Interval', render: s => s.poll_interval_s + 's' },
    { label: 'Status', render: s => el('span', { class: 'pill ' + (s.status || '') }, s.status) },
  ];
  if (isAdmin()) cols.push({
    label: '', render: s => el('div', { class: 'row' },
      el('button', { class: 'btn ghost sm', onclick: () => { editingSensor = s; renderView(); } }, 'Edit'),
      el('button', {
        class: 'btn danger sm', onclick: async () => {
          if (confirm('Delete sensor ' + s.id + ' and its readings?')) {
            try { await api('DELETE', '/sensors/' + encodeURIComponent(s.id)); renderView(); } catch (e) { alert(e.message); }
          }
        }
      }, 'Delete'))
  });
  frag.append(el('div', { class: 'scroll' }, table(cols, sensors)));
  return frag;
}

function weatherForm(w) {
  const editing = !!w;
  const f = el('form', { class: 'form card', style: 'margin-bottom:1rem' });
  f.append(el('h3', {}, editing ? 'Edit ' + w.id : 'Add weather station'));
  const g = el('div', { class: 'form-grid' });
  if (!editing) g.append(field('ID *', 'id', ''));
  g.append(field('Name', 'name', editing ? w.name : ''));
  g.append(field('Site', 'site', editing ? w.site : ''));
  g.append(field('Model', 'model', editing ? w.model : 'Ambient Weather WS-2000'));
  g.append(field('Transport', 'transport', editing ? w.transport : 'http'));
  g.append(field('Address', 'address', editing ? w.address : ''));
  g.append(field('Latitude', 'latitude', editing ? w.latitude : '', 'number'));
  g.append(field('Longitude', 'longitude', editing ? w.longitude : '', 'number'));
  g.append(field('Elevation (m)', 'elevation_m', editing ? w.elevation_m : '', 'number'));
  g.append(field('Timezone', 'timezone', editing ? w.timezone : ''));
  f.append(g);
  const err = el('div'); f.append(err);
  f.append(el('div', { class: 'row' },
    el('button', { class: 'btn', type: 'submit' }, editing ? 'Save' : 'Add'),
    editing ? el('button', { class: 'btn ghost', type: 'button', onclick: () => { editingWeather = null; renderView(); } }, 'Cancel') : null));
  f.addEventListener('submit', async e => {
    e.preventDefault();
    const fd = new FormData(f);
    const body = {};
    for (const [k, v] of fd.entries()) {
      if (v === '') continue;
      body[k] = ['latitude', 'longitude', 'elevation_m'].includes(k) ? Number(v) : v;
    }
    try {
      if (editing) { delete body.id; await api('PATCH', `/weather-stations/${encodeURIComponent(w.id)}`, body); }
      else { if (!body.id) { err.innerHTML = ''; err.append(msg('err', 'ID is required')); return; } await api('POST', '/weather-stations', body); }
      editingWeather = null; renderView();
    } catch (ex) { err.innerHTML = ''; err.append(msg('err', ex.message)); }
  });
  return f;
}

async function viewWeather() {
  const frag = document.createDocumentFragment();
  frag.append(el('h2', {}, 'Weather stations'));
  if (isAdmin()) frag.append(weatherForm(editingWeather));
  const rows = await api('GET', '/weather-stations');
  const cols = [
    { label: 'ID', render: w => w.id },
    { label: 'Name', render: w => w.name || '—' },
    { label: 'Model', render: w => w.model || '—' },
    { label: 'Connection', render: w => `${w.transport || ''} ${w.address || ''}` },
    { label: 'Status', render: w => el('span', { class: 'pill ' + (w.status || '') }, w.status) },
  ];
  if (isAdmin()) cols.push({
    label: '', render: w => el('div', { class: 'row' },
      el('button', { class: 'btn ghost sm', onclick: () => { editingWeather = w; renderView(); } }, 'Edit'),
      el('button', {
        class: 'btn danger sm', onclick: async () => {
          if (confirm('Delete weather station ' + w.id + '?')) {
            try { await api('DELETE', '/weather-stations/' + encodeURIComponent(w.id)); renderView(); } catch (e) { alert(e.message); }
          }
        }
      }, 'Delete'))
  });
  frag.append(rows.length ? el('div', { class: 'scroll' }, table(cols, rows)) : el('p', { class: 'muted' }, 'No weather stations registered.'));
  return frag;
}

async function viewQuery() {
  const frag = document.createDocumentFragment();
  frag.append(el('h2', {}, 'Query & graph'));
  const sensors = await api('GET', '/sensors');
  if (!sensors.length) { frag.append(el('p', { class: 'muted' }, 'No sensors yet.')); return frag; }

  const sel = el('select', { style: 'max-width:260px' }, ...sensors.map(s => el('option', { value: s.id }, s.name ? `${s.name} (${s.id})` : s.id)));
  const rangeSel = el('select', {},
    el('option', { value: 'day' }, 'Last 24 hours'),
    el('option', { value: 'week', selected: 'selected' }, 'Last 7 days'),
    el('option', { value: 'month' }, 'Last 30 days'),
    el('option', { value: 'year' }, 'Last year'),
    el('option', { value: 'custom' }, 'Custom range'));
  const fromInput = el('input', { type: 'date', style: 'width:auto' });
  const toInput = el('input', { type: 'date', style: 'width:auto' });
  const customWrap = el('span', { class: 'row', style: 'display:none' },
    el('span', { class: 'muted' }, 'From'), fromInput, el('span', { class: 'muted' }, 'To'), toInput);

  const chart = el('div', { class: 'chart' });
  const caption = el('div', { class: 'muted', style: 'margin:.4rem 0' });
  const tblWrap = el('div', { class: 'scroll', style: 'margin-top:1rem' });

  const toUtc = ms => new Date(ms).toISOString().slice(0, 19).replace('T', ' ');
  const load = async () => {
    let from = '', to = '';
    const days = { day: 1, week: 7, month: 30, year: 365 };
    if (rangeSel.value === 'custom') {
      if (fromInput.value) from = fromInput.value + ' 00:00:00';
      if (toInput.value) to = toInput.value + ' 23:59:59';
    } else {
      from = toUtc(Date.now() - days[rangeSel.value] * 86400000);
    }
    const p = new URLSearchParams({ limit: '50000' });
    if (from) p.set('from', from);
    if (to) p.set('to', to);
    try {
      const rs = await api('GET', `/sensors/${encodeURIComponent(sel.value)}/readings?${p}`);
      caption.textContent = `${rs.length} reading(s)` + (from ? `  ·  from ${from} UTC` : '') + (to ? `  to ${to} UTC` : '');
      if (!rs.length) { chart.innerHTML = ''; chart.append(el('p', { class: 'muted' }, 'No readings in this range.')); tblWrap.innerHTML = ''; return; }
      drawGraph(chart, rs);
      const cols = [
        { label: 'Time (UTC)', render: r => r.ts_utc },
        { label: 'mag/arcsec²', render: r => fmtMag(r.mag_arcsec2) },
        { label: 'temp °C', render: r => fmtNum(r.temp_c) },
        { label: 'freq Hz', render: r => r.freq_hz },
        { label: 'quality', render: r => el('span', { class: 'pill ' + (r.quality || '') }, r.quality) },
        { label: 'source', render: r => r.source },
      ];
      tblWrap.innerHTML = ''; tblWrap.append(table(cols, rs));
    } catch (e) { chart.innerHTML = ''; chart.append(msg('err', e.message)); }
  };

  rangeSel.addEventListener('change', () => {
    customWrap.style.display = rangeSel.value === 'custom' ? '' : 'none';
    if (rangeSel.value !== 'custom') load();
  });
  sel.addEventListener('change', load);

  frag.append(el('div', { class: 'toolbar' },
    el('span', { class: 'muted' }, 'Sensor'), sel,
    el('span', { class: 'muted' }, 'Range'), rangeSel,
    customWrap,
    el('button', { class: 'btn', onclick: load }, 'Load')));
  frag.append(caption, chart, tblWrap);
  setTimeout(load, 0);
  return frag;
}

async function viewEvents() {
  const frag = document.createDocumentFragment();
  frag.append(el('h2', {}, 'Events'));
  const ev = await api('GET', '/events?limit=100');
  const lvl = l => l === 'error' ? 'suspect' : l === 'warning' ? 'saturated' : 'ok';
  const cols = [
    { label: 'Time (UTC)', render: e => e.ts_utc },
    { label: 'Level', render: e => el('span', { class: 'pill ' + lvl(e.level) }, e.level) },
    { label: 'Source', render: e => e.source },
    { label: 'Device', render: e => e.device_id || '—' },
    { label: 'Event', render: e => e.event },
    { label: 'Detail', render: e => e.detail || '—' },
  ];
  frag.append(el('div', { class: 'scroll' }, table(cols, ev)));
  return frag;
}

async function viewUsers() {
  const frag = document.createDocumentFragment();
  frag.append(el('h2', {}, 'Users'));
  const f = el('form', { class: 'form card', style: 'margin-bottom:1rem' });
  f.append(el('h3', {}, 'Add user'));
  const g = el('div', { class: 'form-grid' });
  g.append(field('Username', 'username', ''));
  g.append(field('Password', 'password', '', 'password'));
  g.append(el('label', {}, 'Role', el('select', { name: 'role' }, el('option', { value: 'viewer' }, 'viewer'), el('option', { value: 'admin' }, 'admin'))));
  f.append(g);
  const err = el('div'); f.append(err);
  f.append(el('button', { class: 'btn', type: 'submit' }, 'Add user'));
  f.addEventListener('submit', async e => {
    e.preventDefault();
    const fd = new FormData(f);
    try { await api('POST', '/users', { username: fd.get('username'), password: fd.get('password'), role: fd.get('role') }); renderView(); }
    catch (ex) { err.innerHTML = ''; err.append(msg('err', ex.message)); }
  });
  frag.append(f);
  const users = await api('GET', '/users');
  const cols = [
    { label: 'Username', render: u => u.username },
    { label: 'Role', render: u => u.role },
    { label: 'Must change pw', render: u => u.must_change_password ? 'yes' : 'no' },
    { label: 'Created', render: u => u.created_at },
    {
      label: '', render: u => el('button', {
        class: 'btn danger sm', onclick: async () => {
          if (confirm('Delete user ' + u.username + '?')) {
            try { await api('DELETE', '/users/' + encodeURIComponent(u.username)); renderView(); } catch (e) { alert(e.message); }
          }
        }
      }, 'Delete')
    },
  ];
  frag.append(el('div', { class: 'scroll' }, table(cols, users)));
  return frag;
}

async function viewDatabase() {
  const frag = document.createDocumentFragment();
  frag.append(el('h2', {}, 'Database'));
  const st = await api('GET', '/db/status');
  const cols = [
    { label: 'Table', render: t => t.table },
    { label: 'Present', render: t => t.present ? 'yes' : 'no' },
    { label: 'Rows', render: t => t.rows },
  ];
  frag.append(el('div', { class: 'scroll' }, table(cols, st.tables)));
  frag.append(el('div', { style: 'margin:1rem 0' },
    el('button', {
      class: 'btn', onclick: async () => {
        if (confirm('Create any missing tables from schema.sql?')) {
          try { await api('POST', '/db/init'); renderView(); } catch (e) { alert(e.message); }
        }
      }
    }, 'Initialize / repair schema')));

  const sensors = await api('GET', '/sensors');
  const pf = el('form', { class: 'form card' });
  pf.append(el('h3', {}, 'Prune readings'));
  const g = el('div', { class: 'form-grid' });
  g.append(el('label', {}, 'Sensor', el('select', { name: 'sensor' }, ...sensors.map(s => el('option', { value: s.id }, s.id)))));
  g.append(field('Delete readings before (YYYY-MM-DD)', 'before', ''));
  pf.append(g);
  const perr = el('div'); pf.append(perr);
  pf.append(el('button', { class: 'btn danger', type: 'submit' }, 'Prune'));
  pf.addEventListener('submit', async e => {
    e.preventDefault();
    const fd = new FormData(pf);
    if (!fd.get('sensor')) { perr.innerHTML = ''; perr.append(msg('err', 'pick a sensor')); return; }
    if (!confirm('Delete readings before ' + fd.get('before') + '?')) return;
    try {
      const r = await api('DELETE', `/sensors/${encodeURIComponent(fd.get('sensor'))}/readings?before=${encodeURIComponent(fd.get('before'))}`);
      perr.innerHTML = ''; perr.append(msg('ok', `Deleted ${r.deleted} readings`));
    } catch (ex) { perr.innerHTML = ''; perr.append(msg('err', ex.message)); }
  });
  frag.append(pf);
  return frag;
}

const VIEWS = {
  dashboard: { label: 'Dashboard', render: viewDashboard },
  query: { label: 'Query & graph', render: viewQuery },
  sensors: { label: 'Sensors', render: viewSensors },
  weather: { label: 'Weather', render: viewWeather },
  events: { label: 'Events', render: viewEvents },
  users: { label: 'Users', admin: true, render: viewUsers },
  database: { label: 'Database', admin: true, render: viewDatabase },
};

function currentViewFromHash() {
  const h = (location.hash || '').replace('#', '');
  return VIEWS[h] ? h : 'dashboard';
}

window.addEventListener('hashchange', () => {
  if (!state.user) return;
  const v = currentViewFromHash();
  state.view = (VIEWS[v].admin && !isAdmin()) ? 'dashboard' : v;
  refreshNav();
  renderView();
});

// ---- shell -----------------------------------------------------------------
async function renderView() {
  if (!main) return;
  main.innerHTML = '';
  main.append(el('p', { class: 'muted' }, 'Loading…'));
  try {
    const node = await VIEWS[state.view].render();
    main.innerHTML = ''; main.append(node);
  } catch (e) {
    if (e.message !== 'unauthorized') { main.innerHTML = ''; main.append(msg('err', e.message)); }
  }
}

function renderApp() {
  const app = document.getElementById('app');
  app.innerHTML = '';
  state.view = currentViewFromHash();
  if (VIEWS[state.view].admin && !isAdmin()) state.view = 'dashboard';
  const nav = el('nav', { class: 'tabs' });
  for (const [key, v] of Object.entries(VIEWS)) {
    if (v.admin && !isAdmin()) continue;
    nav.append(el('button', { class: state.view === key ? 'active' : '', onclick: () => { location.hash = key; } }, v.label));
  }
  const header = el('header', { class: 'top' },
    el('div', { class: 'brand' }, '🔭 NightWatcher2', el('small', {}, 'Dark Sky Network')),
    nav,
    el('div', { class: 'spacer' }),
    el('div', { class: 'who' }, state.user.username + ' · ' + state.user.role),
    el('button', {
      class: 'btn ghost sm', onclick: async () => {
        try { await api('POST', '/logout'); } catch (_) { /* ignore */ }
        state.user = null; showLogin();
      }
    }, 'Log out'));
  main = el('main', {});
  app.append(header, main);
  renderView();
}

function refreshNav() {
  document.querySelectorAll('nav.tabs button').forEach(b => {
    b.classList.toggle('active', b.textContent === VIEWS[state.view].label);
  });
}

// ---- login / password ------------------------------------------------------
function loginShell(title, subtitle, banner, form, footer) {
  const app = document.getElementById('app');
  app.innerHTML = '';
  app.append(el('div', { class: 'login-wrap' },
    el('div', { class: 'card login-card' },
      el('div', { class: 'logo' }, title),
      subtitle ? el('div', { class: 'sub' }, subtitle) : null,
      banner ? el('div', { class: 'banner' }, banner) : null,
      form,
      footer || null)));
}

function showLogin() {
  const err = el('div');
  const f = el('form', { class: 'form' });
  const user = el('input', { name: 'username', placeholder: 'username' });
  const pass = el('input', { name: 'password', type: 'password', placeholder: 'password' });
  f.append(el('label', {}, 'Username', user), el('label', {}, 'Password', pass), err,
    el('button', { class: 'btn', type: 'submit' }, 'Sign in'));
  f.addEventListener('submit', async e => {
    e.preventDefault();
    err.innerHTML = '';
    try {
      const r = await api('POST', '/login', { username: user.value, password: pass.value }, { noAuthRedirect: true });
      if (r.must_change_password) showChangePassword(true);
      else boot();
    } catch (ex) {
      err.append(msg('err', ex.message === 'unauthorized' ? 'invalid credentials' : ex.message));
    }
  });
  loginShell('🔭 NightWatcher2', 'Dark Sky Network · sky quality monitoring', null, f,
    el('div', { class: 'muted', style: 'text-align:center;margin-top:.9rem;font-size:.82rem' }, 'Default login: admin / admin'));
  user.focus();
}

function showChangePassword(forced) {
  const err = el('div');
  const f = el('form', { class: 'form' });
  const cur = el('input', { type: 'password', placeholder: 'current password' });
  const nw = el('input', { type: 'password', placeholder: 'new password' });
  f.append(el('label', {}, 'Current password', cur), el('label', {}, 'New password', nw), err,
    el('button', { class: 'btn', type: 'submit' }, 'Change password'));
  f.addEventListener('submit', async e => {
    e.preventDefault();
    err.innerHTML = '';
    try { await api('POST', '/me/password', { current_password: cur.value, new_password: nw.value }); boot(); }
    catch (ex) { err.append(msg('err', ex.message)); }
  });
  loginShell('Change password', null, forced ? 'You must change the default password before continuing.' : null, f);
}

// ---- boot ------------------------------------------------------------------
async function boot() {
  try {
    const me = await api('GET', '/me', undefined, { noAuthRedirect: true });
    state.user = me;
    renderApp();
  } catch (_) {
    showLogin();
  }
}

boot();
