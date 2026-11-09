/* Tires: degradation curves, a bisection you can step through, range queries and sliding
   windows. Every number is exported from the engine's TireAnalytics and DriverAnalytics;
   the page selects, steps and draws. */
(function (BB) {
  'use strict';

  const esc = BB.esc, model = BB.model, ui = BB.ui, charts = BB.charts;

  const STEP_MS = 800;

  function mount(root, controls) {
    const store = BB.store;
    const data = model.data();
    const tires = data.tires;
    const profiles = tires.profiles;

    let sel = 0;                       // compound shown in the wear, search and range panels
    const hidden = {};                 // compounds switched off in the curves
    let limitIdx = Math.min(4, tires.limits.length - 1);   // bisection limit (index into tires.limits)
    let bsStep = 0;
    let playing = false;
    let timer = null;
    let range = { b: 0, e: 1 };        // ages [b, e) for the range query
    let winCar = 1;
    let winSize = 5;

    const prof = function () { return profiles[sel]; };
    const query = function () { return prof().thresholds[limitIdx]; };

    root.innerHTML =
      '<div class="grid">' +
        ui.panel({ id: 'ti-curves', span: 8, title: 'Lap time by tire age', algo: 'TireAnalytics.cpp · DegradationProfile',
                   algoTip: 'Each curve is the track’s base lap time plus the compound’s own pace model at every legal age (DegradationProfile::lapTime). The triangle marks the age where the cliff first costs time.' }) +
        ui.panel({ id: 'ti-compare', span: 4, title: 'Compare', algo: 'TireAnalytics.cpp · compareCompounds',
                   algoTip: 'compareCompounds scans two compounds age by age and reports the first age one is slower and every age the faster compound changes. A full scan: the gap changes slope at each cliff, so it cannot be bisected.' }) +
        ui.panel({ id: 'ti-wear', span: 6, title: 'Wear per lap', algo: 'TireAnalytics.cpp · incrementalDegradation',
                   algoTip: 'Lap-time change from one age to the next. A jump in this curve is the degradation cliff (DegradationProfile::incrementalDegradation, cliffStepSeconds).' }) +
        ui.panel({ id: 'ti-search', span: 6, title: 'Binary search: first age past a limit', algo: 'TireAnalytics.cpp · binary search',
                   algoTip: 'firstAgeDegradationExceeds bisects the degradation-versus-fresh array, which never falls, in O(log M). The steps shown replay that bisection and are checked against the core’s answer before export.' }) +
        ui.panel({ id: 'ti-range', span: 6, title: 'Range query over tire age', algo: 'TireAnalytics.cpp · SegmentTree',
                   algoTip: 'TireRangeAnalyzer answers sum, mean, minimum and degradation over any age range in O(log M) from a segment tree. Every range is precomputed, so each slider position is a real answer.' }) +
        ui.panel({ id: 'ti-window', span: 6, title: 'Sliding window over a car’s laps', algo: 'DriverAnalytics.cpp · monotonic deque',
                   algoTip: 'trailingWindows: the quickest and slowest lap in the last W laps come from two monotonic deques in O(1) amortised per lap; the mean comes from prefix sums.' }) +
      '</div>';

    // -------------------------------------------------------- header
    function renderControls() {
      controls.innerHTML =
        ui.segmented('compound', data.compounds.map(function (c, i) { return { value: String(i), label: c.name }; }), String(sel), 'Compound') +
        ui.segmented('run', [{ value: 'overtakeOff', label: 'OFF' }, { value: 'overtakeOn', label: 'ON' }], store.get().run, 'Overtake rule');
    }

    // -------------------------------------------------------- curves
    function renderCurves() {
      const series = [], legend = [];
      profiles.forEach(function (p, i) {
        const color = model.compoundColor(i);
        const off = !!hidden[i];
        legend.push('<button type="button" class="tog' + (off ? '' : ' on') + '" data-tog="' + i + '" aria-pressed="' + (!off) + '" title="Show or hide ' + esc(p.name) + '">' +
          '<i class="swatch" style="background:' + color + '"></i>' + esc(p.name) + '</button>');
        if (off) { return; }
        const marks = [];
        if (p.cliffActivationAge !== null) {
          marks.push([p.cliffActivationAge, p.lapTime[p.cliffActivationAge],
                      p.name + ' cliff: age ' + p.cliffActivationAge + ', +' + BB.fmtSec(p.cliffStepSeconds, 3) + ' s that lap']);
        }
        series.push({ name: p.name, color: color, marks: marks, points: p.lapTime.map(function (t, a) { return [a, t]; }) });
      });
      ui.setBody(root, 'ti-curves',
        '<div class="legend togs">' + legend.join('') + '<span class="muted">▲ = degradation cliff</span></div>' +
        (series.length ? charts.lines({ series: series, xLabel: 'tire age', label: 'Lap time by tire age', height: 280,
                                         yFmt: function (v) { return BB.fmtClock(v); } })
                       : '<div class="empty">Every compound is switched off.</div>'));
    }

    // -------------------------------------------------------- compare
    function renderCompare() {
      let html = '<table class="table"><thead><tr><th>Compound</th><th class="num">Fresh</th><th class="num">Last lap</th><th class="num">Cliff</th></tr></thead><tbody>';
      profiles.forEach(function (p, i) {
        const c = data.compounds[i];
        html += '<tr title="Base pace ' + BB.fmtSec(c.basePace, 2) + ' s, wear ' + BB.fmtSec(c.degradationPerLap, 3) + ' s/lap, max stint ' + p.maxStint + ' laps">' +
          '<td><span class="swatch" style="background:' + model.compoundColor(i) + '"></span>' + esc(p.name) + '</td>' +
          '<td class="num">' + BB.fmtClock(p.lapTime[0]) + '</td><td class="num">' + BB.fmtClock(p.lapTime[p.lapTime.length - 1]) + '</td>' +
          '<td class="num">' + (p.cliffActivationAge === null ? '–' : 'age ' + p.cliffActivationAge) + '</td></tr>';
      });
      html += '</tbody></table><div class="kv" style="grid-template-columns:repeat(auto-fit,minmax(88px,1fr))">' +
        profiles.map(function (p, i) {
          const c = data.compounds[i];
          return '<div><div class="kv-l">' + esc(p.name) + '</div><div class="kv-v" style="font-size:12.5px">' + p.maxStint + ' laps max<br>' +
            BB.fmtSec(c.degradationPerLap, 3) + ' s/lap</div></div>';
        }).join('') + '</div>';

      html += '<p class="seg-l" style="margin:16px 0 8px">Crossovers</p><ul class="feed">';
      tires.comparisons.forEach(function (cmp) {
        const a = profiles[cmp.a].name, b = profiles[cmp.b].name;
        html += '<li>' + (cmp.firstSlowerAge === null
          ? '<span><b>' + esc(a) + '</b> is never slower than <b>' + esc(b) + '</b> in the first ' + cmp.agesCompared + ' ages.</span>'
          : '<span><b>' + esc(a) + '</b> becomes slower than <b>' + esc(b) + '</b> from age <b>' + cmp.firstSlowerAge + '</b>' +
            (cmp.switchAges.length > 1 ? ' (leader changes at ages ' + cmp.switchAges.join(', ') + ')' : '') + '.</span>') + '</li>';
      });
      ui.setBody(root, 'ti-compare', html + '</ul>');
    }

    // -------------------------------------------------------- wear
    function renderWear() {
      const p = prof(), color = model.compoundColor(sel);
      const items = p.incremental.map(function (v, a) {
        const isCliff = a === p.cliffActivationAge;
        return { label: a % 5 === 0 ? String(a) : '', value: v, color: isCliff ? 'var(--yellow)' : color, active: true,
                 title: 'Age ' + a + ': +' + BB.fmtSec(v, 3) + ' s on the lap before' + (isCliff ? ' (cliff)' : '') };
      });
      ui.setBody(root, 'ti-wear',
        charts.bars({ items: items, width: 560, height: 210, label: 'Lap-time change per age, ' + p.name }) +
        '<p class="seg-l" style="margin:4px 0 0">Lap-time increase per lap of tire age, seconds. ' + esc(p.name) + '.</p>' +
        '<div class="kv">' +
          '<div><div class="kv-l">Cliff</div><div class="kv-v">' + (p.cliffActivationAge === null ? 'none' : 'age ' + p.cliffActivationAge) + '</div></div>' +
          '<div><div class="kv-l">Cliff step</div><div class="kv-v">' + (p.cliffStepSeconds === null ? '–' : '+' + BB.fmtSec(p.cliffStepSeconds, 3) + ' s') + '</div></div>' +
          '<div><div class="kv-l">Last lap vs before cliff</div><div class="kv-v">' + (p.lapTimeChangeAfterCliff === null ? '–' : '+' + BB.fmtSec(p.lapTimeChangeAfterCliff, 3) + ' s') + '</div></div>' +
          '<div><div class="kv-l">Total loss at max stint</div><div class="kv-v">' + BB.fmtSec(p.cumulative[p.cumulative.length - 1], 2) + ' s</div></div>' +
        '</div>');
    }

    // -------------------------------------------------------- binary search
    function searchState() {
      const p = prof(), q = query(), K = q.steps.length;
      return { p: p, q: q, K: K, step: Math.min(bsStep, K) };
    }

    function renderSearch() {
      const s = searchState(), p = s.p, q = s.q, limit = tires.limits[limitIdx];
      let lo = 0, hi = p.maxStint, mid = -1, exceeds = false;
      if (q.binarySearch && s.step > 0) {
        const row = q.steps[s.step - 1];
        lo = row[0]; hi = row[1]; mid = row[2]; exceeds = row[3] === 1;
      }
      const done = q.binarySearch && s.step === s.K;
      const answer = q.age;

      let cells = '';
      p.degradationVsFresh.forEach(function (v, a) {
        const inside = a >= lo && a < hi;
        const cls = 'bs-cell' + (inside ? ' in' : ' out') + (a === mid ? (exceeds ? ' mid over' : ' mid under') : '') +
          (done && answer === a ? ' answer' : '');
        cells += '<div class="' + cls + '" title="Age ' + a + ': ' + BB.fmtSec(v, 3) + ' s slower than a fresh tire"><span class="dp-i">' + a +
          '</span><span class="dp-v">' + BB.fmtSec(v, 2) + '</span></div>';
      });

      let info;
      if (!q.binarySearch) {
        info = 'This compound’s lap times are not non-decreasing, so the core falls back to a scan. ' +
          (answer === null ? 'No age exceeds the limit.' : 'First age past the limit: <b>' + answer + '</b>.');
      } else if (s.step === 0) {
        info = 'Search the ages <b>[' + lo + ', ' + hi + ')</b> for the first age whose loss against a fresh tire is more than <b>' + BB.fmtSec(limit, 2) + ' s</b>. The losses never fall, so the answer can be bisected.';
      } else {
        info = 'Step ' + s.step + ': try the middle, age <b>' + mid + '</b> (loss ' + BB.fmtSec(p.degradationVsFresh[mid], 3) + ' s). ' +
          (exceeds ? 'That is <b class="red">over</b> the limit, so the answer is at or before it: <span class="t">hi = ' + mid + '</span>.'
                   : 'That is <b class="green">within</b> the limit, so the answer is after it: <span class="t">lo = ' + (mid + 1) + '</span>.');
        if (done) {
          info += '<br><b>Result:</b> ' + (answer === null ? 'no age exceeds the limit.' : 'the first age past the limit is <b>' + answer + '</b>.');
        }
      }

      const label = !q.binarySearch ? 'Scan' : (s.step === 0 ? 'Ready' : 'Step ' + s.step + ' of ' + s.K);

      // The slider row is built once, so a drag is never interrupted by a re-render.
      const body = root.querySelector('#ti-search [data-body]');
      if (!body.querySelector('#ti-limit')) {
        body.innerHTML =
          '<div class="slider-row">' +
            '<label class="seg-l" for="ti-limit">Limit</label>' +
            '<input id="ti-limit" type="range" min="0" max="' + (tires.limits.length - 1) + '" step="1" value="' + limitIdx + '" aria-label="Degradation limit">' +
            '<div class="slider-v" id="ti-limit-v" style="min-width:80px;font-size:18px"></div>' +
          '</div><div id="ti-search-dyn"></div>';
      }
      document.getElementById('ti-limit-v').textContent = BB.fmtSec(limit, 2) + ' s';
      document.getElementById('ti-search-dyn').innerHTML =
        '<div class="dp-bar" style="margin-top:12px">' +
          '<div class="clock-btns">' +
            bsBtn('reset', 'Back to the start', '<path d="M6 5v14M18 5l-9 7 9 7z"/>') +
            bsBtn('back', 'Previous step', '<path d="M15 6l-6 6 6 6"/>') +
            bsBtn('play', playing ? 'Pause' : 'Play', playing ? '<path d="M8 5v14M16 5v14"/>' : '<path d="M8 5l11 7-11 7z"/>', true) +
            bsBtn('fwd', 'Next step', '<path d="M9 6l6 6-6 6"/>') +
            bsBtn('end', 'Skip to the result', '<path d="M18 5v14M6 5l9 7-9 7z"/>') +
          '</div>' +
          '<span class="chip ' + (done ? 'ok' : 'opt') + '">' + esc(label) + '</span>' +
        '</div>' +
        '<div class="bs-grid">' + cells + '</div>' +
        '<div class="dp-info" style="margin-top:12px"><div class="dp-form">' + info + '</div></div>';
    }

    function bsBtn(act, label, path, primary) {
      return '<button class="btn' + (primary ? ' primary' : '') + '" type="button" data-bs="' + act + '" title="' + label + '" aria-label="' + label + '">' +
        '<svg viewBox="0 0 24 24" aria-hidden="true">' + path + '</svg></button>';
    }

    function stopPlay() { if (timer !== null) { clearInterval(timer); timer = null; } playing = false; }
    function startPlay() {
      stopPlay();
      const K = query().steps.length;
      if (bsStep >= K) { bsStep = 0; }
      playing = true;
      timer = setInterval(function () {
        const last = query().steps.length;
        if (bsStep >= last) { stopPlay(); renderSearch(); return; }
        bsStep += 1;
        if (bsStep >= last) { stopPlay(); }
        renderSearch();
      }, STEP_MS);
      renderSearch();
    }

    // -------------------------------------------------------- range query
    function renderRange() {
      const p = prof(), m = p.maxStint;
      if (p.ranges === null) {
        ui.setBody(root, 'ti-range', '<div class="empty">Range answers are exported for stints of up to 80 ages; ' + esc(p.name) + ' allows ' + m + '.</div>');
        return;
      }
      range.b = Math.min(range.b, m - 1);
      range.e = Math.min(Math.max(range.e, range.b + 1), m);
      ui.setBody(root, 'ti-range',
        '<div class="rng"><label class="seg-l" for="ti-b">From age</label>' +
          '<input id="ti-b" type="range" min="0" max="' + (m - 1) + '" step="1" value="' + range.b + '" aria-label="First age">' +
          '<span class="slider-v" id="ti-b-v" style="font-size:16px;min-width:40px">' + range.b + '</span></div>' +
        '<div class="rng"><label class="seg-l" for="ti-e">To age (excl.)</label>' +
          '<input id="ti-e" type="range" min="1" max="' + m + '" step="1" value="' + range.e + '" aria-label="End age">' +
          '<span class="slider-v" id="ti-e-v" style="font-size:16px;min-width:40px">' + range.e + '</span></div>' +
        '<div id="ti-range-out"></div>');
      renderRangeOut();
    }

    function renderRangeOut() {
      const p = prof(), r = p.ranges, b = range.b, e = range.e, i = e - b - 1;
      const items = p.lapTime.map(function (t, a) {
        const inside = a >= b && a < e;
        return { label: a % 5 === 0 ? String(a) : '', value: t - Math.min.apply(null, p.lapTime) + 0.05, color: inside ? model.compoundColor(sel) : 'var(--muted)', active: inside,
                 title: 'Age ' + a + ': ' + BB.fmtClock(t) };
      });
      const out = document.getElementById('ti-range-out');
      if (!out) { return; }
      out.innerHTML =
        charts.bars({ items: items, width: 560, height: 150, label: 'Lap time by age with the chosen range highlighted' }) +
        '<p class="seg-l" style="margin:2px 0 0">Ages ' + b + ' to ' + (e - 1) + ' · ' + (e - b) + (e - b === 1 ? ' lap' : ' laps') + '. Height = lap time above the fastest.</p>' +
        '<div class="kv">' +
          '<div><div class="kv-l">Sum of lap times</div><div class="kv-v">' + BB.fmtSec(r.sum[b][i], 2) + ' s</div></div>' +
          '<div><div class="kv-l">Mean lap</div><div class="kv-v">' + BB.fmtClock(r.average[b][i]) + '</div></div>' +
          '<div><div class="kv-l">Fastest lap</div><div class="kv-v">' + BB.fmtClock(r.min[b][i]) + '</div></div>' +
          '<div><div class="kv-l">Lost to wear</div><div class="kv-v">' + BB.fmtSec(r.degradation[b][i], 2) + ' s</div></div>' +
        '</div>';
    }

    // -------------------------------------------------------- sliding window
    function renderWindow() {
      const run = model.run(), w = run.overview.windows;
      const cars = w.cars;
      if (!cars.some(function (c) { return c.car === winCar; })) { winCar = cars[0].car; }
      const car = cars.find(function (c) { return c.car === winCar; });
      const s = car['w' + winSize];
      const pts = function (arr) { return arr.map(function (v, i) { return [i + 1, v]; }); };
      const color = model.carColor(winCar);

      ui.setBody(root, 'ti-window',
        '<div class="dp-bar" style="margin-bottom:8px">' +
          '<label class="seg-l" for="ti-car">Car</label>' +
          '<select class="select" id="ti-car" aria-label="Car">' + cars.map(function (c) {
            return '<option value="' + c.car + '"' + (c.car === winCar ? ' selected' : '') + '>#' + c.car + ' ' + esc(model.carName(run, c.car)) + '</option>';
          }).join('') + '</select>' +
          ui.segmented('wsize', w.sizes.map(function (n) { return { value: String(n), label: n + ' laps' }; }), String(winSize), 'Window') +
        '</div>' +
        charts.lines({
          series: [
            { name: 'Lap time', color: color, points: pts(car.lapTimes) },
            { name: 'Window mean', color: 'var(--blue)', points: pts(s.average) }
          ],
          bands: [{ name: 'Quickest to slowest lap in the window', color: 'var(--muted)', upper: pts(s.slowest), lower: pts(s.fastest) }],
          xLabel: 'lap', label: 'Lap times with a sliding window', height: 240,
          yFmt: function (v) { return BB.fmtClock(v); }
        }) +
        '<div class="legend"><span><i class="swatch" style="background:' + color + '"></i>Lap time</span>' +
          '<span><i class="swatch" style="background:var(--blue)"></i>Mean of the last ' + winSize + ' laps</span>' +
          '<span><i class="swatch" style="background:var(--muted);opacity:.6"></i>Quickest to slowest in the window</span></div>');
    }

    function renderAll() { renderControls(); renderCurves(); renderCompare(); renderWear(); renderSearch(); renderRange(); renderWindow(); }

    // -------------------------------------------------------- events
    function onControlsClick(ev) {
      const c = ev.target.closest('[data-seg="compound"]');
      if (c) {
        sel = Number(c.getAttribute('data-value'));
        stopPlay(); bsStep = query().steps.length;
        renderControls(); renderWear(); renderSearch(); renderRange();
        return;
      }
      const r = ev.target.closest('[data-seg="run"]');
      if (r) { store.set({ run: r.getAttribute('data-value') }); }
    }

    function onClick(ev) {
      const tog = ev.target.closest('[data-tog]');
      if (tog) { const i = Number(tog.getAttribute('data-tog')); hidden[i] = !hidden[i]; renderCurves(); return; }

      const b = ev.target.closest('[data-bs]');
      if (b) {
        const act = b.getAttribute('data-bs'), K = query().steps.length;
        if (act === 'play') { if (playing) { stopPlay(); renderSearch(); } else { startPlay(); } return; }
        stopPlay();
        if (act === 'reset') { bsStep = 0; }
        else if (act === 'back') { bsStep = Math.max(0, Math.min(bsStep, K) - 1); }
        else if (act === 'fwd') { bsStep = Math.min(K, bsStep + 1); }
        else if (act === 'end') { bsStep = K; }
        renderSearch();
        return;
      }
      const ws = ev.target.closest('[data-seg="wsize"]');
      if (ws) { winSize = Number(ws.getAttribute('data-value')); renderWindow(); }
    }

    function onInput(ev) {
      const id = ev.target.id;
      if (id === 'ti-limit') {
        stopPlay();
        limitIdx = Number(ev.target.value);
        bsStep = query().steps.length;          // show the finished search for the new limit
        renderSearch();
        return;
      }
      if (id === 'ti-b' || id === 'ti-e') {
        const m = prof().maxStint;
        if (id === 'ti-b') { range.b = Number(ev.target.value); if (range.e <= range.b) { range.e = range.b + 1; } }
        else { range.e = Number(ev.target.value); if (range.b >= range.e) { range.b = range.e - 1; } }
        range.b = Math.max(0, Math.min(range.b, m - 1));
        document.getElementById('ti-b').value = String(range.b);
        document.getElementById('ti-e').value = String(range.e);
        document.getElementById('ti-b-v').textContent = String(range.b);
        document.getElementById('ti-e-v').textContent = String(range.e);
        renderRangeOut();
      }
    }

    function onChange(ev) {
      if (ev.target.id === 'ti-car') { winCar = Number(ev.target.value); renderWindow(); }
    }

    controls.addEventListener('click', onControlsClick);
    root.addEventListener('click', onClick);
    root.addEventListener('input', onInput);
    root.addEventListener('change', onChange);

    const unsubscribe = store.subscribe(function (changed) {
      if (changed.indexOf('run') >= 0) { renderControls(); renderWindow(); }
    });

    bsStep = query().steps.length;
    renderAll();

    return function unmount() {
      stopPlay();
      unsubscribe();
      controls.removeEventListener('click', onControlsClick);
      root.removeEventListener('click', onClick);
      root.removeEventListener('input', onInput);
      root.removeEventListener('change', onChange);
    };
  }

  BB.pages = BB.pages || {};
  BB.pages.tires = {
    title: 'Tires',
    subtitle: 'Degradation, crossovers, a bisection you can step through, and range and window queries',
    clock: false,
    mount: mount
  };
})(window.BB = window.BB || {});
