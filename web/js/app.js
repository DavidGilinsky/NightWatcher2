// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          web/js/app.js
// Purpose:       NightWatcher2 web UI: login flow + dashboard, sensor/weather
//                CRUD, readings query + graph, events, users, and database
//                maintenance, all against the /api/v1 API.
// Created:       2026-07-18
// Last Modified: 2026-07-19
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
'use strict';

const state = { user: null, view: 'dashboard' };
let main = null;
let editingSensor = null;
let editingWeather = null;
let editingExport = null;
let exportLogFor = null;

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

function drawWeatherGraph(container, readings) {
  const rs = [...readings].reverse();
  const xs = rs.map(r => Date.parse(r.ts_utc.replace(' ', 'T') + 'Z') / 1000);
  const temp = rs.map(r => r.temp_c);
  const hum = rs.map(r => r.humidity_pct);
  const opts = {
    width: Math.max(320, container.clientWidth - 20), height: 320,
    scales: { x: { time: true } },
    series: [
      {},
      { label: 'temp °C', stroke: '#ff8c42', width: 2, scale: 'temp' },
      { label: 'humidity %', stroke: '#4ea1ff', width: 1, scale: 'hum' },
    ],
    axes: [
      { stroke: '#8fa0c0', grid: { stroke: '#26324f' }, ticks: { stroke: '#26324f' } },
      { scale: 'temp', stroke: '#8fa0c0', grid: { stroke: '#26324f' }, ticks: { stroke: '#26324f' } },
      { scale: 'hum', side: 1, stroke: '#8fa0c0', grid: { show: false } },
    ],
  };
  container.innerHTML = '';
  // eslint-disable-next-line no-undef
  new uPlot(opts, [xs, temp, hum], container);
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
  let stations = [];
  try { stations = await api('GET', '/weather-stations'); } catch (_) { /* weather is optional */ }
  if (!sensors.length && !stations.length) {
    frag.append(el('p', { class: 'muted' }, 'Nothing registered yet — add a sensor or weather station.'));
    return frag;
  }
  if (sensors.length) {
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
  }
  if (stations.length) {
    frag.append(el('h3', { style: 'margin-top:1.2rem' }, 'Weather'));
    const wgrid = el('div', { class: 'grid' });
    for (const w of stations) {
      let latest = null;
      try { const r = await api('GET', `/weather-stations/${encodeURIComponent(w.id)}/readings?limit=1`); latest = r[0]; } catch (_) { /* ignore */ }
      wgrid.append(el('div', { class: 'card' },
        el('h3', {}, w.name || w.id, el('span', { class: 'pill ' + (w.status || '') }, w.status || '')),
        el('div', { class: 'muted', style: 'margin-bottom:.5rem' }, (PROVIDERS[w.provider] || w.provider || '') + (w.site ? ' · ' + w.site : '')),
        latest
          ? el('div', {},
            el('div', { class: 'big' }, fmtNum(latest.temp_c), el('span', { class: 'unit' }, ' °C')),
            el('div', { class: 'row', style: 'margin-top:.35rem' },
              latest.humidity_pct != null ? el('span', { class: 'muted' }, fmtNum(latest.humidity_pct) + ' % RH') : null,
              latest.wind_speed_ms != null ? el('span', { class: 'muted' }, fmtNum(latest.wind_speed_ms) + ' m/s') : null,
              el('span', { class: 'muted' }, latest.ts_utc + ' UTC')))
          : el('div', { class: 'muted' }, 'no readings yet'),
        isAdmin()
          ? el('div', { style: 'margin-top:.7rem' },
            el('button', {
              class: 'btn sm', onclick: async () => {
                try { await api('POST', `/weather-stations/${encodeURIComponent(w.id)}/poll`); renderView(); }
                catch (e) { alert(e.message); }
              }
            }, 'Poll now'))
          : null));
    }
    frag.append(wgrid);
  }
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

const PROVIDERS = { ambientweather: 'Ambient Weather Network', wunderground: 'Weather Underground' };

function weatherForm(w) {
  const editing = !!w;
  const f = el('form', { class: 'form card', style: 'margin-bottom:1rem' });
  f.append(el('h3', {}, editing ? 'Edit ' + w.id : 'Add weather station'));
  const g = el('div', { class: 'form-grid' });
  if (!editing) g.append(field('ID *', 'id', ''));
  g.append(field('Name', 'name', editing ? w.name : ''));
  g.append(field('Site', 'site', editing ? w.site : ''));
  g.append(field('Model', 'model', editing ? w.model : 'Ambient Weather WS-2000'));
  const providerSel = el('select', { name: 'provider' },
    ...Object.entries(PROVIDERS).map(([v, label]) =>
      el('option', { value: v, selected: editing && w.provider === v ? 'selected' : null }, label)));
  g.append(el('label', {}, 'Provider', providerSel));
  g.append(field('Latitude', 'latitude', editing ? w.latitude : '', 'number'));
  g.append(field('Longitude', 'longitude', editing ? w.longitude : '', 'number'));
  g.append(field('Elevation (m)', 'elevation_m', editing ? w.elevation_m : '', 'number'));
  g.append(field('Timezone', 'timezone', editing ? w.timezone : ''));
  g.append(field('Poll interval (s)', 'poll_interval_s', editing ? w.poll_interval_s : 300, 'number', '1'));
  const statusSel = el('select', { name: 'status' },
    ...['active', 'inactive', 'retired'].map(o => el('option', { value: o, selected: editing && w.status === o ? 'selected' : null }, o)));
  g.append(el('label', {}, 'Status', statusSel));
  f.append(g);

  // Provider-specific credentials (secrets show as *** when already set).
  f.append(el('h3', { style: 'margin-top:.6rem' }, 'Provider settings'));
  const cfgWrap = el('div', { class: 'form-grid' });
  const cfg = (editing && w.config && typeof w.config === 'object') ? w.config : {};
  const renderCfg = () => {
    cfgWrap.innerHTML = '';
    if (providerSel.value === 'ambientweather') {
      cfgWrap.append(field('Application key', 'cfg_applicationKey', cfg.applicationKey || ''));
      cfgWrap.append(field('API key', 'cfg_apiKey', cfg.apiKey || ''));
      cfgWrap.append(field('Station MAC (optional)', 'cfg_macAddress', cfg.macAddress || ''));
    } else {
      cfgWrap.append(field('Station ID', 'cfg_stationId', cfg.stationId || ''));
      cfgWrap.append(field('API key', 'cfg_apiKey', cfg.apiKey || ''));
    }
  };
  providerSel.addEventListener('change', renderCfg);
  renderCfg();
  f.append(cfgWrap);

  const err = el('div'); f.append(err);
  f.append(el('div', { class: 'row', style: 'margin-top:.6rem' },
    el('button', { class: 'btn', type: 'submit' }, editing ? 'Save' : 'Add'),
    editing ? el('button', { class: 'btn ghost', type: 'button', onclick: () => { editingWeather = null; renderView(); } }, 'Cancel') : null));
  f.addEventListener('submit', async e => {
    e.preventDefault();
    const fd = new FormData(f);
    const body = {};
    const config = {};
    for (const [k, v] of fd.entries()) {
      if (k.startsWith('cfg_')) { if (v !== '') config[k.slice(4)] = v; continue; }
      if (v === '') continue;
      body[k] = ['latitude', 'longitude', 'elevation_m', 'poll_interval_s'].includes(k) ? Number(v) : v;
    }
    body.config = config;
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
    { label: 'Provider', render: w => PROVIDERS[w.provider] || w.provider || '—' },
    { label: 'Interval', render: w => w.poll_interval_s + 's' },
    { label: 'Status', render: w => el('span', { class: 'pill ' + (w.status || '') }, w.status) },
  ];
  if (isAdmin()) cols.push({
    label: '', render: w => el('div', { class: 'row' },
      el('button', {
        class: 'btn sm', onclick: async () => {
          try {
            const r = await api('POST', `/weather-stations/${encodeURIComponent(w.id)}/poll`);
            alert('Fetched: ' + (r.temp_c != null ? r.temp_c.toFixed(1) + ' °C' : 'no temp') + ', ' +
                  (r.humidity_pct != null ? r.humidity_pct + '% RH' : 'no humidity'));
          } catch (e) { alert('Poll failed: ' + e.message); }
        }
      }, 'Poll'),
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
  let stations = [];
  try { stations = await api('GET', '/weather-stations'); } catch (_) { /* weather is optional */ }
  if (!sensors.length && !stations.length) { frag.append(el('p', { class: 'muted' }, 'No sensors or weather stations yet.')); return frag; }

  const sel = el('select', { style: 'max-width:260px' });
  if (sensors.length) {
    const og = el('optgroup', { label: 'Sensors' });
    for (const s of sensors) og.append(el('option', { value: 'sensor:' + s.id }, s.name ? `${s.name} (${s.id})` : s.id));
    sel.append(og);
  }
  if (stations.length) {
    const og = el('optgroup', { label: 'Weather stations' });
    for (const w of stations) og.append(el('option', { value: 'weather:' + w.id }, w.name ? `${w.name} (${w.id})` : w.id));
    sel.append(og);
  }
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
  const tblWrap = el('div', { class: 'tablebox', style: 'margin-top:1rem' });

  const toUtc = ms => new Date(ms).toISOString().slice(0, 19).replace('T', ' ');
  const rangeParams = () => {
    let from = '', to = '';
    const days = { day: 1, week: 7, month: 30, year: 365 };
    if (rangeSel.value === 'custom') {
      if (fromInput.value) from = fromInput.value + ' 00:00:00';
      if (toInput.value) to = toInput.value + ' 23:59:59';
    } else {
      from = toUtc(Date.now() - days[rangeSel.value] * 86400000);
    }
    return { from, to };
  };
  const load = async () => {
    const { from, to } = rangeParams();
    const p = new URLSearchParams({ limit: '50000' });
    if (from) p.set('from', from);
    if (to) p.set('to', to);
    const idx = sel.value.indexOf(':');
    const kind = sel.value.slice(0, idx);
    const id = sel.value.slice(idx + 1);
    const path = kind === 'weather'
      ? `/weather-stations/${encodeURIComponent(id)}/readings`
      : `/sensors/${encodeURIComponent(id)}/readings`;
    try {
      const rs = await api('GET', `${path}?${p}`);
      caption.textContent = `${rs.length} reading(s)` + (from ? `  ·  from ${from} UTC` : '') + (to ? `  to ${to} UTC` : '');
      if (!rs.length) { chart.innerHTML = ''; chart.append(el('p', { class: 'muted' }, 'No readings in this range.')); tblWrap.innerHTML = ''; return; }
      let cols;
      if (kind === 'weather') {
        drawWeatherGraph(chart, rs);
        cols = [
          { label: 'Time (UTC)', render: r => r.ts_utc },
          { label: 'temp °C', render: r => fmtNum(r.temp_c) },
          { label: 'humidity %', render: r => fmtNum(r.humidity_pct) },
          { label: 'wind m/s', render: r => fmtNum(r.wind_speed_ms) },
          { label: 'pressure hPa', render: r => fmtNum(r.pressure_hpa) },
          { label: 'rain mm/h', render: r => fmtNum(r.rain_rate_mmh) },
        ];
      } else {
        drawGraph(chart, rs);
        cols = [
          { label: 'Time (UTC)', render: r => r.ts_utc },
          { label: 'mag/arcsec²', render: r => fmtMag(r.mag_arcsec2) },
          { label: 'temp °C', render: r => fmtNum(r.temp_c) },
          { label: 'freq Hz', render: r => r.freq_hz },
          { label: 'quality', render: r => el('span', { class: 'pill ' + (r.quality || '') }, r.quality) },
          { label: 'source', render: r => r.source },
        ];
      }
      tblWrap.innerHTML = ''; tblWrap.append(table(cols, rs));
    } catch (e) { chart.innerHTML = ''; chart.append(msg('err', e.message)); }
  };

  // Download a DSN community-format .dat for the selected sensor + range.
  const downloadDsn = () => {
    const idx = sel.value.indexOf(':');
    if (sel.value.slice(0, idx) !== 'sensor') return;
    const id = sel.value.slice(idx + 1);
    const { from, to } = rangeParams();
    const p = new URLSearchParams();
    if (from) p.set('from', from);
    if (to) p.set('to', to);
    const a = el('a', { href: `/api/v1/sensors/${encodeURIComponent(id)}/dsn?${p}`, download: '' });
    document.body.append(a); a.click(); a.remove();
  };
  const dsnBtn = el('button', { class: 'btn ghost', onclick: downloadDsn }, 'Download DSN file');
  const updateDsnBtn = () => { dsnBtn.style.display = sel.value.startsWith('sensor:') ? '' : 'none'; };

  rangeSel.addEventListener('change', () => {
    customWrap.style.display = rangeSel.value === 'custom' ? '' : 'none';
    if (rangeSel.value !== 'custom') load();
  });
  sel.addEventListener('change', () => { updateDsnBtn(); load(); });

  frag.append(el('div', { class: 'toolbar' },
    el('span', { class: 'muted' }, 'Source'), sel,
    el('span', { class: 'muted' }, 'Range'), rangeSel,
    customWrap,
    el('button', { class: 'btn', onclick: load }, 'Load'),
    dsnBtn));
  updateDsnBtn();
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

const EXPORT_TARGETS = { dsn: 'Dark Sky Network' };

function exportForm(x, sensors) {
  const editing = !!x;
  const cfg = (editing && x.config && typeof x.config === 'object') ? x.config : {};
  const auth = (cfg.auth && typeof cfg.auth === 'object') ? cfg.auth : {};
  const f = el('form', { class: 'form card', style: 'margin-bottom:1rem' });
  f.append(el('h3', {}, editing ? 'Edit ' + x.id : 'Add export target'));
  const g = el('div', { class: 'form-grid' });
  if (!editing) g.append(field('ID *', 'id', ''));
  const sensorSel = el('select', { name: 'sensor_id' },
    ...sensors.map(s => el('option', { value: s.id, selected: editing && x.sensor_id === s.id ? 'selected' : null }, s.name ? `${s.name} (${s.id})` : s.id)));
  g.append(el('label', {}, 'Sensor', sensorSel));
  g.append(field('Name', 'name', editing ? x.name : ''));
  const targetSel = el('select', { name: 'target' },
    ...Object.entries(EXPORT_TARGETS).map(([v, l]) => el('option', { value: v, selected: editing && x.target === v ? 'selected' : null }, l)));
  g.append(el('label', {}, 'Target', targetSel));
  const schedSel = el('select', { name: 'schedule' },
    ...['nightly', 'manual', 'interval'].map(o => el('option', { value: o, selected: editing && x.schedule === o ? 'selected' : null }, o)));
  g.append(el('label', {}, 'Schedule', schedSel));
  const timeField = field('Nightly time (local HH:MM)', 'schedule_time', editing ? (x.schedule_time || '06:00') : '06:00');
  const intervalField = field('Interval (s)', 'interval_s', editing && x.interval_s ? x.interval_s : 3600, 'number', '1');
  g.append(timeField); g.append(intervalField);
  const statusSel = el('select', { name: 'status' },
    ...['active', 'inactive', 'retired'].map(o => el('option', { value: o, selected: editing && x.status === o ? 'selected' : null }, o)));
  g.append(el('label', {}, 'Status', statusSel));
  f.append(g);
  const toggleSched = () => {
    timeField.style.display = schedSel.value === 'nightly' ? '' : 'none';
    intervalField.style.display = schedSel.value === 'interval' ? '' : 'none';
  };
  schedSel.addEventListener('change', toggleSched); toggleSched();

  f.append(el('h3', { style: 'margin-top:.6rem' }, 'DSN settings'));
  const dg = el('div', { class: 'form-grid' });
  dg.append(field('Site ID', 'cfg_site_id', cfg.site_id || ''));
  dg.append(field('Supplier (in file name)', 'cfg_supplier', cfg.supplier || ''));
  dg.append(field('Drive folder ID', 'cfg_drive_folder_id', cfg.drive_folder_id || ''));
  f.append(dg);

  f.append(el('h3', { style: 'margin-top:.6rem' }, 'Google Drive auth'));
  const authModeSel = el('select', {},
    el('option', { value: 'oauth', selected: auth.mode === 'service_account' ? null : 'selected' }, 'OAuth (refresh token)'),
    el('option', { value: 'service_account', selected: auth.mode === 'service_account' ? 'selected' : null }, 'Service account'));
  f.append(el('label', {}, 'Auth mode', authModeSel));
  const authWrap = el('div', { class: 'form-grid' });
  f.append(authWrap);
  const renderAuth = () => {
    authWrap.innerHTML = '';
    if (authModeSel.value === 'service_account') {
      authWrap.append(el('div', { class: 'muted', style: 'grid-column:1/-1;font-size:.8rem' },
        auth.client_email ? ('Key set for ' + auth.client_email + '. Paste a new JSON key to replace it, or leave blank to keep it.') : 'Paste the service-account JSON key. Share the Drive folder with its client_email.'));
      authWrap.append(el('label', { style: 'grid-column:1/-1' }, 'Service account JSON',
        el('textarea', { name: 'sa_json', rows: '5', placeholder: '{ "client_email": "...", "private_key": "..." }', style: 'width:100%' })));
    } else {
      authWrap.append(field('Client ID', 'oauth_client_id', auth.client_id || ''));
      authWrap.append(field('Client secret', 'oauth_client_secret', auth.client_secret || ''));
      authWrap.append(field('Refresh token', 'oauth_refresh_token', auth.refresh_token || ''));
      authWrap.append(el('div', { class: 'muted', style: 'grid-column:1/-1;font-size:.8rem' }, 'Run `nwexport-auth` to mint a refresh token from any browser.'));
    }
  };
  authModeSel.addEventListener('change', renderAuth); renderAuth();

  const err = el('div'); f.append(err);
  f.append(el('div', { class: 'row', style: 'margin-top:.6rem' },
    el('button', { class: 'btn', type: 'submit' }, editing ? 'Save' : 'Add'),
    editing ? el('button', { class: 'btn ghost', type: 'button', onclick: () => { editingExport = null; renderView(); } }, 'Cancel') : null));

  f.addEventListener('submit', async e => {
    e.preventDefault();
    const fd = new FormData(f);
    const body = {};
    for (const [k, v] of fd.entries()) {
      if (k.startsWith('cfg_') || k.startsWith('oauth_') || k === 'sa_json') continue;
      if (v === '') continue;
      body[k] = (k === 'interval_s') ? Number(v) : v;
    }
    const config = {};
    if (fd.get('cfg_site_id')) config.site_id = fd.get('cfg_site_id');
    if (fd.get('cfg_supplier')) config.supplier = fd.get('cfg_supplier');
    if (fd.get('cfg_drive_folder_id')) config.drive_folder_id = fd.get('cfg_drive_folder_id');
    if (authModeSel.value === 'oauth') {
      const a = { mode: 'oauth' };
      if (fd.get('oauth_client_id')) a.client_id = fd.get('oauth_client_id');
      if (fd.get('oauth_client_secret')) a.client_secret = fd.get('oauth_client_secret');
      if (fd.get('oauth_refresh_token')) a.refresh_token = fd.get('oauth_refresh_token');
      config.auth = a;
    } else {
      const sj = (fd.get('sa_json') || '').trim();
      if (sj) {
        let sa;
        try { sa = JSON.parse(sj); } catch (_) { err.innerHTML = ''; err.append(msg('err', 'Service account JSON is invalid')); return; }
        config.auth = { mode: 'service_account', client_email: sa.client_email, private_key: sa.private_key, token_uri: sa.token_uri || 'https://oauth2.googleapis.com/token' };
      }
    }
    body.config = config;
    try {
      if (editing) { delete body.id; await api('PATCH', `/export-targets/${encodeURIComponent(x.id)}`, body); }
      else { if (!body.id) { err.innerHTML = ''; err.append(msg('err', 'ID is required')); return; } await api('POST', '/export-targets', body); }
      editingExport = null; renderView();
    } catch (ex) { err.innerHTML = ''; err.append(msg('err', ex.message)); }
  });
  return f;
}

async function viewExports() {
  const frag = document.createDocumentFragment();
  frag.append(el('h2', {}, 'DSN Export'));
  const sensors = await api('GET', '/sensors');
  if (!sensors.length) {
    frag.append(el('p', { class: 'muted' }, 'Add a sensor first — exports are built from a sensor’s readings.'));
    return frag;
  }
  if (isAdmin()) frag.append(exportForm(editingExport, sensors));
  const rows = await api('GET', '/export-targets');
  const cols = [
    { label: 'ID', render: x => x.id },
    { label: 'Sensor', render: x => x.sensor_id },
    { label: 'Target', render: x => EXPORT_TARGETS[x.target] || x.target },
    { label: 'Schedule', render: x => x.schedule === 'nightly' ? `nightly ${x.schedule_time || ''}` : (x.schedule === 'interval' ? `every ${x.interval_s || '?'}s` : 'manual') },
    { label: 'Last export', render: x => x.last_export_ts || '—' },
    { label: 'Status', render: x => el('span', { class: 'pill ' + (x.status || '') }, x.status) },
  ];
  if (isAdmin()) cols.push({
    label: '', render: x => el('div', { class: 'row' },
      el('button', {
        class: 'btn sm', onclick: async () => {
          try {
            const r = await api('POST', `/export-targets/${encodeURIComponent(x.id)}/run`);
            alert(`Exported ${r.rows} reading(s) in ${r.files} file(s)` +
                  (r.file_names && r.file_names.length ? ':\n' + r.file_names.join('\n') : '.'));
            renderView();
          } catch (e) { alert('Run failed: ' + e.message); }
        }
      }, 'Run now'),
      el('button', { class: 'btn ghost sm', onclick: () => { exportLogFor = (exportLogFor === x.id ? null : x.id); renderView(); } }, 'Log'),
      el('button', { class: 'btn ghost sm', onclick: () => { editingExport = x; renderView(); } }, 'Edit'),
      el('button', {
        class: 'btn danger sm', onclick: async () => {
          if (confirm('Delete export target ' + x.id + '?')) {
            try { await api('DELETE', '/export-targets/' + encodeURIComponent(x.id)); renderView(); } catch (e) { alert(e.message); }
          }
        }
      }, 'Delete'))
  });
  frag.append(rows.length ? el('div', { class: 'scroll' }, table(cols, rows)) : el('p', { class: 'muted' }, 'No export targets yet.'));

  if (exportLogFor) {
    frag.append(el('h3', { style: 'margin-top:1rem' }, 'Export log — ' + exportLogFor));
    try {
      const log = await api('GET', `/export-targets/${encodeURIComponent(exportLogFor)}/log`);
      const lcols = [
        { label: 'When (UTC)', render: l => l.ts_utc },
        { label: 'Window', render: l => (l.from_ts || '') + ' → ' + (l.to_ts || '') },
        { label: 'Rows', render: l => l.row_count },
        { label: 'File', render: l => l.file_name || '—' },
        { label: 'Status', render: l => el('span', { class: 'pill ' + (l.status === 'error' ? 'suspect' : (l.status === 'ok' ? 'ok' : '')) }, l.status) },
        { label: 'Detail', render: l => l.detail || '' },
      ];
      frag.append(log.length ? el('div', { class: 'scroll' }, table(lcols, log)) : el('p', { class: 'muted' }, 'Nothing exported yet.'));
    } catch (e) { frag.append(msg('err', e.message)); }
  }
  return frag;
}

async function viewServer() {
  const frag = document.createDocumentFragment();
  frag.append(el('h2', {}, 'Web server'));
  let s;
  try { s = await api('GET', '/settings'); }
  catch (e) { frag.append(msg('err', e.message)); return frag; }
  const cfg = s.configured, run = s.running;

  frag.append(el('p', { class: 'muted', style: 'margin-bottom:.6rem' },
    `Currently listening on ${run.bind}:${run.port}.`));
  if (s.restart_required) {
    frag.append(msg('warn', `Pending change: configured ${cfg.bind}:${cfg.port}, still running on ${run.bind}:${run.port}. Restart the daemon to apply.`));
  }

  const f = el('form', { class: 'form card', style: 'margin-bottom:1rem' });
  f.append(el('h3', {}, 'Listen address'));
  const g = el('div', { class: 'form-grid' });
  const bindInput = el('input', { name: 'bind', type: 'text', value: cfg.bind || '' });
  g.append(el('label', {}, 'Bind address', bindInput));
  g.append(field('Port', 'port', cfg.port, 'number', '1'));
  f.append(g);
  f.append(el('div', { class: 'muted', style: 'font-size:.82rem;margin-top:.3rem' },
    '127.0.0.1 = localhost only  ·  0.0.0.0 = all interfaces  ·  or a specific interface IP.'));

  const warn = el('div', { style: 'margin-top:.5rem' });
  const updateWarn = () => {
    const b = bindInput.value.trim();
    const local = b === '' || b === '127.0.0.1' || b === 'localhost' || b === '::1';
    warn.innerHTML = '';
    if (!local) warn.append(msg('warn',
      '⚠ Exposing the server beyond localhost makes the UI and API reachable on your network. Read endpoints are unauthenticated, so anyone who can reach this host can view your sensor data and configuration.'));
  };
  bindInput.addEventListener('input', updateWarn); updateWarn();
  f.append(warn);

  const out = el('div'); f.append(out);
  f.append(el('div', { class: 'row', style: 'margin-top:.6rem' },
    el('button', { class: 'btn', type: 'submit' }, 'Save')));
  f.addEventListener('submit', async e => {
    e.preventDefault();
    out.innerHTML = '';
    try {
      const r = await api('PUT', '/settings', {
        bind: bindInput.value.trim(),
        port: Number(f.querySelector('input[name="port"]').value),
      });
      out.append(r.restart_required
        ? msg('warn', `Saved. Restart the daemon to start listening on ${r.configured.bind}:${r.configured.port}.`)
        : msg('ok', 'Saved (unchanged from the running configuration).'));
    } catch (ex) { out.append(msg('err', ex.message)); }
  });
  frag.append(f);
  return frag;
}

const VIEWS = {
  dashboard: { label: 'Dashboard', render: viewDashboard },
  query: { label: 'Query & graph', render: viewQuery },
  sensors: { label: 'Sensors', render: viewSensors },
  weather: { label: 'Weather', render: viewWeather },
  exports: { label: 'DSN Export', admin: true, render: viewExports },
  events: { label: 'Events', render: viewEvents },
  users: { label: 'Users', admin: true, render: viewUsers },
  database: { label: 'Database', admin: true, render: viewDatabase },
  server: { label: 'Server', admin: true, render: viewServer },
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
