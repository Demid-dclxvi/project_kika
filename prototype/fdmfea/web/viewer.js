/* 3D-просмотр воксельной модели и результатов расчёта (three.js r149, глобальный THREE). */
(function (global) {
  'use strict';

  // ------------------------------------------------------------------ утилиты
  function b64ToArray(b64, Type) {
    const bin = atob(b64);
    const n = bin.length;
    const buf = new ArrayBuffer(n);
    const u8 = new Uint8Array(buf);
    for (let i = 0; i < n; i++) u8[i] = bin.charCodeAt(i);
    return new Type(buf);
  }
  function hex(h) {
    const c = new THREE.Color(h);
    return [c.r, c.g, c.b];
  }
  function lerp3(a, b, t) {
    return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
  }
  function ramp(stops, t) {
    t = Math.max(0, Math.min(1, t));
    const k = t * (stops.length - 1);
    const i = Math.min(stops.length - 2, Math.floor(k));
    return lerp3(stops[i], stops[i + 1], k - i);
  }

  const LOCAL = [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0], [0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]];
  const FACE_NODES = [[0, 4, 7, 3], [1, 2, 6, 5], [0, 1, 5, 4], [3, 7, 6, 2], [0, 3, 2, 1], [4, 5, 6, 7]];
  const DIRS = [[-1, 0, 0], [1, 0, 0], [0, -1, 0], [0, 1, 0], [0, 0, -1], [0, 0, 1]];
  const OPP = [1, 0, 3, 2, 5, 4];

  // палитры (проверены на различимость при нарушениях цветового зрения)
  const BLUE_RAMP = ['#cde2fb', '#9ec5f4', '#6da7ec', '#3987e5', '#256abf', '#184f95', '#0d366b'].map(hex);
  const CAT = { blue: '#2a78d6', orange: '#eb6834', aqua: '#1baf7a' };
  const STATUS = { critical: '#d03b3b', serious: '#ec835a', warning: '#fab219' };

  const MODE_GROUP = [0, 0, 1, 1, 2, 2, 2, 1];   // вид разрушения -> группа
  const MODE_GROUPS = [
    { name: 'Вдоль нити', color: CAT.blue },
    { name: 'Между нитями в слое', color: CAT.aqua },
    { name: 'Между слоями', color: CAT.orange },
  ];
  const ROLE_GROUPS = [
    { name: 'Стенки', color: CAT.blue },
    { name: 'Сплошное заполнение', color: CAT.orange },
    { name: 'Разреженное заполнение', color: CAT.aqua },
  ];

  function fmt(v, d) {
    if (!isFinite(v)) return '—';
    const a = Math.abs(v);
    if (d === undefined) d = a >= 100 ? 0 : a >= 10 ? 1 : a >= 1 ? 2 : 3;
    return v.toFixed(d).replace('.', ',');
  }

  // ------------------------------------------------------------------ модель
  function decodeModel(m) {
    const g = m.grid;
    const n = m.n_elems;
    const el = b64ToArray(m.elems, Int16Array);
    const ix = new Int32Array(n), iy = new Int32Array(n), iz = new Int32Array(n);
    for (let e = 0; e < n; e++) { ix[e] = el[3 * e]; iy[e] = el[3 * e + 1]; iz[e] = el[3 * e + 2]; }
    const nx = g.nx, ny = g.ny, nz = g.nz;
    const lookup = new Int32Array(nx * ny * nz).fill(-1);
    for (let e = 0; e < n; e++) lookup[(iz[e] * ny + iy[e]) * nx + ix[e]] = e;
    const nodeFlat = b64ToArray(m.node_flat, Int32Array);
    const nx1 = nx + 1, ny1 = ny + 1;
    const nodeMap = new Int32Array(nx1 * ny1 * (nz + 1)).fill(-1);
    for (let i = 0; i < nodeFlat.length; i++) nodeMap[nodeFlat[i]] = i;
    const corners = new Int32Array(n * 8);
    for (let e = 0; e < n; e++) {
      for (let c = 0; c < 8; c++) {
        const L = LOCAL[c];
        corners[8 * e + c] = nodeMap[((iz[e] + L[2]) * ny1 + (iy[e] + L[1])) * nx1 + (ix[e] + L[0])];
      }
    }
    const nbr = new Int32Array(n * 6);
    for (let e = 0; e < n; e++) {
      for (let d = 0; d < 6; d++) {
        const a = ix[e] + DIRS[d][0], b = iy[e] + DIRS[d][1], c = iz[e] + DIRS[d][2];
        nbr[6 * e + d] = (a < 0 || b < 0 || c < 0 || a >= nx || b >= ny || c >= nz) ? -1 : lookup[(c * ny + b) * nx + a];
      }
    }
    const o = m.origin;
    const nn = nodeFlat.length;
    const npos = new Float32Array(nn * 3);
    for (let i = 0; i < nn; i++) {
      const f = nodeFlat[i];
      const k = Math.floor(f / (nx1 * ny1));
      const r = f - k * nx1 * ny1;
      const j = Math.floor(r / nx1);
      const ii = r - j * nx1;
      npos[3 * i] = g.x0 + ii * g.sx - o[0];
      npos[3 * i + 1] = g.y0 + j * g.sy - o[1];
      npos[3 * i + 2] = g.z_edges[k] - o[2];
    }
    const center = new Float32Array(n * 3);
    for (let e = 0; e < n; e++) {
      center[3 * e] = g.x0 + (ix[e] + 0.5) * g.sx - o[0];
      center[3 * e + 1] = g.y0 + (iy[e] + 0.5) * g.sy - o[1];
      center[3 * e + 2] = 0.5 * (g.z_edges[iz[e]] + g.z_edges[iz[e] + 1]) - o[2];
    }
    const flat = new Float64Array(n);
    for (let e = 0; e < n; e++) flat[e] = (iz[e] * ny + iy[e]) * nx + ix[e];
    return {
      raw: m, n, nn, g, ix, iy, iz, lookup, corners, nbr, npos, center, flat,
      role: b64ToArray(m.role, Uint8Array), rho: b64ToArray(m.rho, Uint8Array),
      size: m.size, summary: m.summary,
    };
  }

  function decodeResult(r) {
    return {
      summary: r.summary,
      sf: b64ToArray(r.sf, Float32Array),
      vm: b64ToArray(r.vm, Float32Array),
      mode: b64ToArray(r.mode, Uint8Array),
      u: b64ToArray(r.u, Float32Array),
      sel: r.sel || {},
    };
  }

  function percentile(arr, p) {
    const a = Array.from(arr).filter(isFinite).sort((x, y) => x - y);
    if (!a.length) return 0;
    return a[Math.min(a.length - 1, Math.floor(p * (a.length - 1)))];
  }

  // ------------------------------------------------------------------ управление камерой
  class Turntable {
    constructor(camera, dom, onChange, onClick) {
      this.camera = camera; this.dom = dom; this.onChange = onChange; this.onClick = onClick;
      this.target = new THREE.Vector3(); this.radius = 100; this.theta = -Math.PI / 4; this.phi = 1.05;
      this.pointers = new Map(); this.mode = null; this.down = null;
      dom.addEventListener('pointerdown', (e) => this._down(e));
      dom.addEventListener('pointermove', (e) => this._move(e));
      dom.addEventListener('pointerup', (e) => this._up(e));
      dom.addEventListener('pointercancel', (e) => this._up(e));
      dom.addEventListener('wheel', (e) => { e.preventDefault(); this.zoom(Math.exp(e.deltaY * 0.0012)); }, { passive: false });
      dom.addEventListener('contextmenu', (e) => e.preventDefault());
      this.update();
    }
    update() {
      const s = Math.sin(this.phi);
      const p = new THREE.Vector3(s * Math.cos(this.theta), s * Math.sin(this.theta), Math.cos(this.phi)).multiplyScalar(this.radius).add(this.target);
      this.camera.position.copy(p);
      this.camera.up.set(0, 0, 1);
      this.camera.lookAt(this.target);
      this.camera.near = this.radius / 200; this.camera.far = this.radius * 20;
      this.camera.updateProjectionMatrix();
      this.onChange && this.onChange();
    }
    zoom(f) { this.radius = Math.max(1, Math.min(5000, this.radius * f)); this.update(); }
    pan(dx, dy) {
      const h = this.dom.clientHeight || 1;
      const k = 2 * this.radius * Math.tan(THREE.MathUtils.degToRad(this.camera.fov / 2)) / h;
      const right = new THREE.Vector3().setFromMatrixColumn(this.camera.matrix, 0);
      const up = new THREE.Vector3().setFromMatrixColumn(this.camera.matrix, 1);
      this.target.addScaledVector(right, -dx * k).addScaledVector(up, dy * k);
      this.update();
    }
    rotate(dx, dy) {
      this.theta -= dx * 0.008;
      this.phi = Math.max(0.02, Math.min(Math.PI - 0.02, this.phi - dy * 0.008));
      this.update();
    }
    _down(e) {
      this.dom.setPointerCapture(e.pointerId);
      this.pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
      if (this.pointers.size === 1) {
        this.mode = (e.button === 2 || e.button === 1 || e.shiftKey) ? 'pan' : 'rotate';
        this.down = { x: e.clientX, y: e.clientY, t: performance.now(), shift: e.shiftKey, alt: e.altKey, button: e.button, moved: 0 };
      } else if (this.pointers.size === 2) {
        this.mode = 'pinch';
        const [a, b] = [...this.pointers.values()];
        this.pinch = { d: Math.hypot(a.x - b.x, a.y - b.y), cx: (a.x + b.x) / 2, cy: (a.y + b.y) / 2 };
        if (this.down) this.down.moved = 99;
      }
    }
    _move(e) {
      if (!this.pointers.has(e.pointerId)) return;
      const prev = this.pointers.get(e.pointerId);
      const dx = e.clientX - prev.x, dy = e.clientY - prev.y;
      this.pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
      if (this.down) this.down.moved += Math.abs(dx) + Math.abs(dy);
      if (this.mode === 'rotate') this.rotate(dx, dy);
      else if (this.mode === 'pan') this.pan(dx, dy);
      else if (this.mode === 'pinch' && this.pointers.size === 2) {
        const [a, b] = [...this.pointers.values()];
        const d = Math.hypot(a.x - b.x, a.y - b.y);
        const cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
        if (this.pinch.d > 0 && d > 0) this.zoom(this.pinch.d / d);
        this.pan(cx - this.pinch.cx, cy - this.pinch.cy);
        this.pinch = { d, cx, cy };
      }
    }
    _up(e) {
      const wasClick = this.down && this.down.moved < 5 && this.pointers.size === 1 && performance.now() - this.down.t < 600;
      if (wasClick && this.onClick && this.down.button === 0) this.onClick(e, { shift: this.down.shift || e.shiftKey, alt: this.down.alt || e.altKey });
      this.pointers.delete(e.pointerId);
      if (this.pointers.size === 0) { this.mode = null; this.down = null; }
      else if (this.pointers.size === 1) { this.mode = 'rotate'; }
    }
  }

  // ------------------------------------------------------------------ просмотрщик
  class VoxelViewer {
    constructor(container, opts) {
      opts = opts || {};
      this.el = container;
      this.el.classList.add('vw');
      this.canvasWrap = document.createElement('div');
      this.canvasWrap.className = 'vw-canvas';
      this.el.appendChild(this.canvasWrap);
      this.tip = document.createElement('div');
      this.tip.className = 'vw-tip';
      this.tip.hidden = true;
      this.el.appendChild(this.tip);
      this.renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false, preserveDrawingBuffer: true });
      this.renderer.setPixelRatio(Math.min(2, global.devicePixelRatio || 1));
      this.canvasWrap.appendChild(this.renderer.domElement);
      this.scene = new THREE.Scene();
      this.camera = new THREE.PerspectiveCamera(35, 1, 0.1, 10000);
      this.scene.add(new THREE.HemisphereLight(0xffffff, 0x8a8a80, 0.75));
      const d1 = new THREE.DirectionalLight(0xffffff, 0.55); d1.position.set(1, -1.5, 2); this.camera.add(d1);
      const d2 = new THREE.DirectionalLight(0xffffff, 0.25); d2.position.set(-1, 1, -0.5); this.camera.add(d2);
      this.scene.add(this.camera);
      this.material = new THREE.MeshLambertMaterial({ vertexColors: true, side: THREE.DoubleSide });
      this.mesh = null;
      this.markers = new THREE.Group(); this.scene.add(this.markers);
      this.helpers = new THREE.Group(); this.scene.add(this.helpers);
      this.controls = new Turntable(this.camera, this.renderer.domElement, () => this.requestRender(), (e, mod) => this._click(e, mod));
      this.raycaster = new THREE.Raycaster();
      this.field = 'neutral'; this.caseIdx = 0; this.deform = 0; this.section = null;
      this.results = []; this.overlay = []; this.target = 2.0;
      this.onPick = null; this.onHover = null;
      this.renderer.domElement.addEventListener('pointermove', (e) => this._hover(e));
      this.renderer.domElement.addEventListener('pointerleave', () => { this.tip.hidden = true; });
      this._ro = new ResizeObserver(() => this.resize());
      this._ro.observe(this.el);
      this._themeWatch();
      this.resize();
    }

    _themeWatch() {
      const upd = () => { this._readTheme(); if (this.model) this.recolor(); this._buildHelpers(); this.requestRender(); };
      try { global.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', upd); } catch (e) { /* старые браузеры */ }
      new MutationObserver(upd).observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme', 'class'] });
      this._readTheme();
    }
    _readTheme() {
      const cs = getComputedStyle(this.el);
      const get = (k, d) => (cs.getPropertyValue(k) || d).trim() || d;
      this.theme = {
        bg: get('--vw-bg', '#eef0ed'), grid: get('--vw-grid', '#c9ccc4'), grid2: get('--vw-grid-2', '#dde0d8'),
        neutral: get('--vw-neutral', '#c8c7c0'), part: get('--vw-part', '#d9dccf'),
        sel: get('--vw-sel', '#f2a900'), fix: get('--vw-fix', '#2a78d6'), load: get('--vw-load', '#e34948'),
        dark: get('--vw-dark', '0') === '1',
      };
      this.renderer.setClearColor(new THREE.Color(this.theme.bg), 1);
    }

    resize() {
      const w = this.el.clientWidth || 300, h = this.el.clientHeight || 300;
      this.renderer.setSize(w, h, false);
      this.renderer.domElement.style.width = w + 'px';
      this.renderer.domElement.style.height = h + 'px';
      this.camera.aspect = w / h;
      this.camera.updateProjectionMatrix();
      this.requestRender();
    }
    requestRender() {
      if (this._raf) return;
      this._raf = requestAnimationFrame(() => { this._raf = 0; this.renderer.render(this.scene, this.camera); });
    }

    // ---------------- данные
    setModel(modelPayload) {
      this.model = decodeModel(modelPayload);
      this.results = [];
      this.overlay = [];
      this.section = null;
      this.vis = new Uint8Array(this.model.n).fill(1);
      this._buildHelpers();
      this.rebuild();
      this.view('iso');
    }
    setResults(resultsPayload, target) {
      this.results = (resultsPayload || []).map(decodeResult);
      if (target) this.target = target;
      this.rebuild();
    }
    setTarget(t) { this.target = t; this.recolor(); }
    setField(field, caseIdx) {
      this.field = field;
      if (caseIdx !== undefined) this.caseIdx = caseIdx;
      if (this.deform && this.results.length) this.updatePositions();
      this.recolor();
      this._buildMarkers();
    }
    setCase(i) { this.caseIdx = i; this.updatePositions(); this.recolor(); this._buildMarkers(); }
    setDeform(scale) { this.deform = scale; this.updatePositions(); this._buildMarkers(); }
    autoDeformScale() {
      const r = this.results[this.caseIdx];
      if (!r) return 0;
      const md = r.summary.max_disp || 0;
      const L = Math.max.apply(null, this.model.size);
      return md > 0 ? 0.08 * L / md : 0;
    }
    setSection(axis, pos) {
      this.section = (axis === null || axis === undefined) ? null : { axis, pos };
      this.rebuild();
    }
    // overlay: [{keys: Set|Array, color, alpha}]
    setOverlay(list) {
      this.overlay = (list || []).map((o) => ({ keys: o.keys instanceof Set ? o.keys : new Set(o.keys), color: hex(o.color), alpha: o.alpha === undefined ? 0.85 : o.alpha }));
      this.recolor();
    }
    // markers: [{keys, vector:[x,y,z]|null, color, label}]
    setMarkers(list) { this.markerSpec = list || []; this._buildMarkers(); }

    // ---------------- геометрия
    rebuild() {
      const M = this.model;
      if (!M) return;
      const n = M.n, vis = this.vis;
      if (this.section) {
        const a = this.section.axis, p = this.section.pos;
        for (let e = 0; e < n; e++) vis[e] = M.center[3 * e + a] <= p ? 1 : 0;
      } else vis.fill(1);
      let nf = 0;
      for (let e = 0; e < n; e++) {
        if (!vis[e]) continue;
        for (let d = 0; d < 6; d++) { const b = M.nbr[6 * e + d]; if (b < 0 || !vis[b]) nf++; }
      }
      const fe = new Int32Array(nf), fd = new Uint8Array(nf), cut = new Uint8Array(nf);
      let k = 0;
      for (let e = 0; e < n; e++) {
        if (!vis[e]) continue;
        for (let d = 0; d < 6; d++) {
          const b = M.nbr[6 * e + d];
          if (b < 0 || !vis[b]) { fe[k] = e; fd[k] = d; cut[k] = b >= 0 ? 1 : 0; k++; }
        }
      }
      this.faceElem = fe; this.faceDir = fd; this.faceCut = cut; this.nFaces = nf;
      const geom = new THREE.BufferGeometry();
      this.posArr = new Float32Array(nf * 12);
      this.nrmArr = new Float32Array(nf * 12);
      this.colArr = new Float32Array(nf * 12);
      const idx = new Uint32Array(nf * 6);
      for (let f = 0; f < nf; f++) {
        const v = 4 * f;
        idx.set([v, v + 1, v + 2, v, v + 2, v + 3], 6 * f);
      }
      geom.setIndex(new THREE.BufferAttribute(idx, 1));
      geom.setAttribute('position', new THREE.BufferAttribute(this.posArr, 3));
      geom.setAttribute('normal', new THREE.BufferAttribute(this.nrmArr, 3));
      geom.setAttribute('color', new THREE.BufferAttribute(this.colArr, 3));
      if (this.mesh) { this.scene.remove(this.mesh); this.mesh.geometry.dispose(); }
      this.mesh = new THREE.Mesh(geom, this.material);
      this.scene.add(this.mesh);
      this.updatePositions(true);
      this.recolor();
      this._buildMarkers();
    }

    _nodeDisp() {
      const r = this.results[this.caseIdx];
      return (this.deform && r) ? r.u : null;
    }

    updatePositions(skipRender) {
      const M = this.model;
      if (!M || !this.mesh) return;
      const P = this.posArr, N = this.nrmArr, np = M.npos;
      const U = this._nodeDisp();
      const s = this.deform;
      const tmp = new Float32Array(12);
      for (let f = 0; f < this.nFaces; f++) {
        const e = this.faceElem[f], d = this.faceDir[f];
        const fn = FACE_NODES[d];
        for (let q = 0; q < 4; q++) {
          const node = M.corners[8 * e + fn[q]];
          let x = np[3 * node], y = np[3 * node + 1], z = np[3 * node + 2];
          if (U) { x += s * U[3 * node]; y += s * U[3 * node + 1]; z += s * U[3 * node + 2]; }
          tmp[3 * q] = x; tmp[3 * q + 1] = y; tmp[3 * q + 2] = z;
        }
        P.set(tmp, 12 * f);
        // нормаль по диагоналям четырёхугольника
        const ax = tmp[6] - tmp[0], ay = tmp[7] - tmp[1], az = tmp[8] - tmp[2];
        const bx = tmp[9] - tmp[3], by = tmp[10] - tmp[4], bz = tmp[11] - tmp[5];
        let nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
        const l = Math.hypot(nx, ny, nz) || 1; nx /= l; ny /= l; nz /= l;
        for (let q = 0; q < 4; q++) { N[12 * f + 3 * q] = nx; N[12 * f + 3 * q + 1] = ny; N[12 * f + 3 * q + 2] = nz; }
      }
      this.mesh.geometry.attributes.position.needsUpdate = true;
      this.mesh.geometry.attributes.normal.needsUpdate = true;
      this.mesh.geometry.computeBoundingSphere();
      if (!skipRender) this.requestRender();
    }

    // ---------------- цвета
    _elemColorFn() {
      const M = this.model, r = this.results[this.caseIdx], T = this.target, th = this.theme;
      const neutral = hex(th.part);
      const gray = hex(th.neutral);
      const f = this.field;
      if (f === 'sf' && r) {
        const steps = this.sfSteps().map((s) => ({ hi: s.hi, c: hex(s.color) }));
        return (e) => { const v = r.sf[e]; for (const s of steps) if (v < s.hi) return s.c; return steps[steps.length - 1].c; };
      }
      if (f === 'stress' && r) {
        const mx = this.fieldRange().max || 1;
        return (e) => ramp(BLUE_RAMP, r.vm[e] / mx);
      }
      if (f === 'disp' && r) {
        const mx = this.fieldRange().max || 1;
        const U = r.u;
        return (e) => {
          let s = 0;
          for (let c = 0; c < 8; c++) { const nd = M.corners[8 * e + c]; s += Math.hypot(U[3 * nd], U[3 * nd + 1], U[3 * nd + 2]); }
          return ramp(BLUE_RAMP, s / 8 / mx);
        };
      }
      if (f === 'mode' && r) {
        const cols = MODE_GROUPS.map((g) => hex(g.color));
        return (e) => (r.sf[e] < 2 * T ? cols[MODE_GROUP[r.mode[e]] || 0] : gray);
      }
      if (f === 'structure') {
        const cols = ROLE_GROUPS.map((g) => hex(g.color));
        return (e) => cols[M.role[e]] || neutral;
      }
      if (f === 'density') return (e) => ramp(BLUE_RAMP, M.rho[e] / 255);
      return () => neutral;
    }

    sfSteps() {
      const T = this.target;
      const s = [{ hi: 1, color: STATUS.critical, label: '< 1' }];
      if (T > 1.5) {
        s.push({ hi: 1.5, color: STATUS.serious, label: '1–1,5' });
        s.push({ hi: T, color: STATUS.warning, label: '1,5–' + fmt(T, 1) });
      } else s.push({ hi: T, color: STATUS.serious, label: '1–' + fmt(T, 1) });
      s.push({ hi: 2 * T, color: '#b7d3f6', label: fmt(T, 1) + '–' + fmt(2 * T, 1) });
      s.push({ hi: 4 * T, color: '#6da7ec', label: fmt(2 * T, 1) + '–' + fmt(4 * T, 1) });
      s.push({ hi: Infinity, color: '#256abf', label: '> ' + fmt(4 * T, 1) });
      return s;
    }

    fieldRange() {
      const r = this.results[this.caseIdx];
      if (!r) return { max: 1 };
      if (this.field === 'stress') {
        if (!r._vmMax) r._vmMax = percentile(r.vm, 0.995) || 1;
        return { max: r._vmMax };
      }
      if (this.field === 'disp') return { max: r.summary.max_disp || 1 };
      return { max: 1 };
    }

    legend() {
      const f = this.field, r = this.results[this.caseIdx];
      if (f === 'sf' && r) return { type: 'steps', title: 'Запас прочности', items: this.sfSteps() };
      if (f === 'stress' && r) return { type: 'ramp', title: 'Напряжение в материале, МПа', min: 0, max: this.fieldRange().max, colors: BLUE_RAMP };
      if (f === 'disp' && r) return { type: 'ramp', title: 'Перемещение, мм', min: 0, max: this.fieldRange().max, colors: BLUE_RAMP };
      if (f === 'mode' && r) return { type: 'cats', title: 'Что разрушится первым', items: MODE_GROUPS.concat([{ name: 'Запас больше ' + fmt(2 * this.target, 1), color: this.theme.neutral }]) };
      if (f === 'structure') return { type: 'cats', title: 'Структура печати', items: ROLE_GROUPS };
      if (f === 'density') return { type: 'ramp', title: 'Заполненность вокселя', min: 0, max: 100, unit: '%', colors: BLUE_RAMP };
      return null;
    }

    recolor() {
      const M = this.model;
      if (!M || !this.mesh) return;
      const C = this.colArr;
      const fn = this._elemColorFn();
      const cutTint = this.theme.dark ? 0.82 : 0.9;
      const ov = this.overlay;
      for (let f = 0; f < this.nFaces; f++) {
        const e = this.faceElem[f];
        let c = fn(e);
        if (this.faceCut[f]) c = [c[0] * cutTint, c[1] * cutTint, c[2] * cutTint];
        else if (ov.length) {
          const key = M.flat[e] * 6 + this.faceDir[f];
          for (let i = ov.length - 1; i >= 0; i--) {
            if (ov[i].keys.has(key)) { c = lerp3(c, ov[i].color, ov[i].alpha); break; }
          }
        }
        for (let q = 0; q < 4; q++) { C[12 * f + 3 * q] = c[0]; C[12 * f + 3 * q + 1] = c[1]; C[12 * f + 3 * q + 2] = c[2]; }
      }
      this.mesh.geometry.attributes.color.needsUpdate = true;
      this.requestRender();
    }

    // ---------------- вспомогательная графика
    _buildHelpers() {
      const M = this.model;
      this.helpers.clear();
      if (!M) return;
      const L = Math.max(M.size[0], M.size[1]) * 1.6 + 20;
      const step = L > 300 ? 50 : L > 120 ? 10 : 5;
      const half = Math.ceil(L / 2 / step) * step;
      const g = new THREE.GridHelper(2 * half, 2 * half / step, new THREE.Color(this.theme.grid), new THREE.Color(this.theme.grid2));
      g.rotation.x = Math.PI / 2;
      g.position.set(M.size[0] / 2, M.size[1] / 2, -0.01);
      this.helpers.add(g);
      // оси
      const axLen = Math.max(8, Math.min(30, 0.25 * Math.max.apply(null, M.size)));
      const mk = (dir, col) => new THREE.ArrowHelper(new THREE.Vector3().fromArray(dir), new THREE.Vector3(0, 0, 0), axLen, col, axLen * 0.18, axLen * 0.09);
      this.helpers.add(mk([1, 0, 0], 0xd03b3b), mk([0, 1, 0], 0x0ca30c), mk([0, 0, 1], 0x2a78d6));
      this.requestRender();
    }

    _buildMarkers() {
      this.markers.clear();
      const M = this.model;
      if (!M || !this.markerSpec) { this.requestRender(); return; }
      const L = Math.max.apply(null, M.size);
      const U = this._nodeDisp();
      for (const mk of this.markerSpec) {
        if (!mk.vector) continue;
        const keys = mk.keys instanceof Set ? mk.keys : new Set(mk.keys);
        // центр выбранных граней
        let sx = 0, sy = 0, sz = 0, cnt = 0, nx = 0, ny = 0, nz = 0;
        for (let e = 0; e < M.n; e++) {
          const base = M.flat[e] * 6;
          for (let d = 0; d < 6; d++) {
            if (!keys.has(base + d)) continue;
            const fn = FACE_NODES[d];
            for (let q = 0; q < 4; q++) {
              const node = M.corners[8 * e + fn[q]];
              let x = M.npos[3 * node], y = M.npos[3 * node + 1], z = M.npos[3 * node + 2];
              if (U) { x += this.deform * U[3 * node]; y += this.deform * U[3 * node + 1]; z += this.deform * U[3 * node + 2]; }
              sx += x; sy += y; sz += z; cnt++;
            }
            nx += DIRS[d][0]; ny += DIRS[d][1]; nz += DIRS[d][2];
          }
        }
        if (!cnt) continue;
        const c = new THREE.Vector3(sx / cnt, sy / cnt, sz / cnt);
        const v = new THREE.Vector3().fromArray(mk.vector);
        if (v.length() < 1e-12) continue;
        v.normalize();
        const len = Math.max(6, 0.22 * L);
        // стрелка приходит в точку приложения
        const from = c.clone().addScaledVector(v, -len);
        const a = new THREE.ArrowHelper(v, from, len, new THREE.Color(mk.color || this.theme.load), len * 0.22, len * 0.11);
        a.renderOrder = 10;
        a.traverse((o) => { if (o.material) { o.material.depthTest = false; o.material.transparent = true; } });
        this.markers.add(a);
      }
      this.requestRender();
    }

    // отметка слабого места (координаты детали)
    setCritical(xyz) {
      if (this._crit) { this.scene.remove(this._crit); this._crit = null; }
      if (xyz && this.model) {
        const L = Math.max.apply(null, this.model.size);
        const r = Math.max(0.8, 0.018 * L);
        const g = new THREE.Group();
        const mat1 = new THREE.MeshBasicMaterial({ color: 0x16181b, depthTest: false, transparent: true, opacity: 0.95 });
        const mat2 = new THREE.MeshBasicMaterial({ color: 0xffffff, depthTest: false, transparent: true, opacity: 0.95 });
        const s1 = new THREE.Mesh(new THREE.SphereGeometry(r, 20, 14), mat1);
        const s2 = new THREE.Mesh(new THREE.SphereGeometry(r * 0.55, 16, 10), mat2);
        s1.renderOrder = 20; s2.renderOrder = 21;
        g.add(s1, s2);
        g.position.fromArray(xyz);
        this._crit = g;
        this.scene.add(g);
      }
      this.requestRender();
    }

    view(name) {
      const M = this.model;
      if (!M) return;
      const s = M.size;
      const c = new THREE.Vector3(s[0] / 2, s[1] / 2, s[2] / 2);
      const R = Math.hypot(s[0], s[1], s[2]) / 2;
      const dist = R / Math.sin(THREE.MathUtils.degToRad(this.camera.fov / 2)) * 1.12 / Math.min(1, this.camera.aspect * 1.1);
      const ct = this.controls;
      ct.target.copy(c); ct.radius = dist;
      if (name === 'top') { ct.theta = -Math.PI / 2; ct.phi = 0.02; }
      else if (name === 'front') { ct.theta = -Math.PI / 2; ct.phi = Math.PI / 2; }
      else if (name === 'right') { ct.theta = 0; ct.phi = Math.PI / 2; }
      else if (name === 'iso') { ct.theta = -Math.PI / 3; ct.phi = 1.0; }
      ct.update();
    }

    // ---------------- выбор граней
    _ray(e) {
      const r = this.renderer.domElement.getBoundingClientRect();
      const p = new THREE.Vector2(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
      this.raycaster.setFromCamera(p, this.camera);
      if (!this.mesh) return null;
      const hit = this.raycaster.intersectObject(this.mesh, false)[0];
      if (!hit) return null;
      const f = Math.floor(hit.faceIndex / 2);
      const el = this.faceElem[f], d = this.faceDir[f];
      return { face: f, elem: el, dir: d, cut: !!this.faceCut[f], key: this.model.flat[el] * 6 + d, point: hit.point.toArray() };
    }
    _click(e, mod) {
      const h = this._ray(e);
      if (this.onPick) this.onPick(h, mod);
    }
    _hover(e) {
      if (e.buttons) { this.tip.hidden = true; return; }
      if (this._hoverPending) { this._hoverEvt = e; return; }
      this._hoverPending = true; this._hoverEvt = e;
      requestAnimationFrame(() => {
        this._hoverPending = false;
        const ev = this._hoverEvt;
        const h = this._ray(ev);
        if (!h) { this.tip.hidden = true; return; }
        const text = this.describe(h);
        const r = this.el.getBoundingClientRect();
        this.tip.innerHTML = text;
        this.tip.hidden = false;
        const x = ev.clientX - r.left, y = ev.clientY - r.top;
        this.tip.style.left = Math.min(r.width - this.tip.offsetWidth - 8, x + 14) + 'px';
        this.tip.style.top = Math.max(8, y - this.tip.offsetHeight - 10) + 'px';
        if (this.onHover) this.onHover(h);
      });
    }
    describe(h) {
      const M = this.model, e = h.elem;
      const c = [M.center[3 * e], M.center[3 * e + 1], M.center[3 * e + 2]];
      let s = '<span class="vw-tip-xyz">x ' + fmt(c[0], 1) + ' · y ' + fmt(c[1], 1) + ' · z ' + fmt(c[2], 1) + ' мм</span>';
      const r = this.results[this.caseIdx];
      if (r && ['sf', 'stress', 'disp', 'mode'].indexOf(this.field) >= 0) {
        s += '<b>запас ' + fmt(Math.min(r.sf[e], 999)) + '</b> · ' + fmt(r.vm[e]) + ' МПа';
        s += '<br><span class="vw-tip-mode">' + (global.FDM_MODES ? global.FDM_MODES[r.mode[e]] : '') + '</span>';
      } else {
        s += ROLE_GROUPS[M.role[e]].name + ' · ' + Math.round(M.rho[e] / 2.55) + '%';
      }
      return s;
    }

    // соседняя граничная грань при обходе поверхности через ребро в направлении t
    stepFace(e, d, t) {
      const M = this.model;
      const e1 = M.nbr[6 * e + t];
      if (e1 >= 0) {
        const e2 = M.nbr[6 * e1 + d];
        if (e2 >= 0) return [e2, OPP[t]];       // вогнутый угол
        return [e1, d];                          // плоско
      }
      return [e, t];                             // выпуклый угол
    }
    isBoundary(e, d) { return this.model.nbr[6 * e + d] < 0; }

    // заливка: plane — копланарные грани; axis — поверхность, перпендикулярная оси (отверстия, бобышки)
    floodFaces(e0, d0, mode, axis) {
      const M = this.model;
      const out = new Set();
      const seen = new Set();
      const key = (e, d) => M.flat[e] * 6 + d;
      const stack = [[e0, d0]];
      const limit = 400000;
      const ax0 = d0 >> 1;
      while (stack.length && out.size < limit) {
        const [e, d] = stack.pop();
        const k = key(e, d);
        if (seen.has(k)) continue;
        seen.add(k);
        if (!this.isBoundary(e, d)) continue;
        if (mode === 'plane' && d !== d0) continue;
        if (mode === 'axis' && (d >> 1) === axis) continue;
        out.add(k);
        const a = d >> 1;
        for (let t = 0; t < 6; t++) {
          if ((t >> 1) === a) continue;
          const [e2, d2] = mode === 'plane' ? (M.nbr[6 * e + t] >= 0 ? [M.nbr[6 * e + t], d] : [-1, -1]) : this.stepFace(e, d, t);
          if (e2 < 0) continue;
          if (mode === 'plane') {
            // копланарность: та же плоскость (одинаковая координата по оси нормали)
            const same = a === 0 ? M.ix[e2] === M.ix[e0] : a === 1 ? M.iy[e2] === M.iy[e0] : M.iz[e2] === M.iz[e0];
            if (!same) continue;
          }
          if (!seen.has(key(e2, d2))) stack.push([e2, d2]);
        }
      }
      return out;
    }
    brushFaces(point, radius) {
      const M = this.model;
      const out = new Set();
      const r2 = radius * radius;
      for (let e = 0; e < M.n; e++) {
        const dx = M.center[3 * e] - point[0], dy = M.center[3 * e + 1] - point[1], dz = M.center[3 * e + 2] - point[2];
        if (dx * dx + dy * dy + dz * dz > r2 * 1.5 + 4) continue;
        for (let d = 0; d < 6; d++) {
          if (M.nbr[6 * e + d] >= 0) continue;
          const D = DIRS[d];
          const hx = (D[0] * M.g.sx) / 2, hy = (D[1] * M.g.sy) / 2;
          const hz = D[2] * (M.g.z_edges[M.iz[e] + 1] - M.g.z_edges[M.iz[e]]) / 2;
          const fx = dx + hx, fy = dy + hy, fz = dz + hz;
          if (fx * fx + fy * fy + fz * fz <= r2) out.add(M.flat[e] * 6 + d);
        }
      }
      return out;
    }
    sideFaces(side) {
      // side: 0..5 (как направления граней)
      const M = this.model;
      const a = side >> 1, sg = side & 1 ? 1 : -1;
      let ext = -Infinity;
      for (let e = 0; e < M.n; e++) if (M.nbr[6 * e + side] < 0) ext = Math.max(ext, sg * M.center[3 * e + a]);
      const out = new Set();
      const tol = (a === 2 ? 0.6 : 0.6) * (a === 0 ? M.g.sx : a === 1 ? M.g.sy : 1);
      for (let e = 0; e < M.n; e++) {
        if (M.nbr[6 * e + side] < 0 && sg * M.center[3 * e + a] >= ext - tol) out.add(M.flat[e] * 6 + side);
      }
      return out;
    }
    facesArea(keys) {
      const M = this.model;
      let A = 0;
      const set = keys instanceof Set ? keys : new Set(keys);
      for (const k of set) {
        const fl = Math.floor(k / 6), d = k - fl * 6;
        const iz = Math.floor(fl / (M.g.nx * M.g.ny));
        const dz = M.g.z_edges[iz + 1] - M.g.z_edges[iz];
        A += d < 2 ? M.g.sy * dz : d < 4 ? M.g.sx * dz : M.g.sx * M.g.sy;
      }
      return A;
    }
    screenshot() { this.renderer.render(this.scene, this.camera); return this.renderer.domElement.toDataURL('image/png'); }
  }

  global.FDMViewer = { VoxelViewer, fmt, MODE_GROUPS, ROLE_GROUPS, b64ToArray };
  global.FDM_MODES = [
    'Разрыв вдоль нити', 'Смятие вдоль нити', 'Отрыв соседних нитей (в слое)', 'Смятие поперёк нити',
    'Расслоение между слоями (отрыв по Z)', 'Смятие по Z', 'Межслойный сдвиг', 'Сдвиг в плоскости слоя',
  ];
})(window);
