/* Прочность печати — интерфейс приложения. */
(function () {
  'use strict';
  const $ = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const fmt = FDMViewer.fmt;
  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  const DETAIL = [
    { n: 25000, label: 'черновая' }, { n: 60000, label: 'обычная' }, { n: 120000, label: 'точная' },
    { n: 220000, label: 'очень точная' }, { n: 350000, label: 'максимальная' },
  ];
  const LOAD_TYPES = {
    force: { name: 'Сила', unit: 'Н', faces: true },
    mass: { name: 'Подвешенный груз', unit: 'кг', faces: true },
    bearing: { name: 'Нагрузка на отверстие', unit: 'Н', faces: true },
    pressure: { name: 'Давление', unit: 'МПа', faces: true },
    moment: { name: 'Крутящий момент', unit: 'Н·мм', faces: true },
    impact: { name: 'Удар падающим грузом', unit: 'кг', faces: true },
    displacement: { name: 'Заданное перемещение', unit: 'мм', faces: true },
    gravity: { name: 'Собственный вес / перегрузка', unit: 'g', faces: false },
  };
  const DIRS = [['-z', '−Z'], ['+z', '+Z'], ['-x', '−X'], ['+x', '+X'], ['-y', '−Y'], ['+y', '+Y'],
    ['normal_in', 'внутрь поверхности'], ['normal_out', 'от поверхности'], ['custom', 'свой вектор']];
  const AXIS_DIRS = DIRS.filter((d) => d[0].length === 2 || d[0] === 'custom');
  const DURATIONS = [['short', 'Кратковременная'], ['long', 'Длительная (ползучесть)'], ['cyclic', 'Циклическая (усталость)']];
  const PROP_ROWS = [
    ['Модуль упругости, МПа', 'E1', 'E2', 'E3'],
    ['Растяжение, МПа', 'Xt', 'Yt', 'Zt'],
    ['Сжатие, МПа', 'Xc', 'Yc', 'Zc'],
    ['Сдвиг, МПа', null, 'S12', 'S13'],
  ];
  const VERDICT = { ok: 'Выдержит', risk: 'Мало запаса', fail: 'Разрушится' };

  let uid = 1;
  const newCase = (name) => ({ id: uid++, name: name || 'Случай', duration: 'short', cycles: 100000, temperature: 23, thermal: false, loads: [] });
  const S = {
    model: null, gcodeName: '', db: {}, detail: 1, tool: 'plane', brushR: 4, sel: new Set(),
    job: { material: 'PLA', overrides: {}, target_sf: 2, fixtures: [], cases: [newCase('Основная нагрузка')] },
    activeCase: 0, results: null, resCase: 0, field: 'model', hover: null, dirty: false, busy: false, pendingJob: null,
  };
  let viewer;

  // ------------------------------------------------------------------ сервер
  async function api(path, opts) {
    const r = await fetch(path, opts);
    let j;
    try { j = await r.json(); } catch (e) { throw new Error('Сервер вернул неожиданный ответ (' + r.status + ').'); }
    if (!r.ok || (j && j.error)) throw new Error((j && j.error) || r.statusText);
    return j;
  }
  async function waitTask(st) {
    showProgress(true, st.frac, st.text);
    for (;;) {
      await sleep(250);
      const s = await api('/api/task?id=' + st.id);
      showProgress(true, s.frac, s.text);
      if (s.done) {
        showProgress(false);
        if (s.error) throw new Error(s.error);
        return api('/api/task_result?id=' + st.id);
      }
    }
  }
  function showProgress(on, frac, text) {
    $('#progress').hidden = !on;
    if (on) { $('#progBar').style.width = Math.round((frac || 0) * 100) + '%'; $('#progText').textContent = text || ''; }
  }
  function toast(msg, err) {
    const t = $('#toast');
    t.textContent = msg; t.className = 'toast' + (err ? ' err' : ''); t.hidden = false;
    clearTimeout(toast._t);
    toast._t = setTimeout(() => { t.hidden = true; }, err ? 7000 : 3500);
  }
  function setBusy(b) {
    S.busy = b;
    $('#btnRun').disabled = b || !canRun();
    $('#btnExample').disabled = b; $('#btnExample2').disabled = b;
    $('#btnRebuild').disabled = b;
  }

  // ------------------------------------------------------------------ модель
  async function openFile(file) {
    if (!file) return;
    if (/\.bgcode$/i.test(file.name)) { toast('Двоичный G-code (.bgcode) не поддерживается — сохраните из слайсера обычный .gcode.', true); return; }
    setBusy(true);
    try {
      const buf = await file.arrayBuffer();
      const st = await api('/api/model?max_elems=' + DETAIL[S.detail].n, { method: 'POST', body: buf, headers: { 'X-Filename': encodeURIComponent(file.name) } });
      const res = await waitTask(st);
      onModel(res, false);
      if (S.pendingJob) { applyJobFile(S.pendingJob); S.pendingJob = null; }
    } catch (e) { toast(e.message, true); }
    setBusy(false);
  }
  async function openExample() {
    setBusy(true);
    try {
      const name = 'bracket_side.gcode';
      const st = await api('/api/example?name=' + name + '&max_elems=' + DETAIL[S.detail].n, { method: 'POST' });
      const res = await waitTask(st);
      onModel(res, false);
      const job = await api('/api/example_job?name=' + name);
      S.job = fromBackendJob(job);
      S.activeCase = 0;
      renderAll();
      toast('Открыт пример. Закрепления и нагрузки уже заданы — нажмите «Рассчитать».');
    } catch (e) { toast(e.message, true); }
    setBusy(false);
  }
  async function rebuild() {
    if (!S.model) return;
    const pts = snapshotPts();
    setBusy(true);
    try {
      const st = await api('/api/rebuild', { method: 'POST', body: JSON.stringify({ max_elems: DETAIL[S.detail].n }), headers: { 'Content-Type': 'application/json' } });
      const res = await waitTask(st);
      onModel(res, true);
      restorePts(pts);
      renderAll();
    } catch (e) { toast(e.message, true); }
    setBusy(false);
  }
  function onModel(res, keepJob) {
    viewer.setModel(res.model);
    S.model = viewer.model;
    if (res.gcode_name) S.gcodeName = res.gcode_name;
    S.results = null; S.sel = new Set(); S.field = 'model';
    viewer.setResults([]);
    viewer.setCritical(null);
    if (!keepJob) {
      S.job.fixtures = []; S.job.cases = [newCase('Основная нагрузка')]; S.activeCase = 0;
      if (res.material_guess && S.db[res.material_guess]) { S.job.material = res.material_guess; S.job.overrides = {}; }
    }
    $('#emptyState').hidden = true;
    $('#fileName').textContent = S.gcodeName;
    $('#fileName').title = S.gcodeName;
    $('#btnSaveJob').disabled = false;
    $('#stepRes').hidden = true;
    $('#btnReport').setAttribute('aria-disabled', 'true');
    renderAll();
  }

  // ------------------------------------------------------------------ грани <-> точки (для перестроения сетки)
  function faceInfo(k) {
    const M = S.model, g = M.g;
    const fl = Math.floor(k / 6), d = k - fl * 6;
    const ix = fl % g.nx, iy = Math.floor(fl / g.nx) % g.ny, iz = Math.floor(fl / (g.nx * g.ny));
    return { ix, iy, iz, d };
  }
  function facesToPts(keys) {
    const M = S.model, g = M.g, o = M.raw.origin;
    const out = [];
    for (const k of keys) {
      const f = faceInfo(k);
      const D = [[-1, 0, 0], [1, 0, 0], [0, -1, 0], [0, 1, 0], [0, 0, -1], [0, 0, 1]][f.d];
      const z0 = g.z_edges[f.iz], z1 = g.z_edges[f.iz + 1];
      out.push([
        +(g.x0 + (f.ix + 0.5 + D[0] / 2) * g.sx - o[0]).toFixed(3),
        +(g.y0 + (f.iy + 0.5 + D[1] / 2) * g.sy - o[1]).toFixed(3),
        +(0.5 * (z0 + z1) + D[2] * (z1 - z0) / 2 - o[2]).toFixed(3), f.d]);
    }
    return out;
  }
  function ptsToFaces(pts) {
    const M = S.model, g = M.g, o = M.raw.origin;
    const out = new Set();
    const ze = g.z_edges;
    const zIndex = (z) => { let lo = 0, hi = ze.length - 2; while (lo < hi) { const m = (lo + hi + 1) >> 1; if (ze[m] <= z) lo = m; else hi = m - 1; } return lo; };
    const eps = 1e-3;
    for (const p of pts) {
      const d = p[3], a = d >> 1, sg = d & 1 ? 1 : -1;
      // точка на грани; чуть-чуть сдвигаем внутрь детали (против нормали)
      const q = [p[0] + o[0], p[1] + o[1], p[2] + o[2]];
      q[a] -= sg * eps;
      const base = [Math.floor((q[0] - g.x0) / g.sx), Math.floor((q[1] - g.y0) / g.sy), zIndex(q[2])];
      for (const t of [0, 1, -1, 2, -2]) {
        const c = base.slice();
        c[a] += t;
        if (c[0] < 0 || c[1] < 0 || c[2] < 0 || c[0] >= g.nx || c[1] >= g.ny || c[2] >= g.nz) continue;
        const e = M.lookup[(c[2] * g.ny + c[1]) * g.nx + c[0]];
        if (e >= 0 && M.nbr[6 * e + d] < 0) { out.add(M.flat[e] * 6 + d); break; }
      }
    }
    return out;
  }
  function snapshotPts() {
    const res = { fix: S.job.fixtures.map((f) => facesToPts(f.faces)), loads: {} };
    for (const c of S.job.cases) for (const l of c.loads) if (l.faces) res.loads[l.id] = facesToPts(l.faces);
    return res;
  }
  function restorePts(pts) {
    S.job.fixtures.forEach((f, i) => { f.faces = Array.from(ptsToFaces(pts.fix[i] || [])); });
    for (const c of S.job.cases) for (const l of c.loads) if (pts.loads[l.id]) l.faces = Array.from(ptsToFaces(pts.loads[l.id]));
  }

  // ------------------------------------------------------------------ выбор граней
  function holeRegion(h) {
    const a0 = h.dir >> 1;
    let best = null;
    for (let a = 0; a < 3; a++) {
      if (a === a0) continue;
      const r = viewer.floodFaces(h.elem, h.dir, 'axis', a);
      const area = viewer.facesArea(r);
      if (r.size >= 4 && (!best || area < best.area)) best = { r, area };
    }
    return best ? best.r : viewer.floodFaces(h.elem, h.dir, 'plane');
  }
  function onPick(h, mod) {
    if (!S.model || !h || h.cut) return;
    let region;
    if (S.tool === 'plane') region = viewer.floodFaces(h.elem, h.dir, 'plane');
    else if (S.tool === 'hole') region = holeRegion(h);
    else region = viewer.brushFaces(h.point, S.brushR);
    if (mod.alt) for (const k of region) S.sel.delete(k);
    else if (mod.shift) for (const k of region) S.sel.add(k);
    else S.sel = new Set(region);
    updateSel();
  }
  function updateSel() {
    const n = S.sel.size;
    $('#selInfo').innerHTML = n ? 'Выбрано: <b>' + n + '</b> гр. · <b class="num">' + fmt(viewer.facesArea(S.sel), 0) + ' мм²</b>' : 'Щелчок — выбрать, Shift — добавить, Alt — убрать';
    $('#btnClearSel').disabled = !n;
    $('#btnAddFix').disabled = !n;
    $$('[data-act="reselect"]').forEach((b) => { b.disabled = !n; });
    refreshOverlay();
  }

  // ------------------------------------------------------------------ подсветка и стрелки
  function cssVar(n) { return getComputedStyle(document.documentElement).getPropertyValue(n).trim(); }
  function dirVec(dir, faces) {
    if (Array.isArray(dir)) return dir.map(Number);
    if (dir === 'normal_in' || dir === 'normal_out') {
      const D = [[-1, 0, 0], [1, 0, 0], [0, -1, 0], [0, 1, 0], [0, 0, -1], [0, 0, 1]];
      const v = [0, 0, 0];
      for (const k of faces || []) { const d = k % 6; v[0] += D[d][0]; v[1] += D[d][1]; v[2] += D[d][2]; }
      const l = Math.hypot(v[0], v[1], v[2]) || 1;
      const s = dir === 'normal_in' ? -1 : 1;
      return [s * v[0] / l, s * v[1] / l, s * v[2] / l];
    }
    const v = [0, 0, 0];
    const ax = { x: 0, y: 1, z: 2 }[dir[1]];
    v[ax] = dir[0] === '-' ? -1 : 1;
    return v;
  }
  function loadVec(l) {
    const dir = l.dir === 'custom' ? l.vec : l.dir;
    if (l.type === 'pressure') return dirVec('normal_in', l.faces);
    if (l.type === 'moment') return null;
    if (l.type === 'displacement') {
      const v = l.disp.map((x) => (x === '' || x == null ? 0 : +x));
      return Math.hypot(v[0], v[1], v[2]) > 0 ? v : null;
    }
    return dirVec(dir, l.faces);
  }
  function refreshOverlay() {
    if (!S.model) return;
    const fix = cssVar('--fix') || '#3b3f45', load = cssVar('--load') || '#e34948', sel = cssVar('--sel') || '#f2a900';
    const showSetup = S.field === 'model' || S.field === 'structure' || S.field === 'density' || !S.results;
    const ov = [];
    const mk = [];
    const hv = S.hover;
    S.job.fixtures.forEach((f) => ov.push({ keys: f.faces, color: fix, alpha: hv && hv.kind === 'fix' && hv.id === f.id ? 0.95 : 0.7 }));
    if (showSetup) {
      const c = S.job.cases[S.activeCase];
      if (c) for (const l of c.loads) {
        if (!l.faces) continue;
        ov.push({ keys: l.faces, color: load, alpha: hv && hv.kind === 'load' && hv.id === l.id ? 0.95 : 0.65 });
        mk.push({ keys: l.faces, vector: loadVec(l), color: load });
      }
    } else if (S.results) {
      const r = S.results.results[S.resCase];
      const loads = (r && r.summary.loads) || [];
      let li = 0;
      for (const k of Object.keys((r && r.sel) || {})) {
        if (k.startsWith('load')) { const ld = loads[li++]; mk.push({ keys: r.sel[k], vector: ld && ld.vector, color: load }); }
      }
    }
    if (S.sel.size) ov.push({ keys: S.sel, color: sel, alpha: 0.9 });
    viewer.setOverlay(ov);
    viewer.setMarkers(mk);
  }

  // ------------------------------------------------------------------ панель: деталь
  function renderModel() {
    const M = S.model;
    $('#meshCtl').hidden = !M;
    $('#detailVal').textContent = DETAIL[S.detail].label;
    if (!M) return;
    const s = M.summary;
    const chips = [
      ['Слайсер', s.slicer === 'unknown' ? '—' : s.slicer], ['Пластик', s.filament_type || '—'],
      ['Слой', fmt(s.layer_height, 2) + ' мм'], ['Заполнение', Math.round(s.infill_density * 100) + '% ' + (s.infill_pattern || '')],
      ['Габарит', s.size.map((v) => fmt(v, 1)).join(' × ') + ' мм'],
      ['Сетка', fmt(s.voxel, 2) + ' мм · ' + s.elements.toLocaleString('ru-RU') + ' эл.'],
    ];
    let h = '<dl class="kv">' + chips.map((c) => '<dt>' + c[0] + '</dt><dd>' + esc(c[1]) + '</dd>').join('') + '</dl>';
    for (const w of s.warnings || []) h += '<div class="warn">' + esc(w) + '</div>';
    if (s.removed_fraction > 0.01) h += '<div class="warn">Отброшено ' + Math.round(s.removed_fraction * 100) + '% материала, не связанного с основной деталью (другие объекты на столе или мусор).</div>';
    $('#modelInfo').innerHTML = h;
    $('#modelInfo').classList.remove('muted');
  }

  // ------------------------------------------------------------------ панель: материал
  function curMaterial() {
    const base = S.db[S.job.material] || {};
    return Object.assign({}, base, S.job.overrides);
  }
  function renderMaterial() {
    const sel = $('#matSel');
    if (!sel.options.length) {
      for (const k of Object.keys(S.db)) { const o = document.createElement('option'); o.value = k; o.textContent = S.db[k].name; sel.appendChild(o); }
    }
    sel.value = S.job.material;
    $('#targetSf').value = S.job.target_sf;
    const m = curMaterial();
    $('#matNote').textContent = m.note || '';
    let t = '<thead><tr><th></th><th>вдоль нити</th><th>поперёк</th><th>по Z</th></tr></thead><tbody>';
    for (const r of PROP_ROWS) {
      t += '<tr><th>' + r[0] + '</th>';
      for (let i = 1; i < 4; i++) {
        const k = r[i];
        t += '<td>' + (k ? '<input type="number" step="any" data-prop="' + k + '" value="' + (m[k] != null ? m[k] : '') + '" aria-label="' + r[0] + ' ' + k + '">' : '—') + '</td>';
      }
      t += '</tr>';
    }
    t += '<tr><th>Плотность, г/см³</th><td><input type="number" step="any" data-prop="density" value="' + m.density + '"></td><th>HDT, °C</th><td><input type="number" step="any" data-prop="hdt" value="' + m.hdt + '"></td></tr>';
    t += '</tbody>';
    $('#propTable').innerHTML = t;
    $('#propTable').className = 'ptab';
  }

  // ------------------------------------------------------------------ панель: закрепления
  function facesMeta(faces) {
    if (!faces || !faces.length) return '<span class="warn-inline">поверхность не выбрана</span>';
    return faces.length + ' гр. · ' + fmt(viewer.facesArea(faces), 0) + ' мм²';
  }
  function renderFixtures() {
    const ul = $('#fixList');
    ul.innerHTML = S.job.fixtures.map((f) => {
      const ch = (c) => '<label><input type="checkbox" data-comp="' + c + '"' + (f.components.indexOf(c) >= 0 ? ' checked' : '') + '> ' + c.toUpperCase() + '</label>';
      return '<li class="item" data-fid="' + f.id + '"><div class="item-head"><span class="dot" style="background:var(--fix)"></span>' +
        '<input type="text" data-k="name" value="' + esc(f.name) + '" aria-label="Название"><button type="button" class="icon-btn" data-act="reselect" title="Заменить поверхность выбранной"' + (S.sel.size ? '' : ' disabled') + '>⟲</button>' +
        '<button type="button" class="icon-btn" data-act="del" title="Удалить">✕</button></div>' +
        '<div class="meta">' + facesMeta(f.faces) + '</div>' +
        '<div class="chk"><span class="muted">запрещено смещение по</span>' + ch('x') + ch('y') + ch('z') + '</div></li>';
    }).join('');
  }

  // ------------------------------------------------------------------ панель: случаи и нагрузки
  function opt(list, v) { return list.map((o) => '<option value="' + o[0] + '"' + (o[0] === v ? ' selected' : '') + '>' + o[1] + '</option>').join(''); }
  function num(k, v, label, extra) {
    return '<div class="field' + (extra || '') + '"><label>' + label + '</label><input type="number" step="any" data-k="' + k + '" value="' + (v == null ? '' : v) + '"></div>';
  }
  function loadFields(l) {
    const T = LOAD_TYPES[l.type];
    const dirSel = (list) => '<div class="field"><label>Направление</label><select data-k="dir">' + opt(list, l.dir) + '</select></div>' +
      (l.dir === 'custom' ? '<div class="field wide"><label>Вектор направления X, Y, Z</label><div class="vec">' + [0, 1, 2].map((i) => '<input type="number" step="any" data-vec="' + i + '" value="' + l.vec[i] + '">').join('') + '</div></div>' : '');
    switch (l.type) {
      case 'force': case 'bearing':
        return num('value', l.value, 'Сила, Н') + dirSel(DIRS);
      case 'mass':
        return num('value', l.value, 'Масса, кг') + dirSel(DIRS);
      case 'pressure':
        return num('value', l.value, 'Давление, МПа', ' wide') + '<div class="hint wide">Давит на выбранную поверхность. 1 МПа = 10 атм = 1 Н/мм².</div>';
      case 'moment':
        return num('value', l.value, 'Момент, Н·мм') + '<div class="field"><label>Ось вращения</label><select data-k="axis">' + opt(AXIS_DIRS.filter((d) => d[0] !== 'custom'), l.axis) + '</select></div>' +
          '<div class="hint wide">Направление — по правилу правой руки. 1 Н·м = 1000 Н·мм.</div>';
      case 'impact':
        return num('value', l.value, 'Масса груза, кг') + num('height', l.height, 'Высота падения, мм') + dirSel(DIRS);
      case 'displacement':
        return '<div class="field wide"><label>Смещение X, Y, Z, мм (пусто — свободно)</label><div class="vec">' + [0, 1, 2].map((i) => '<input type="number" step="any" data-disp="' + i + '" value="' + (l.disp[i] == null ? '' : l.disp[i]) + '">').join('') + '</div></div>';
      case 'gravity':
        return num('value', l.value, 'Перегрузка, g') + dirSel(AXIS_DIRS) + '<div class="hint wide">1 g — собственный вес детали; 5–10 g — тряска, падение в коробке.</div>';
      default: return '';
    }
    void T;
  }
  function renderCases() {
    const box = $('#caseList');
    box.innerHTML = S.job.cases.map((c, ci) => {
      const loads = c.loads.map((l) => '<li class="item" data-lid="' + l.id + '"><div class="item-head"><span class="dot" style="background:var(--load)"></span><b>' + LOAD_TYPES[l.type].name + '</b>' +
        (LOAD_TYPES[l.type].faces ? '<button type="button" class="icon-btn" data-act="reselect" title="Заменить поверхность выбранной"' + (S.sel.size ? '' : ' disabled') + '>⟲</button>' : '<span style="flex:1"></span>') +
        '<button type="button" class="icon-btn" data-act="delload" title="Удалить">✕</button></div>' +
        (LOAD_TYPES[l.type].faces ? '<div class="meta">' + facesMeta(l.faces) + '</div>' : '') +
        '<div class="load-grid">' + loadFields(l) + '</div></li>').join('');
      return '<div class="case' + (ci === S.activeCase ? ' active' : '') + '" data-cid="' + c.id + '">' +
        '<div class="case-head"><input type="text" data-ck="name" value="' + esc(c.name) + '" aria-label="Название случая">' +
        (S.job.cases.length > 1 ? '<button type="button" class="icon-btn" data-act="delcase" title="Удалить случай">✕</button>' : '') + '</div>' +
        '<div class="case-body"><div class="load-grid">' +
        '<div class="field"><label>Характер нагрузки</label><select data-ck="duration">' + opt(DURATIONS, c.duration) + '</select></div>' +
        '<div class="field"><label>Температура, °C</label><input type="number" step="any" data-ck="temperature" value="' + c.temperature + '"></div>' +
        (c.duration === 'cyclic' ? '<div class="field wide"><label>Число циклов</label><input type="number" step="any" data-ck="cycles" value="' + c.cycles + '"></div>' : '') +
        '<div class="chk wide"><label><input type="checkbox" data-ck="thermal"' + (c.thermal ? ' checked' : '') + '> учитывать тепловое расширение (если деталь зажата)</label></div>' +
        '</div><ul class="items">' + loads + '</ul>' +
        '<div class="addload"><select data-ck="newtype" aria-label="Тип нагрузки">' + Object.keys(LOAD_TYPES).map((k) => '<option value="' + k + '">' + LOAD_TYPES[k].name + '</option>').join('') + '</select>' +
        '<button type="button" class="btn sm" data-act="addload">+ Нагрузка</button></div>' +
        '</div></div>';
    }).join('');
    $$('.case', box).forEach((el, i) => { el.style.outline = i === S.activeCase && S.job.cases.length > 1 ? '1.5px solid var(--fg)' : ''; });
  }

  function findLoad(id) {
    for (const c of S.job.cases) for (const l of c.loads) if (l.id === id) return l;
    return null;
  }
  function defaultsFor(type) {
    const d = { id: uid++, type, faces: [], value: 10, dir: '-z', vec: [0, 0, -1], axis: '+z', height: 300, disp: ['', '', ''] };
    if (type === 'mass') d.value = 1;
    if (type === 'pressure') d.value = 0.1;
    if (type === 'moment') d.value = 1000;
    if (type === 'impact') d.value = 0.5;
    if (type === 'gravity') d.value = 1;
    if (type === 'displacement') d.disp = ['', '', '-1'];
    if (type === 'pressure' || type === 'bearing') d.dir = type === 'bearing' ? '-z' : 'normal_in';
    return d;
  }

  // ------------------------------------------------------------------ результаты
  function renderResults() {
    const R = S.results;
    $('#stepRes').hidden = !R;
    if (!R) return;
    const cards = R.results.map((r, i) => {
      const s = r.summary;
      const kind = [s.duration_name];
      if (s.cycles) kind.push(Number(s.cycles).toLocaleString('ru-RU') + ' циклов');
      if (s.temperature != null) kind.push(fmt(s.temperature, 0) + ' °C');
      let kv = '<dt>Разрушение</dt><dd>' + esc(s.mode) + '</dd>' +
        '<dt>Где</dt><dd class="num">' + s.sf_xyz.map((v) => fmt(v, 1)).join(' · ') + ' мм</dd>' +
        '<dt>Прогиб</dt><dd class="num">' + fmt(s.max_disp, 2) + ' мм</dd>';
      if (s.limit) kv += '<dt>Выдержит до</dt><dd class="num">' + esc(s.limit.short || s.limit.text) + '</dd>';
      if (s.impact_factor) kv += '<dt>Удар</dt><dd>коэффициент <span class="num">' + fmt(s.impact_factor, 2) + '</span></dd>';
      if (s.disp_force) kv += '<dt>Усилие</dt><dd class="num">' + fmt(Math.hypot(s.disp_force[0], s.disp_force[1], s.disp_force[2]), 1) + ' Н</dd>';
      let w = (s.warnings || []).map((x) => '<div class="warn">' + esc(x) + '</div>').join('');
      if (s.sf_min_at_bc) w += '<div class="warn">У места закрепления или нагрузки локальный пик (запас ' + fmt(s.sf_min, 2) + ') — обычно это особенность модели.</div>';
      return '<button type="button" class="rcard' + (i === S.resCase ? ' on' : '') + '" data-ri="' + i + '"><div class="top-row"><div><h3>' + esc(s.name) + '</h3><div class="kind">' + esc(kind.join(' · ')) + '</div></div>' +
        '<span class="verdict ' + s.verdict + '">' + VERDICT[s.verdict] + '</span></div>' +
        '<div class="sfrow"><span class="sfbig ' + s.verdict + '">' + fmt(Math.min(s.sf, 999), 2) + '</span><span class="kind">запас прочности<br>нужно ≥ ' + fmt(s.target_sf, 1) + '</span></div>' +
        '<dl class="kv">' + kv + '</dl>' + w + '</button>';
    }).join('');
    $('#resCards').innerHTML = (S.dirty ? '<div class="warn">Задание изменено после расчёта — нажмите «Рассчитать», чтобы обновить результаты.</div>' : '') + cards +
      '<div class="hint">Время расчёта: ' + fmt(R.total_time, 1) + ' с.</div>';
  }

  // ------------------------------------------------------------------ просмотр
  const FIELDS_PRE = [['model', 'Модель'], ['structure', 'Структура печати'], ['density', 'Плотность']];
  const FIELDS_RES = [['sf', 'Запас прочности'], ['stress', 'Напряжения'], ['disp', 'Перемещения'], ['mode', 'Что разрушится'], ['model', 'Нагрузки'], ['structure', 'Структура']];
  function renderFields() {
    const list = S.results ? FIELDS_RES : FIELDS_PRE;
    $('#fields').innerHTML = list.map((f) => '<button type="button" data-f="' + f[0] + '"' + (f[0] === S.field ? ' class="on"' : '') + (S.model ? '' : ' disabled') + '>' + f[1] + '</button>').join('');
    $('#deformCtl').hidden = !S.results;
    $('#btnWeak').hidden = !S.results;
  }
  function applyView() {
    if (!S.model) return;
    viewer.setTarget(S.job.target_sf);
    viewer.setField(S.field === 'model' ? 'neutral' : S.field, S.resCase);
    const r = S.results && S.results.results[S.resCase];
    viewer.setCritical(r && ['sf', 'stress', 'mode'].indexOf(S.field) >= 0 ? r.summary.sf_xyz : null);
    applyDeform();
    refreshOverlay();
    renderLegend();
  }
  function applyDeform() {
    const on = $('#deformOn').checked && S.results;
    const k = Math.pow(10, ($('#deform').value - 50) / 50);
    const s = on ? viewer.autoDeformScale() * k : 0;
    $('#deformVal').textContent = on ? fmt(s, s >= 10 ? 0 : 1) : '—';
    viewer.setDeform(s);
  }
  function renderLegend() {
    const el = $('#legend');
    const lg = viewer.legend();
    if (!lg) { el.hidden = true; return; }
    let h = '<h4>' + lg.title + '</h4>';
    if (lg.type === 'steps') for (const s of lg.items) h += '<div class="row"><span class="sw" style="background:' + s.color + '"></span><span class="num">' + s.label + '</span></div>';
    else if (lg.type === 'cats') for (const s of lg.items) h += '<div class="row"><span class="sw" style="background:' + s.color + '"></span><span>' + s.name + '</span></div>';
    else {
      const cols = lg.colors.map((c) => 'rgb(' + c.map((v) => Math.round(v * 255)).join(',') + ')');
      h += '<div class="bar" style="background:linear-gradient(90deg,' + cols.join(',') + ')"></div><div class="ticks"><span>0</span><span>' + fmt(lg.max / 2) + '</span><span>' + fmt(lg.max) + (lg.unit || '') + '</span></div>';
    }
    el.innerHTML = h; el.hidden = false;
  }
  function applySection() {
    const a = $('#secAxis').value;
    $('#secPos').disabled = a === '';
    if (!S.model) return;
    if (a === '') viewer.setSection(null);
    else viewer.setSection(+a, ($('#secPos').value / 1000) * S.model.size[+a]);
    refreshOverlay();
  }

  // ------------------------------------------------------------------ задание
  function canRun() {
    return !!S.model && S.job.fixtures.some((f) => f.faces.length) && S.job.cases.some((c) => c.loads.length);
  }
  function markDirty() {
    if (S.results && !S.dirty) { S.dirty = true; renderResults(); }
    $('#btnRun').disabled = S.busy || !canRun();
    $('#runHint').hidden = canRun();
  }
  function toBackendJob() {
    const j = S.job;
    const mat = Object.keys(j.overrides).length ? Object.assign({ base: j.material }, j.overrides) : j.material;
    const where = (f) => ({ faces: f.slice() });
    const load = (l) => {
      const dir = l.dir === 'custom' ? l.vec.map(Number) : l.dir;
      switch (l.type) {
        case 'force': case 'bearing': return { type: l.type, where: where(l.faces), value: +l.value, direction: dir };
        case 'mass': return { type: 'mass', where: where(l.faces), kg: +l.value, direction: dir };
        case 'pressure': return { type: 'pressure', where: where(l.faces), value: +l.value };
        case 'moment': return { type: 'moment', where: where(l.faces), value: +l.value, axis: l.axis };
        case 'impact': return { type: 'impact', where: where(l.faces), kg: +l.value, height: +l.height, direction: dir };
        case 'displacement': return { type: 'displacement', where: where(l.faces), vector: l.disp.map((x) => (x === '' || x == null ? null : +x)) };
        case 'gravity': { const v = dirVec(dir); return { type: 'gravity', g: v.map((x) => x * +l.value) }; }
      }
      return null;
    };
    return {
      title: (S.gcodeName || 'Деталь').replace(/\.[^.]+$/, ''),
      material: mat, target_sf: +j.target_sf || 2,
      fixtures: j.fixtures.filter((f) => f.faces.length).map((f) => ({ name: f.name, where: where(f.faces), components: f.components })),
      cases: j.cases.filter((c) => c.loads.length).map((c) => ({
        name: c.name, duration: c.duration, cycles: +c.cycles, temperature: c.temperature === '' ? null : +c.temperature,
        thermal_expansion: !!c.thermal, loads: c.loads.map(load),
      })),
    };
  }
  function vecToDir(v) {
    v = v.map(Number);
    const L = Math.hypot(v[0], v[1], v[2]);
    if (!L) return { dir: '-z', vec: [0, 0, -1], mag: 0 };
    for (let a = 0; a < 3; a++) {
      if (Math.abs(Math.abs(v[a]) - L) < 1e-9 * L) return { dir: (v[a] < 0 ? '-' : '+') + 'xyz'[a], vec: v.map((x) => x / L), mag: L };
    }
    return { dir: 'custom', vec: v.map((x) => +(x / L).toFixed(4)), mag: L };
  }
  function fromBackendJob(job) {
    const out = { material: 'PLA', overrides: {}, target_sf: job.target_sf || 2, fixtures: [], cases: [] };
    if (typeof job.material === 'string') out.material = job.material;
    else if (job.material) { out.material = job.material.base || 'PLA'; for (const k of Object.keys(job.material)) if (k !== 'base') out.overrides[k] = job.material[k]; }
    const faces = (w) => (w && w.faces ? w.faces.slice() : []);
    for (const f of job.fixtures || []) out.fixtures.push({ id: uid++, name: f.name || 'Закрепление', faces: faces(f.where), components: f.components || 'xyz' });
    for (const c of job.cases || []) {
      const nc = newCase(c.name);
      nc.duration = c.duration || 'short'; nc.cycles = c.cycles || 100000;
      nc.temperature = c.temperature == null ? 23 : c.temperature; nc.thermal = !!c.thermal_expansion;
      for (const l of c.loads || []) {
        const t = l.type === 'torque' ? 'moment' : (l.type === 'acceleration' || l.type === 'weight' || l.type === 'self_weight') ? 'gravity' : l.type;
        const d = defaultsFor(LOAD_TYPES[t] ? t : 'force');
        d.faces = faces(l.where);
        if (l.vector && (t === 'force' || t === 'bearing')) { const r = vecToDir(l.vector); d.value = +r.mag.toFixed(4); d.dir = r.dir; d.vec = r.vec; }
        if (l.value != null && t !== 'moment') d.value = l.value;
        if (l.direction) { if (Array.isArray(l.direction)) { const r = vecToDir(l.direction); d.dir = r.dir; d.vec = r.vec; } else d.dir = l.direction; }
        if (t === 'mass' || t === 'impact') d.value = l.kg != null ? l.kg : l.mass;
        if (t === 'impact') d.height = l.height || 0;
        if (t === 'moment') { if (l.axis) { d.axis = l.axis; d.value = l.value; } else if (l.vector) { const r = vecToDir(l.vector); d.axis = r.dir === 'custom' ? '+z' : r.dir; d.value = r.mag; } }
        if (t === 'displacement') d.disp = (l.vector || [null, null, null]).map((x) => (x == null ? '' : x));
        if (t === 'gravity') { const g = l.g || [0, 0, -1]; const r = vecToDir(Array.isArray(g) ? g : [0, 0, -g]); d.value = +r.mag.toFixed(3); d.dir = r.dir; d.vec = r.vec; }
        nc.loads.push(d);
      }
      out.cases.push(nc);
    }
    if (!out.cases.length) out.cases.push(newCase('Основная нагрузка'));
    return out;
  }
  function saveJob() {
    const data = {
      app: 'fdmfea', version: 1, gcode: S.gcodeName, detail: S.detail,
      ui: S.job, points: S.model ? snapshotPts() : null,
      job: toBackendJob(),
    };
    const blob = new Blob([JSON.stringify(data, null, 1)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = (S.gcodeName || 'деталь').replace(/\.[^.]+$/, '') + '.job.json';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  }
  async function applyJobFile(data) {
    try {
      if (data.app === 'fdmfea' && data.ui) {
        const job = data.ui;
        let maxId = 0;
        for (const f of job.fixtures) maxId = Math.max(maxId, f.id);
        for (const c of job.cases) { maxId = Math.max(maxId, c.id); for (const l of c.loads) maxId = Math.max(maxId, l.id); }
        uid = maxId + 1;
        S.job = job;
        if (data.points) restorePts(data.points);
      } else {
        // задание в формате командной строки: области задаются геометрически
        const resolved = await api('/api/resolve_job', { method: 'POST', body: JSON.stringify(data.job || data), headers: { 'Content-Type': 'application/json' } });
        S.job = fromBackendJob(resolved);
      }
      S.activeCase = 0;
      renderAll();
      markDirty();
      toast('Задание загружено.');
    } catch (e) { toast('Не удалось применить задание: ' + e.message, true); }
  }

  async function runAnalysis() {
    if (!canRun()) return;
    const job = toBackendJob();
    setBusy(true);
    $('#runHint').hidden = true;
    try {
      const st = await api('/api/solve', { method: 'POST', body: JSON.stringify(job), headers: { 'Content-Type': 'application/json' } });
      const res = await waitTask(st);
      S.results = res; S.dirty = false; S.resCase = 0; S.field = 'sf';
      viewer.setResults(res.results, job.target_sf);
      $('#btnReport').setAttribute('aria-disabled', 'false');
      renderFields(); renderResults(); applyView();
      const worst = res.results.reduce((a, r) => Math.min(a, r.summary.sf), Infinity);
      toast('Расчёт готов: минимальный запас прочности ' + fmt(worst, 2) + '.');
      if (window.innerWidth > 900) $('#stepRes').scrollIntoView({ behavior: 'smooth', block: 'start' });
    } catch (e) { toast(e.message, true); }
    setBusy(false);
  }

  function renderAll() {
    renderModel(); renderMaterial(); renderFixtures(); renderCases(); renderResults(); renderFields();
    updateSel(); applyView(); markDirty();
    $('#runHint').hidden = canRun();
  }

  // ------------------------------------------------------------------ события
  function bind() {
    $('#gcodeInput').addEventListener('change', (e) => { openFile(e.target.files[0]); e.target.value = ''; });
    $('#jobInput').addEventListener('change', async (e) => {
      const f = e.target.files[0]; e.target.value = '';
      if (!f) return;
      try {
        const data = JSON.parse(await f.text());
        if (!S.model) { S.pendingJob = data; toast('Задание прочитано. Теперь откройте G-code этой детали' + (data.gcode ? ' (' + data.gcode + ')' : '') + '.'); return; }
        applyJobFile(data);
      } catch (err) { toast('Файл задания не читается: ' + err.message, true); }
    });
    $('#btnExample').addEventListener('click', openExample);
    $('#btnExample2').addEventListener('click', openExample);
    $('#btnSaveJob').addEventListener('click', saveJob);
    $('#btnRun').addEventListener('click', runAnalysis);
    $('#btnRebuild').addEventListener('click', rebuild);
    $('#detail').addEventListener('input', (e) => { S.detail = +e.target.value; $('#detailVal').textContent = DETAIL[S.detail].label; });
    $('#btnClearSel').addEventListener('click', () => { S.sel = new Set(); updateSel(); });
    document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && S.sel.size) { S.sel = new Set(); updateSel(); } });
    $$('#tools button').forEach((b) => b.addEventListener('click', () => {
      S.tool = b.dataset.t;
      $$('#tools button').forEach((x) => x.classList.toggle('on', x === b));
      $('#brushCtl').hidden = S.tool !== 'brush';
    }));
    $('#brushR').addEventListener('input', (e) => { S.brushR = +e.target.value; $('#brushRVal').textContent = S.brushR + ' мм'; });
    $('#fields').addEventListener('click', (e) => {
      const b = e.target.closest('button[data-f]');
      if (!b) return;
      S.field = b.dataset.f;
      renderFields(); applyView();
    });
    $('#deformOn').addEventListener('change', applyDeform);
    $('#deform').addEventListener('input', applyDeform);
    $('#secAxis').addEventListener('change', applySection);
    let raf = 0;
    $('#secPos').addEventListener('input', () => { if (!raf) raf = requestAnimationFrame(() => { raf = 0; applySection(); }); });
    $$('[data-v]').forEach((b) => b.addEventListener('click', () => viewer.view(b.dataset.v)));
    $('#btnWeak').addEventListener('click', () => {
      const r = S.results && S.results.results[S.resCase];
      if (!r) return;
      const z = r.summary.sf_xyz[2];
      $('#secAxis').value = '2';
      $('#secPos').value = Math.min(1000, Math.round((z + 0.01) / S.model.size[2] * 1000));
      applySection();
      viewer.view('top');
    });
    $('#btnReport').addEventListener('click', (e) => { if ($('#btnReport').getAttribute('aria-disabled') === 'true') e.preventDefault(); });

    // материал
    $('#matSel').addEventListener('change', (e) => { S.job.material = e.target.value; S.job.overrides = {}; renderMaterial(); markDirty(); });
    $('#targetSf').addEventListener('change', (e) => { S.job.target_sf = Math.max(1, +e.target.value || 2); markDirty(); if (S.results) { viewer.setTarget(S.job.target_sf); renderLegend(); } });
    $('#propTable').addEventListener('change', (e) => {
      const k = e.target.dataset.prop;
      if (!k) return;
      const v = +e.target.value;
      if (!(v > 0)) { toast('Значение должно быть больше нуля.', true); renderMaterial(); return; }
      S.job.overrides[k] = v; markDirty();
    });

    // закрепления
    $('#btnAddFix').addEventListener('click', () => {
      if (!S.sel.size) return;
      const n = S.job.fixtures.length + 1;
      S.job.fixtures.push({ id: uid++, name: 'Закрепление ' + n, faces: Array.from(S.sel), components: 'xyz' });
      S.sel = new Set();
      renderFixtures(); updateSel(); markDirty();
    });
    const fixList = $('#fixList');
    fixList.addEventListener('input', (e) => {
      const li = e.target.closest('[data-fid]'); if (!li) return;
      const f = S.job.fixtures.find((x) => x.id === +li.dataset.fid);
      if (e.target.dataset.k === 'name') f.name = e.target.value;
    });
    fixList.addEventListener('change', (e) => {
      const li = e.target.closest('[data-fid]'); if (!li) return;
      const f = S.job.fixtures.find((x) => x.id === +li.dataset.fid);
      if (e.target.dataset.comp) {
        f.components = $$('[data-comp]', li).filter((c) => c.checked).map((c) => c.dataset.comp).join('');
        if (!f.components) { toast('Закрепление без осей ничего не держит — отметьте хотя бы одну ось.', true); }
        markDirty();
      }
    });
    fixList.addEventListener('click', (e) => {
      const b = e.target.closest('[data-act]'); if (!b) return;
      const li = b.closest('[data-fid]');
      const i = S.job.fixtures.findIndex((x) => x.id === +li.dataset.fid);
      if (b.dataset.act === 'del') S.job.fixtures.splice(i, 1);
      if (b.dataset.act === 'reselect' && S.sel.size) { S.job.fixtures[i].faces = Array.from(S.sel); S.sel = new Set(); }
      renderFixtures(); updateSel(); markDirty();
    });
    fixList.addEventListener('mouseover', (e) => { const li = e.target.closest('[data-fid]'); const h = li ? { kind: 'fix', id: +li.dataset.fid } : null; if (JSON.stringify(h) !== JSON.stringify(S.hover)) { S.hover = h; refreshOverlay(); } });
    fixList.addEventListener('mouseleave', () => { S.hover = null; refreshOverlay(); });

    // случаи
    $('#btnAddCase').addEventListener('click', () => { S.job.cases.push(newCase('Случай ' + (S.job.cases.length + 1))); S.activeCase = S.job.cases.length - 1; renderCases(); refreshOverlay(); markDirty(); });
    const box = $('#caseList');
    const caseOf = (el) => { const c = el.closest('[data-cid]'); return c ? S.job.cases.findIndex((x) => x.id === +c.dataset.cid) : -1; };
    box.addEventListener('focusin', (e) => { const ci = caseOf(e.target); if (ci >= 0 && ci !== S.activeCase) { S.activeCase = ci; $$('.case', box).forEach((el, i) => { el.style.outline = i === ci && S.job.cases.length > 1 ? '1.5px solid var(--fg)' : ''; }); refreshOverlay(); } });
    box.addEventListener('input', (e) => {
      const t = e.target, ci = caseOf(t); if (ci < 0) return;
      const c = S.job.cases[ci];
      if (t.dataset.ck === 'name') { c.name = t.value; return; }
      if (t.dataset.ck === 'temperature' || t.dataset.ck === 'cycles') { c[t.dataset.ck] = t.value; markDirty(); return; }
      const li = t.closest('[data-lid]'); if (!li) return;
      const l = findLoad(+li.dataset.lid);
      if (t.dataset.k === 'value' || t.dataset.k === 'height') { l[t.dataset.k] = t.value; markDirty(); }
      else if (t.dataset.vec) { l.vec[+t.dataset.vec] = t.value; markDirty(); refreshOverlay(); }
      else if (t.dataset.disp) { l.disp[+t.dataset.disp] = t.value; markDirty(); refreshOverlay(); }
    });
    box.addEventListener('change', (e) => {
      const t = e.target, ci = caseOf(t); if (ci < 0) return;
      const c = S.job.cases[ci];
      if (t.dataset.ck === 'duration') { c.duration = t.value; renderCases(); markDirty(); return; }
      if (t.dataset.ck === 'thermal') { c.thermal = t.checked; markDirty(); return; }
      const li = t.closest('[data-lid]'); if (!li) return;
      const l = findLoad(+li.dataset.lid);
      if (t.dataset.k === 'dir') { l.dir = t.value; renderCases(); refreshOverlay(); markDirty(); }
      if (t.dataset.k === 'axis') { l.axis = t.value; markDirty(); }
    });
    box.addEventListener('click', (e) => {
      const b = e.target.closest('[data-act]'); if (!b) return;
      const ci = caseOf(b); const c = S.job.cases[ci];
      if (b.dataset.act === 'delcase') { S.job.cases.splice(ci, 1); S.activeCase = Math.max(0, Math.min(S.activeCase, S.job.cases.length - 1)); }
      if (b.dataset.act === 'addload') {
        const type = $('[data-ck="newtype"]', b.closest('.case')).value;
        if (LOAD_TYPES[type].faces && !S.sel.size) { toast('Сначала выберите на модели поверхность, к которой приложена нагрузка.', true); return; }
        const l = defaultsFor(type);
        if (LOAD_TYPES[type].faces) { l.faces = Array.from(S.sel); S.sel = new Set(); }
        c.loads.push(l); S.activeCase = ci;
      }
      const li = b.closest('[data-lid]');
      if (li) {
        const idx = c.loads.findIndex((x) => x.id === +li.dataset.lid);
        if (b.dataset.act === 'delload') c.loads.splice(idx, 1);
        if (b.dataset.act === 'reselect' && S.sel.size) { c.loads[idx].faces = Array.from(S.sel); S.sel = new Set(); }
      }
      renderCases(); updateSel(); markDirty();
    });
    box.addEventListener('mouseover', (e) => { const li = e.target.closest('[data-lid]'); const h = li ? { kind: 'load', id: +li.dataset.lid } : null; if (JSON.stringify(h) !== JSON.stringify(S.hover)) { S.hover = h; refreshOverlay(); } });
    box.addEventListener('mouseleave', () => { S.hover = null; refreshOverlay(); });

    // результаты
    $('#resCards').addEventListener('click', (e) => {
      const b = e.target.closest('[data-ri]'); if (!b) return;
      S.resCase = +b.dataset.ri;
      if (['model', 'structure', 'density'].indexOf(S.field) >= 0) S.field = 'sf';
      viewer.setCase(S.resCase);
      renderResults(); renderFields(); applyView();
    });

    // перетаскивание файла
    const vz = $('#viewer'), dz = $('#dropzone');
    let depth = 0;
    window.addEventListener('dragenter', (e) => { if (e.dataTransfer && Array.from(e.dataTransfer.types).indexOf('Files') >= 0) { depth++; dz.hidden = false; e.preventDefault(); } });
    window.addEventListener('dragleave', () => { depth = Math.max(0, depth - 1); if (!depth) dz.hidden = true; });
    window.addEventListener('dragover', (e) => e.preventDefault());
    window.addEventListener('drop', (e) => {
      e.preventDefault(); depth = 0; dz.hidden = true;
      const f = e.dataTransfer.files[0];
      if (!f) return;
      if (/\.json$/i.test(f.name)) { $('#jobInput').files = e.dataTransfer.files; $('#jobInput').dispatchEvent(new Event('change')); }
      else openFile(f);
    });
    void vz;
  }

  async function init() {
    try {
      viewer = new FDMViewer.VoxelViewer($('#viewer'));
    } catch (e) {
      $('#emptyState').innerHTML = '<div class="drop"><b>Нет WebGL</b><span>Браузер не может показать 3D. Откройте приложение в свежем Chrome, Edge или Firefox.</span></div>';
      return;
    }
    viewer.onPick = onPick;
    window.__fdm = { S, viewer };   // для отладки из консоли браузера
    bind();
    try { S.db = await api('/api/materials'); } catch (e) { toast('Не удалось получить список материалов: ' + e.message, true); }
    renderAll();
  }
  init();
})();
