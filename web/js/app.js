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
const NW_DOW = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
function fmtSchedule(x) {
  const t = x.schedule_time || '';
  if (x.schedule === 'nightly') return `nightly ${t}`.trim();
  if (x.schedule === 'weekly') return `weekly ${NW_DOW[x.schedule_day] || 'Sun'} ${t}`.trim();
  if (x.schedule === 'monthly') return `monthly day ${x.schedule_day || 1} ${t}`.trim();
  if (x.schedule === 'interval') return `every ${x.interval_s || '?'}s`;
  return 'manual';
}

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

// Moon-phase glyph for an instant, from SunCalc's illuminated-phase value
// (0=new, .25=first quarter, .5=full, .75=last quarter). Northern-hemisphere
// orientation, which suits our sites.
function moonGlyph(date) {
  // eslint-disable-next-line no-undef
  const phase = SunCalc.getMoonIllumination(date).phase;
  return ['🌑', '🌒', '🌓', '🌔', '🌕', '🌖', '🌗', '🌘'][Math.round(phase * 8) % 8];
}

function drawGraph(container, readings, sensor, opts) {
  const rs = [...readings].reverse();  // API returns newest-first
  const xs = rs.map(r => Date.parse(r.ts_utc.replace(' ', 'T') + 'Z') / 1000);
  const mag = rs.map(r => r.mag_arcsec2);
  const temp = rs.map(r => r.temp_c);

  const data = [xs, mag, temp];
  const series = [
    {},
    { label: 'mag/arcsec²', stroke: '#4ea1ff', width: 2, scale: 'mag' },
    { label: 'sensor °C', stroke: '#ff8c42', width: 1, scale: 'temp' },
  ];
  const scales = { x: { time: true } };
  const axes = [
    { stroke: '#8fa0c0', grid: { stroke: '#26324f' }, ticks: { stroke: '#26324f' } },
    { scale: 'mag', stroke: '#8fa0c0', grid: { stroke: '#26324f' }, ticks: { stroke: '#26324f' } },
    { scale: 'temp', side: 1, stroke: '#8fa0c0', grid: { show: false } },
  ];
  const hooks = {};

  // Ambient (outdoor) temperature from a co-located weather station, interpolated
  // onto the reading timestamps and drawn on the same °C axis as the sensor temp.
  // opts.weather holds that station's readings ({ts_utc, temp_c}); opt-in via opts.ambient.
  const wxRows = (opts && opts.weather) || [];
  if (wxRows.length && (!opts || opts.ambient !== false)) {
    const wx = [];
    for (const w of wxRows) {
      const wt = Date.parse(String(w.ts_utc).replace(' ', 'T') + 'Z') / 1000;
      if (!Number.isNaN(wt) && w.temp_c != null) wx.push([wt, Number(w.temp_c)]);
    }
    wx.sort((a, b) => a[0] - b[0]);
    if (wx.length) {
      const MAXGAP = 3600;  // don't interpolate ambient across gaps > 1h
      const amb = new Array(xs.length);
      let p = 0;
      for (let i = 0; i < xs.length; i++) {
        const x = xs[i];
        while (p < wx.length - 1 && wx[p + 1][0] <= x) p++;
        const a = wx[p], b = wx[p + 1];
        if (x < wx[0][0]) amb[i] = null;
        else if (!b) amb[i] = (x - a[0] <= MAXGAP) ? a[1] : null;
        else if (b[0] - a[0] > MAXGAP) amb[i] = null;
        else amb[i] = a[1] + (x - a[0]) / (b[0] - a[0]) * (b[1] - a[1]);
      }
      data.push(amb);
      series.push({ label: 'ambient °C', stroke: '#39c0b0', width: 1, scale: 'temp', points: { show: false } });
    }
  }

  // Sun/Moon altitude are a pure function of time + observer location, so the
  // overlay just needs the sensor's coordinates and SunCalc. Each line is opt-in
  // via `opts` (default on) and skipped silently when coordinates are missing.
  const lat = sensor ? Number(sensor.latitude) : NaN;
  const lon = sensor ? Number(sensor.longitude) : NaN;
  const haveEphem = typeof SunCalc !== 'undefined' && sensor &&
    sensor.latitude != null && sensor.longitude != null &&
    Number.isFinite(lat) && Number.isFinite(lon);
  const wantSun = haveEphem && (!opts || opts.sun !== false);
  const wantMoon = haveEphem && (!opts || opts.moon !== false);
  if (wantSun || wantMoon) {
    const deg = 180 / Math.PI;
    scales.alt = {};  // auto-range in degrees
    axes.push({
      scale: 'alt', side: 1, stroke: '#8fa0c0', grid: { show: false },
      values: (u, vals) => vals.map(v => v + '°'),
    });
    if (wantSun) {
      // eslint-disable-next-line no-undef
      const sunAlt = xs.map(x => SunCalc.getPosition(new Date(x * 1000), lat, lon).altitude * deg);
      data.push(sunAlt);
      series.push({ label: 'sun alt°', stroke: '#ffcf5c', width: 1, dash: [5, 3], scale: 'alt', points: { show: false } });
    }
    if (wantMoon) {
      // eslint-disable-next-line no-undef
      const moonAlt = xs.map(x => SunCalc.getMoonPosition(new Date(x * 1000), lat, lon).altitude * deg);
      data.push(moonAlt);
      series.push({ label: 'moon alt°', stroke: '#c8d2ea', width: 1, dash: [5, 3], scale: 'alt', points: { show: false } });
      const moonIdx = data.length - 1;  // moon altitude is the last series pushed
      // A moon-phase glyph riding on the moon line at its highest point.
      hooks.draw = [(u) => {
        const moon = u.data[moonIdx];
        let mi = -1, mv = -Infinity;
        for (let i = 0; i < moon.length; i++) {
          if (moon[i] != null && moon[i] > mv) { mv = moon[i]; mi = i; }
        }
        if (mi < 0) return;
        const dpr = u.pxRatio || 1;
        const px = u.valToPos(u.data[0][mi], 'x', true);
        const py = u.valToPos(mv, 'alt', true);
        const ctx = u.ctx;
        ctx.save();
        ctx.fillStyle = '#dfe6f5';
        ctx.font = (15 * dpr) + 'px sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'bottom';
        ctx.fillText(moonGlyph(new Date(u.data[0][mi] * 1000)), px, py - 3 * dpr);
        ctx.restore();
      }];
    }
  }

  const uplotOpts = {
    width: Math.max(320, container.clientWidth - 20), height: 320,
    scales, series, axes, hooks,
  };
  container.innerHTML = '';
  // eslint-disable-next-line no-undef
  new uPlot(uplotOpts, data, container);
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
          ? el('div', { class: 'row', style: 'margin-top:.7rem' },
            el('button', { class: 'btn ghost sm', type: 'button', onclick: () => sensorTest(s) }, 'Test'),
            s.status === 'active'
              ? el('button', {
                  class: 'btn sm', type: 'button', onclick: async () => {
                    try { await api('POST', `/sensors/${encodeURIComponent(s.id)}/poll`); renderView(); }
                    catch (e) { alert(e.message); }
                  }
                }, 'Poll now')
              : el('button', { class: 'btn sm', type: 'button', onclick: () => setSensorEnabled(s, true) }, 'Enable'))
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

// ---- lightweight modal overlay -------------------------------------------
function modal(title, bodyNode) {
  const overlay = el('div', { class: 'modal-overlay' });
  const onKey = e => { if (e.key === 'Escape') close(); };
  function close() { overlay.remove(); document.removeEventListener('keydown', onKey); }
  const card = el('div', { class: 'modal-card' },
    el('div', { class: 'modal-head' },
      el('h3', { style: 'margin:0' }, title),
      el('button', { class: 'btn ghost sm', type: 'button', onclick: close }, '✕')),
    bodyNode);
  overlay.append(card);
  overlay.addEventListener('click', e => { if (e.target === overlay) close(); });
  document.addEventListener('keydown', onKey);
  document.body.append(overlay);
  return { overlay, close };
}

// Render one self-test check as a labelled OK/FAIL row with its values.
function renderCheck(c) {
  const labels = { connect: 'Connectivity', identify: 'Identity (ix)', reading: 'Reading (rx)', calibration: 'Calibration (cx)' };
  let detail = c.detail || '';
  if (c.ok && c.name === 'identify') {
    detail = `serial ${c.serial}, protocol ${c.protocol}, feature ${c.feature}`;
    if (c.serial_match === false) detail += ' — ⚠ does not match the registered serial';
  } else if (c.ok && c.name === 'reading') {
    detail = `${fmtMag(c.mag_arcsec2)} mag/arcsec², ${fmtNum(c.temp_c)} °C, quality ${c.quality}`;
  } else if (c.ok && c.name === 'calibration') {
    detail = `light offset ${fmtMag(c.light_cal_offset)}, sensor offset ${fmtMag(c.sensor_offset)} mag/arcsec²`;
  }
  return el('div', { class: 'check' },
    el('div', { class: 'row', style: 'justify-content:space-between' },
      el('b', {}, labels[c.name] || c.name),
      el('span', { class: 'pill ' + (c.ok ? 'ok' : 'suspect') }, c.ok ? 'OK' : 'FAIL')),
    detail ? el('div', { class: 'muted', style: 'font-size:.85rem;margin-top:.15rem' }, detail) : null);
}

// Run the non-persisting self-test for a sensor and show results in a modal.
async function sensorTest(s) {
  const body = el('div');
  body.append(el('div', { class: 'muted', style: 'margin-bottom:.6rem' },
    `Live test of ${s.id} at ${s.address}. Nothing is written to the database.`));
  const status = el('div', {}, msg('warn', 'Running self-test…'));
  body.append(status);
  const results = el('div', { class: 'checks' });
  body.append(results);
  modal('Verify ' + s.id, body);
  try {
    const r = await api('POST', `/sensors/${encodeURIComponent(s.id)}/test`);
    status.innerHTML = '';
    status.append(msg(r.ok ? 'ok' : 'err', r.ok
      ? '✓ All checks passed — safe to enable database population.'
      : '✗ One or more checks failed (see below).'));
    for (const c of (r.checks || [])) results.append(renderCheck(c));
  } catch (e) {
    status.innerHTML = '';
    status.append(msg('err', 'Test could not run: ' + e.message));
  }
}

// Enable/disable database population for a sensor (daemon reloads immediately).
async function setSensorEnabled(s, enable) {
  try {
    await api('POST', `/sensors/${encodeURIComponent(s.id)}/${enable ? 'enable' : 'disable'}`);
    renderView();
  } catch (e) { alert(e.message); }
}

// Calibration panel: read cx + lock status, view/record history, and (admin)
// arm/disarm or manually write calibration values.
async function sensorCalibration(s) {
  const p = id => `/sensors/${encodeURIComponent(s.id)}/calibration${id}`;
  const body = el('div');
  body.append(el('div', { class: 'muted', style: 'margin-bottom:.5rem' }, `${s.transport} ${s.address}`));
  const curBox = el('div', { class: 'checks' });
  const status = el('div');
  const histBox = el('div', { style: 'margin-top:.6rem' });
  body.append(curBox);

  const numField = (label, name) => {
    const input = el('input', { name, type: 'number', step: 'any' });
    return { input, label: el('label', {}, label, input) };
  };
  const inputs = {
    light_offset: numField('Light offset (mag)', 'light_offset'),
    light_temp: numField('Light temp (°C)', 'light_temp'),
    dark_period: numField('Dark period (s)', 'dark_period'),
    dark_temp: numField('Dark temp (°C)', 'dark_temp'),
  };

  async function loadCurrent() {
    curBox.innerHTML = ''; curBox.append(msg('warn', 'Reading calibration from the device…'));
    try {
      const c = await api('GET', p(''));
      curBox.innerHTML = '';
      curBox.append(el('div', { class: 'check' },
        el('b', {}, 'Current calibration'),
        el('div', { class: 'muted', style: 'font-size:.85rem;margin-top:.15rem' },
          `light offset ${fmtMag(c.light_cal_offset)} mag · dark period ${fmtNum(c.dark_cal_period_s)} s · ` +
          `light temp ${fmtNum(c.temp_light_c)} °C · dark temp ${fmtNum(c.temp_dark_c)} °C · ` +
          `sensor offset ${fmtMag(c.sensor_offset)} mag`)));
      inputs.light_offset.input.value = c.light_cal_offset;
      inputs.light_temp.input.value = c.temp_light_c;
      inputs.dark_period.input.value = c.dark_cal_period_s;
      inputs.dark_temp.input.value = c.temp_dark_c;
    } catch (e) { curBox.innerHTML = ''; curBox.append(msg('err', e.message)); }
  }

  async function loadHistory() {
    try {
      const rows = await api('GET', p('/history?limit=25'));
      histBox.innerHTML = '';
      histBox.append(el('h3', { style: 'font-size:1rem;margin:.4rem 0' }, 'History'));
      if (!rows.length) { histBox.append(el('div', { class: 'muted' }, 'No calibration history yet.')); return; }
      histBox.append(el('div', { class: 'scroll' }, table([
        { label: 'When (UTC)', render: r => r.ts_utc },
        { label: 'Type', render: r => r.event_type },
        { label: 'Offset', render: r => r.light_cal_offset == null ? '—' : fmtMag(r.light_cal_offset) },
        { label: 'Period', render: r => r.dark_cal_period_s == null ? '—' : fmtNum(r.dark_cal_period_s) + 's' },
        { label: 'By', render: r => r.changed_by || '—' },
        { label: 'Note', render: r => r.note || '—' },
      ], rows)));
    } catch (e) { histBox.innerHTML = ''; histBox.append(msg('err', e.message)); }
  }

  body.append(status);

  if (isAdmin()) {
    const calAction = async (path, payload, describe) => {
      status.innerHTML = '';
      try {
        const r = await api('POST', p(path), payload);
        if (r && r.mode !== undefined) {
          status.append(msg('ok', `${describe}: ${r.armed ? 'armed' : 'disarmed'}, unlock switch is ` +
            `${r.locked ? 'LOCKED' : 'UNLOCKED'}.` + (r.armed ? ' Flip the unlock switch to calibrate.' : '')));
        } else { status.append(msg('ok', `${describe} done.`)); }
        loadHistory();
      } catch (e) { status.append(msg('err', e.message)); }
    };
    body.append(el('div', { class: 'row', style: 'margin:.5rem 0' },
      el('button', { class: 'btn ghost sm', type: 'button', onclick: () => calAction('/record', undefined, 'Snapshot recorded').then(loadHistory) }, 'Record snapshot'),
      el('button', { class: 'btn ghost sm', type: 'button', onclick: () => calAction('/arm', { mode: 'light' }, 'Arm light') }, 'Arm light'),
      el('button', { class: 'btn ghost sm', type: 'button', onclick: () => calAction('/arm', { mode: 'dark' }, 'Arm dark') }, 'Arm dark'),
      el('button', { class: 'btn ghost sm', type: 'button', onclick: () => calAction('/disarm', {}, 'Disarm') }, 'Disarm')));

    body.append(el('div', { class: 'card', style: 'margin-top:.5rem' },
      el('b', {}, 'Manually set calibration'),
      msg('warn', '⚠ Writes directly to the unit’s calibration EEPROM. Use only to restore or copy a known calibration.'),
      el('div', { class: 'form-grid' }, inputs.light_offset.label, inputs.light_temp.label, inputs.dark_period.label, inputs.dark_temp.label),
      el('button', {
        class: 'btn', type: 'button', style: 'margin-top:.5rem', onclick: async () => {
          status.innerHTML = '';
          const payload = {};
          for (const k of Object.keys(inputs)) { const v = inputs[k].input.value; if (v !== '') payload[k] = Number(v); }
          if (!Object.keys(payload).length) { status.append(msg('err', 'No values to set.')); return; }
          if (!confirm(`Write these calibration values to ${s.id}? This modifies the device.`)) return;
          try { await api('POST', p('/set'), payload); status.append(msg('ok', 'Calibration written to the device.')); loadCurrent(); loadHistory(); }
          catch (e) { status.append(msg('err', e.message)); }
        }
      }, 'Write to device')));
  }

  body.append(histBox);
  modal('Calibration — ' + s.id, body);
  loadCurrent();
  loadHistory();
}

function sensorForm(s) {
  const editing = !!s;
  const f = el('form', { class: 'form card', style: 'margin-bottom:1rem' });
  f.append(el('h3', {}, editing ? 'Edit ' + s.id : 'Add sensor'));
  const g = el('div', { class: 'form-grid' });
  if (!editing) g.append(field('ID *', 'id', ''));
  const curTransport = editing ? (s.transport || 'tcp') : 'tcp';
  const transportSel = el('select', { name: 'transport' },
    el('option', { value: 'tcp', selected: curTransport === 'tcp' ? 'selected' : null }, 'SQM-LE (Ethernet)'),
    el('option', { value: 'serial', selected: curTransport === 'serial' ? 'selected' : null }, 'SQM-LU (USB)'));
  g.append(el('label', {}, 'Transport', transportSel));
  const addrInput = el('input', { name: 'address', type: 'text', value: editing ? (s.address || '') : '' });
  const addrLabelText = document.createTextNode('');
  g.append(el('label', {}, addrLabelText, addrInput));
  const syncAddr = () => {
    const serial = transportSel.value === 'serial';
    addrLabelText.textContent = serial ? 'Serial device' : 'Host:port';
    addrInput.placeholder = serial ? '/dev/ttyUSB0 (or /dev/serial/by-id/…)' : 'host:10001';
  };
  transportSel.addEventListener('change', syncAddr); syncAddr();
  g.append(field('Name', 'name', editing ? s.name : ''));
  g.append(field('Site', 'site', editing ? s.site : ''));
  g.append(field('Latitude', 'latitude', editing ? s.latitude : '', 'number'));
  g.append(field('Longitude', 'longitude', editing ? s.longitude : '', 'number'));
  g.append(field('Elevation (m)', 'elevation_m', editing ? s.elevation_m : '', 'number'));
  g.append(field('Timezone', 'timezone', editing ? s.timezone : ''));
  g.append(field('Poll interval (s)', 'poll_interval_s', editing ? s.poll_interval_s : 300, 'number', '1'));
  g.append(field('Model', 'model', editing ? s.model : ''));
  const defaultStatus = editing ? s.status : 'inactive';
  const statusSel = el('select', { name: 'status' },
    ...['active', 'inactive', 'retired'].map(o => el('option', { value: o, selected: o === defaultStatus ? 'selected' : null }, o)));
  g.append(el('label', {}, 'Status', statusSel));
  f.append(g);
  // USB scan: find SQM-LU units on the serial bus and fill the form from a pick.
  const scanOut = el('div', { style: 'margin-top:.4rem' });
  f.append(el('div', { class: 'row', style: 'margin-top:.3rem' },
    el('button', {
      class: 'btn ghost sm', type: 'button', onclick: async () => {
        scanOut.innerHTML = ''; scanOut.append(msg('warn', 'Scanning the USB bus…'));
        try {
          const found = await api('GET', '/discover/usb');
          scanOut.innerHTML = '';
          if (!found.length) {
            scanOut.append(msg('warn', 'No SQM-LU found. Check the USB cable and that the daemon user is in the “dialout” group.'));
            return;
          }
          for (const d of found) {
            scanOut.append(el('div', { class: 'row', style: 'gap:.5rem;margin:.25rem 0' },
              el('button', {
                class: 'btn sm', type: 'button', onclick: () => {
                  transportSel.value = 'serial'; syncAddr(); addrInput.value = d.device;
                  const idInput = f.querySelector('input[name="id"]');
                  if (idInput && !idInput.value && d.serial) idInput.value = 'SQM-' + d.serial;
                }
              }, 'Use'),
              el('span', { class: 'muted', style: 'font-size:.85rem' }, `${d.device} — serial ${d.serial}, model ${d.model}`)));
          }
        } catch (e) { scanOut.innerHTML = ''; scanOut.append(msg('err', e.message)); }
      }
    }, 'Scan USB for SQM-LU')));
  f.append(scanOut);
  if (!editing) f.append(el('div', { class: 'muted', style: 'font-size:.82rem' },
    'New sensors start disabled — nothing is written to the database. Add it, click Test to verify, then Enable database population.'));
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
      el('button', { class: 'btn ghost sm', type: 'button', onclick: () => sensorTest(s) }, 'Test'),
      el('button', { class: 'btn ghost sm', type: 'button', onclick: () => sensorCalibration(s) }, 'Calibrate'),
      s.status === 'active'
        ? el('button', { class: 'btn ghost sm', type: 'button', onclick: () => setSensorEnabled(s, false) }, 'Disable')
        : el('button', { class: 'btn sm', type: 'button', onclick: () => setSensorEnabled(s, true) }, 'Enable'),
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

  // Overlay toggles — persisted, applied to sensor graphs only. Ambient temperature
  // comes from a co-located weather station (matched by site, else the sole station);
  // its checkbox appears only when such a station exists for the selected sensor.
  const overlayPref = {
    sun: localStorage.getItem('nw_overlay_sun') !== '0',
    moon: localStorage.getItem('nw_overlay_moon') !== '0',
    ambient: localStorage.getItem('nw_overlay_ambient') !== '0',
  };
  const coLocatedStation = (sensor) => {
    if (!sensor) return null;
    const bySite = sensor.site ? stations.filter(w => w.site && w.site === sensor.site) : [];
    if (bySite.length) return bySite[0];
    const active = stations.filter(w => w.status !== 'retired');
    return active.length === 1 ? active[0] : null;  // sole-station fallback
  };
  let lastSensorGraph = null;  // { readings, sensor, weather } backing the current sensor graph
  const redrawOverlay = () => {
    if (lastSensorGraph) {
      drawGraph(chart, lastSensorGraph.readings, lastSensorGraph.sensor, {
        sun: overlayPref.sun, moon: overlayPref.moon,
        ambient: overlayPref.ambient, weather: lastSensorGraph.weather,
      });
    }
  };
  const overlayChk = (key, label) => el('label', { style: 'display:inline-flex;align-items:center;gap:.3rem' },
    el('input', {
      type: 'checkbox', ...(overlayPref[key] ? { checked: 'checked' } : {}),
      onchange: (e) => {
        overlayPref[key] = e.target.checked;
        localStorage.setItem('nw_overlay_' + key, e.target.checked ? '1' : '0');
        redrawOverlay();
      },
    }), label);
  const ambientChkLabel = overlayChk('ambient', 'Ambient');
  ambientChkLabel.style.display = 'none';  // shown by load() when a co-located station exists
  const overlayWrap = el('span', { class: 'row', style: 'gap:.5rem' },
    el('span', { class: 'muted' }, 'Overlay'), overlayChk('sun', 'Sun'), overlayChk('moon', 'Moon'), ambientChkLabel);

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
      lastSensorGraph = null;
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
        const sensor = sensors.find(s => s.id === id);
        const station = coLocatedStation(sensor);
        ambientChkLabel.style.display = station ? '' : 'none';
        let weather = null;
        if (station) {
          try { weather = await api('GET', `/weather-stations/${encodeURIComponent(station.id)}/readings?${p}`); }
          catch (_) { weather = null; }
        }
        lastSensorGraph = { readings: rs, sensor, weather };
        drawGraph(chart, rs, sensor, {
          sun: overlayPref.sun, moon: overlayPref.moon,
          ambient: overlayPref.ambient, weather,
        });
        cols = [
          { label: 'Time (UTC)', render: r => r.ts_utc },
          { label: 'mag/arcsec²', render: r => fmtMag(r.mag_arcsec2) },
          { label: 'sensor °C', render: r => fmtNum(r.temp_c) },
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
  const updateDsnBtn = () => {
    const isSensor = sel.value.startsWith('sensor:');
    dsnBtn.style.display = isSensor ? '' : 'none';
    overlayWrap.style.display = isSensor ? '' : 'none';
  };

  rangeSel.addEventListener('change', () => {
    customWrap.style.display = rangeSel.value === 'custom' ? '' : 'none';
    if (rangeSel.value !== 'custom') load();
  });
  sel.addEventListener('change', () => { updateDsnBtn(); load(); });

  frag.append(el('div', { class: 'toolbar' },
    el('span', { class: 'muted' }, 'Source'), sel,
    el('span', { class: 'muted' }, 'Range'), rangeSel,
    customWrap,
    overlayWrap,
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
    ...['nightly', 'weekly', 'monthly', 'interval', 'manual'].map(o => el('option', { value: o, selected: editing && x.schedule === o ? 'selected' : null }, o)));
  g.append(el('label', {}, 'Schedule', schedSel));
  const timeField = field('Time (local HH:MM)', 'schedule_time', editing ? (x.schedule_time || '06:00') : '06:00');
  const intervalField = field('Interval (s)', 'interval_s', editing && x.interval_s ? x.interval_s : 3600, 'number', '1');
  const daySel = el('select', { name: 'schedule_day' });
  const dayLabel = el('span', {}, 'Day');
  const dayField = el('label', {}, dayLabel, daySel);
  const DOW = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
  const fillDays = () => {
    const weekly = schedSel.value === 'weekly';
    dayLabel.textContent = weekly ? 'Day of week' : 'Day of month';
    const cur = editing && x.schedule_day != null ? Number(x.schedule_day) : (weekly ? 0 : 1);
    daySel.innerHTML = '';
    if (weekly) DOW.forEach((n, i) => daySel.append(el('option', { value: i, selected: i === cur ? 'selected' : null }, n)));
    else for (let d = 1; d <= 28; d++) daySel.append(el('option', { value: d, selected: d === cur ? 'selected' : null }, String(d)));
  };
  g.append(timeField); g.append(dayField); g.append(intervalField);
  const statusSel = el('select', { name: 'status' },
    ...['active', 'inactive', 'retired'].map(o => el('option', { value: o, selected: editing && x.status === o ? 'selected' : null }, o)));
  g.append(el('label', {}, 'Status', statusSel));
  f.append(g);
  const toggleSched = () => {
    const s = schedSel.value;
    const dayed = s === 'weekly' || s === 'monthly';
    timeField.style.display = (s === 'nightly' || dayed) ? '' : 'none';
    intervalField.style.display = s === 'interval' ? '' : 'none';
    dayField.style.display = dayed ? '' : 'none';
    daySel.disabled = !dayed;
    if (dayed) fillDays();
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
      body[k] = (k === 'interval_s' || k === 'schedule_day') ? Number(v) : v;
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
    { label: 'Schedule', render: x => fmtSchedule(x) },
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
  const scheme = t => t ? 'https' : 'http';

  frag.append(el('p', { class: 'muted', style: 'margin-bottom:.6rem' },
    `Currently listening on ${scheme(run.tls)}://${run.bind}:${run.port}.`));
  if (s.restart_required) {
    frag.append(msg('warn', `Pending change: configured ${scheme(cfg.tls)}://${cfg.bind}:${cfg.port}, still running on ${scheme(run.tls)}://${run.bind}:${run.port}. Restart the daemon to apply.`));
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

  const tlsInput = el('input', { name: 'tls', type: 'checkbox', style: 'width:auto;margin:0' });
  tlsInput.checked = !!cfg.tls;
  f.append(el('label', { class: 'row', style: 'gap:.5rem;margin-top:.6rem;color:var(--text)' },
    tlsInput, 'Serve over HTTPS (TLS)'));
  f.append(el('div', { class: 'muted', style: 'font-size:.82rem' },
    'A self-signed certificate is generated automatically on first use. Browsers show a one-time warning you can accept; the connection is encrypted either way.'));

  const warn = el('div', { style: 'margin-top:.5rem' });
  const updateWarn = () => {
    const b = bindInput.value.trim();
    const local = b === '' || b === '127.0.0.1' || b === 'localhost' || b === '::1';
    warn.innerHTML = '';
    if (!local) {
      const tail = tlsInput.checked
        ? 'Traffic is encrypted with TLS.'
        : 'Traffic is still plain HTTP — enable HTTPS below, or only expose this on a trusted network.';
      warn.append(msg('warn',
        '⚠ Off localhost, the UI and API become reachable across your network. In this mode every request — reads included — requires a login or the API token, so set a strong admin password first. ' + tail));
    }
  };
  bindInput.addEventListener('input', updateWarn);
  tlsInput.addEventListener('change', updateWarn);
  updateWarn();
  f.append(warn);

  const out = el('div'); f.append(out);
  const applyNow = async () => {
    out.innerHTML = '';
    const port = Number(f.querySelector('input[name="port"]').value);
    try {
      await api('PUT', '/settings', { bind: bindInput.value.trim(), port, tls: tlsInput.checked });
      const r = await api('POST', '/settings/apply', {});
      const cb = r.configured.bind, cp = r.configured.port, ctls = !!r.configured.tls;
      const sc = ctls ? 'https' : 'http';
      const host = (cb === '0.0.0.0' || cb === '::') ? location.hostname : cb;
      const newUrl = `${sc}://${host}:${cp}/`;
      const manual = () => {
        out.innerHTML = '';
        out.append(msg('warn', `The server is restarting on ${newUrl}. This page couldn’t reconnect ` +
          `automatically` + (ctls ? ' — your browser will warn about the self-signed certificate; accept it to continue.' : '.')));
        out.append(el('div', { style: 'margin-top:.3rem' }, el('a', { href: newUrl }, `Open ${newUrl}`)));
      };
      // A scheme change (http↔https) can't be probed from this origin, and a
      // fresh self-signed cert needs the user to accept it — so hand off to a
      // manual link instead of polling.
      if (location.protocol !== sc + ':') { manual(); return; }
      out.append(msg('warn', `Server restarting on ${sc}://${cb}:${cp} — reconnecting…`));
      let tries = 0;
      const poll = async () => {
        tries += 1;
        try {
          const res = await fetch('/api/v1/version', { cache: 'no-store' });
          if (res.ok) { location.reload(); return; }
        } catch (_) { /* still restarting */ }
        if (tries < 25) { setTimeout(poll, 600); return; }
        manual();
      };
      setTimeout(poll, 1200);
    } catch (ex) { out.append(msg('err', ex.message)); }
  };
  f.append(el('div', { class: 'row', style: 'margin-top:.6rem' },
    el('button', { class: 'btn', type: 'submit' }, 'Save'),
    el('button', { class: 'btn', type: 'button', onclick: applyNow }, 'Restart & apply')));
  f.addEventListener('submit', async e => {
    e.preventDefault();
    out.innerHTML = '';
    try {
      const r = await api('PUT', '/settings', {
        bind: bindInput.value.trim(),
        port: Number(f.querySelector('input[name="port"]').value),
        tls: tlsInput.checked,
      });
      out.append(r.restart_required
        ? msg('warn', `Saved. Restart the daemon (or use “Restart & apply”) to start listening on ${scheme(r.configured.tls)}://${r.configured.bind}:${r.configured.port}.`)
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
