/* Отчёт: распаковка данных и управление 3D-сценой. */
(function () {
  'use strict';
  const $ = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const fmt = (v, d) => FDMViewer.fmt(v, d);

  async function unpack(b64) {
    const bin = atob(b64);
    const u8 = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
    if (!('DecompressionStream' in window)) throw new Error('Браузер слишком старый для распаковки данных отчёта.');
    const ds = new DecompressionStream('gzip');
    const txt = await new Response(new Blob([u8]).stream().pipeThrough(ds)).text();
    return JSON.parse(txt);
  }

  function renderLegend(el, lg) {
    if (!lg) { el.hidden = true; return; }
    let h = '<h4>' + lg.title + '</h4>';
    if (lg.type === 'steps') {
      for (const s of lg.items) h += '<div class="row"><span class="sw" style="background:' + s.color + '"></span><span class="num">' + s.label + '</span></div>';
    } else if (lg.type === 'cats') {
      for (const s of lg.items) h += '<div class="row"><span class="sw" style="background:' + s.color + '"></span><span>' + s.name + '</span></div>';
    } else if (lg.type === 'ramp') {
      const cols = lg.colors.map((c) => 'rgb(' + c.map((v) => Math.round(v * 255)).join(',') + ')');
      h += '<div class="bar" style="background:linear-gradient(90deg,' + cols.join(',') + ')"></div>';
      const mx = lg.max;
      h += '<div class="ticks"><span>0</span><span>' + fmt(mx / 2) + '</span><span>' + fmt(mx) + (lg.unit || '') + '</span></div>';
    }
    el.innerHTML = h;
    el.hidden = false;
  }

  async function main() {
    const fb = $('#vfb');
    let data;
    try { data = await unpack(window.FDM_PAYLOAD); } catch (e) { fb.textContent = 'Не удалось открыть данные: ' + e.message; return; }
    let viewer;
    try {
      viewer = new FDMViewer.VoxelViewer($('#viewer'));
    } catch (e) { fb.textContent = 'Нет WebGL — 3D-просмотр недоступен в этом браузере. Сводка выше полная.'; return; }
    fb.remove();
    viewer.setModel(data.model);
    viewer.setResults(data.results.results, data.results.target_sf);
    const legend = $('#legend');
    let field = 'sf', caseIdx = 0;
    const deformOn = $('#deformOn'), deform = $('#deform'), deformVal = $('#deformVal');
    function applyDeform() {
      const auto = viewer.autoDeformScale();
      const k = Math.pow(10, (deform.value - 50) / 50);   // ×0.1 … ×10 от авто
      const s = deformOn.checked ? auto * k : 0;
      deformVal.textContent = deformOn.checked ? fmt(s, s >= 10 ? 0 : 1) : '—';
      viewer.setDeform(s);
    }
    function markers() {
      const r = data.results.results[caseIdx];
      if (!r) return;
      const ov = [], mk = [];
      const loads = r.summary.loads || [];
      let li = 0;
      for (const k of Object.keys(r.sel || {})) {
        if (k.startsWith('fix')) ov.push({ keys: r.sel[k], color: '#3b3f45', alpha: 0.6 });
        else {
          const ld = loads[li++];
          mk.push({ keys: r.sel[k], vector: ld && ld.vector ? ld.vector : null, color: '#e34948' });
        }
      }
      viewer.setOverlay(ov);
      viewer.setMarkers(mk);
    }
    function show() {
      viewer.setField(field, caseIdx);
      const rs = data.results.results[caseIdx];
      viewer.setCritical(rs && ['sf', 'mode', 'stress'].indexOf(field) >= 0 ? rs.summary.sf_xyz : null);
      markers();
      applyDeform();
      renderLegend(legend, viewer.legend());
    }
    $$('#fields button').forEach((b) => b.addEventListener('click', () => {
      field = b.dataset.f;
      $$('#fields button').forEach((x) => x.classList.toggle('on', x === b));
      show();
    }));
    $$('.case').forEach((c, i) => c.addEventListener('click', () => {
      caseIdx = i;
      $$('.case').forEach((x) => { x.classList.toggle('on', x === c); x.setAttribute('aria-pressed', x === c ? 'true' : 'false'); });
      viewer.setCase(i);
      show();
      if (window.innerWidth < 760) $('#viewer').scrollIntoView({ behavior: 'smooth', block: 'center' });
    }));
    deformOn.addEventListener('change', applyDeform);
    deform.addEventListener('input', applyDeform);
    const secAxis = $('#secAxis'), secPos = $('#secPos');
    function applySection() {
      const a = secAxis.value;
      secPos.disabled = a === '';
      if (a === '') { viewer.setSection(null); return; }
      const ax = +a;
      viewer.setSection(ax, (secPos.value / 1000) * data.model.size[ax]);
    }
    secAxis.addEventListener('change', applySection);
    let secRaf = 0;
    secPos.addEventListener('input', () => { if (!secRaf) secRaf = requestAnimationFrame(() => { secRaf = 0; applySection(); }); });
    $$('[data-v]').forEach((b) => b.addEventListener('click', () => viewer.view(b.dataset.v)));
    show();
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', main); else main();
})();
