/* Drivers: lap-time comparison, rankings, top laps, modelled sector stats and range queries
   with the tree nodes they touch. Every figure is exported from the engine's analytics;
   the page selects, compares and draws. */
(function (BB) {
  'use strict';

  const esc = BB.esc, model = BB.model, ui = BB.ui, charts = BB.charts;

  const MAX_PICKED = 3;
  const METRICS = [
    { value: 'averageLap', label: 'Pace' },
    { value: 'bestLap', label: 'Best lap' },
    { value: 'lapRange', label: 'Range' },
    { value: 'meanAbsDeviation', label: 'Deviation' }
  ];

  function mount(root, controls) {
    const store = BB.store;
    const data = model.data();
    const N = model.laps();

    let sel = [];                 // car numbers on screen, oldest first
    let metric = 'averageLap';
    let topK = 5, topScope = 'field', topOrder = 'fastest';
    let rangeCar = 1, rangeFrom = 1, rangeTo = N;

    const run = function () { return model.run(); };
    const analytics = function () { return run().analytics; };
    const windows = function () { return run().overview.windows; };
    const driver = function (car) { return run().overview.drivers.find(function (d) { return d.car === car; }); };
    const lapTimesOf = function (car) { return windows().cars.find(function (c) { return c.car === car; }).lapTimes; };

    function initialSelection() {
      const cars = run().drivers.map(function (d) { return d.car; });
      const wanted = [];
      const focused = store.get().car;
      if (focused !== null && cars.indexOf(focused) >= 0) { wanted.push(focused); }
      [1, 3, 4].concat(cars).forEach(function (c) {
        if (cars.indexOf(c) >= 0 && wanted.indexOf(c) < 0 && wanted.length < MAX_PICKED) { wanted.push(c); }
      });
      return wanted;
    }

    root.innerHTML =
      '<div class="grid">' +
        ui.panel({ id: 'dr-pick', span: 12, title: 'Drivers', algo: 'DriverAnalytics.cpp · hash map',
                   algoTip: 'DriverAnalytics finds each car’s summary through a hash map from car number to slot. It is used only for lookup, never iterated, so bucket order cannot reach any output.' }) +
        ui.panel({ id: 'dr-laps', span: 8, title: 'Lap-time comparison', algo: 'LapRecorder · lap records',
                   algoTip: 'Each point is a LapRecord the simulator produced: the car’s own pace plus any time lost behind another car. Pit loss is not in it; ▲ marks the lap that ends in a stop.' }) +
        ui.panel({ id: 'dr-cards', span: 4, title: 'Summary', algo: 'DriverAnalytics.cpp · summaries',
                   algoTip: 'Per-car summary computed from the lap records: best and worst lap (earliest on ties), mean lap, range, and mean absolute deviation from the car’s own average.' }) +
        ui.panel({ id: 'dr-sectors', span: 6, title: 'Sector comparison', algo: 'DriverAnalytics.cpp · compareSectors',
                   algoTip: 'MODELLED, not measured: the simulator times whole laps, so each lap is split in proportion to the track’s sector times. The sectors add no information beyond the lap time.' }) +
        ui.panel({ id: 'dr-rank', span: 6, title: 'Pace and consistency ranking', algo: 'DriverAnalytics.cpp · rankDrivers',
                   algoTip: 'rankDrivers orders cars by a metric with explicit tie-breaks (equal values go to the lower car number), so the order is the same on every run.' }) +
        ui.panel({ id: 'dr-top', span: 6, title: 'Fastest and slowest laps', algo: 'DriverAnalytics.cpp · bounded heap',
                   algoTip: 'selectLaps keeps a hand-written binary heap of at most K laps with the worst of them at the root: O(R log K) over R laps, against O(R log R) to sort them all.' }) +
        ui.panel({ id: 'dr-range', span: 6, title: 'Range query over laps', algo: 'RaceLapAnalytics.cpp · segment tree + Fenwick',
                   algoTip: 'Total time and quickest lap over any laps come from a segment tree; time lost to traffic comes from a Fenwick tree. Both answer in O(log N). The nodes shown are the ones the query adds up.' }) +
      '</div>';

    function renderControls() {
      controls.innerHTML = ui.segmented('run', [{ value: 'overtakeOff', label: 'OFF' }, { value: 'overtakeOn', label: 'ON' }],
                                        store.get().run, 'Overtake rule');
    }

    // ------------------------------------------------------------ picker
    function renderPick() {
      const r = run();
      ui.setBody(root, 'dr-pick',
        '<div class="legend togs" style="margin:0">' + r.drivers.map(function (d) {
          const on = sel.indexOf(d.car) >= 0;
          return '<button type="button" class="tog' + (on ? ' on' : '') + '" data-pick="' + d.car + '" aria-pressed="' + on + '">' +
            '<i class="swatch" style="background:' + model.carColor(d.car) + '"></i>#' + d.car + ' ' + esc(d.name) + '</button>';
        }).join('') + '<span class="muted">Pick up to ' + MAX_PICKED + ' cars to compare. A fourth replaces the oldest.</span></div>');
    }

    function pick(car) {
      const i = sel.indexOf(car);
      if (i >= 0) { if (sel.length > 1) { sel.splice(i, 1); } }
      else { sel.push(car); if (sel.length > MAX_PICKED) { sel.shift(); } }
      renderAllPanels();
    }

    // ------------------------------------------------------------ lap times
    function renderLaps() {
      const r = run();
      const series = sel.map(function (car) {
        const times = lapTimesOf(car);
        const marks = [];
        r.laps.forEach(function (l) {
          const e = l.order.find(function (x) { return x.car === car; });
          if (e && e.pitted) { marks.push([l.lap, times[l.lap - 1], 'Lap ' + l.lap + ': pit stop at the end of the lap', 'var(--blue)']); }
        });
        return { name: '#' + car + ' ' + model.carName(r, car), color: model.carColor(car), marks: marks,
                 points: times.map(function (t, i) { return [i + 1, t]; }) };
      });
      ui.setBody(root, 'dr-laps',
        charts.lines({ series: series, xLabel: 'lap', label: 'Lap time by lap', height: 400, yFmt: function (v) { return BB.fmtClock(v); } }) +
        '<div class="legend">' + series.map(function (s) {
          return '<span><i class="swatch" style="background:' + s.color + '"></i>' + esc(s.name) + '</span>';
        }).join('') + '<span><i class="swatch" style="background:var(--blue);border-radius:2px"></i>▲ pit stop</span></div>');
    }

    // ------------------------------------------------------------ summary cards
    function renderCards() {
      ui.setBody(root, 'dr-cards', sel.map(function (car) {
        const d = driver(car);
        return '<div class="card" style="border-left-color:' + model.carColor(car) + '">' +
          '<div class="card-h"><span class="carno">#' + car + '</span> ' + esc(d.name) + ' <span class="chip">' + d.stops + (d.stops === 1 ? ' stop' : ' stops') + '</span></div>' +
          '<div class="kv" style="margin-top:8px;grid-template-columns:repeat(2,1fr)">' +
            '<div><div class="kv-l">Best lap</div><div class="kv-v" style="font-size:13px">' + BB.fmtClock(d.bestLap.time) + ' <span class="muted">L' + d.bestLap.lap + '</span></div></div>' +
            '<div><div class="kv-l">Worst lap</div><div class="kv-v" style="font-size:13px">' + BB.fmtClock(d.worstLap.time) + ' <span class="muted">L' + d.worstLap.lap + '</span></div></div>' +
            '<div><div class="kv-l">Mean lap</div><div class="kv-v" style="font-size:13px">' + BB.fmtClock(d.averageLap) + '</div></div>' +
            '<div><div class="kv-l">Race time</div><div class="kv-v" style="font-size:13px">' + BB.fmtClock(d.totalTime) + '</div></div>' +
            '<div><div class="kv-l">Lap range</div><div class="kv-v" style="font-size:13px">' + BB.fmtSec(d.lapRange, 3) + ' s</div></div>' +
            '<div><div class="kv-l">Deviation</div><div class="kv-v" style="font-size:13px">' + BB.fmtSec(d.meanAbsDeviation, 3) + ' s</div></div>' +
            '<div style="grid-column:span 2"><div class="kv-l">Lost to traffic</div><div class="kv-v" style="font-size:13px">' + BB.fmtSec(d.trafficTime, 2) + ' s</div></div>' +
          '</div></div>';
      }).join('') + '<p class="muted" style="margin-top:8px;font-size:12px">Lap range is worst minus best lap; deviation is how far a lap typically strays from the car’s own average. Lower is steadier.</p>');
    }

    // ------------------------------------------------------------ sectors
    function renderSectors() {
      const s = analytics().sectors;
      if (s === null) { ui.setBody(root, 'dr-sectors', '<div class="empty">This track has no sectors.</div>'); return; }
      const ref = sel[0];
      const carData = function (car) { return s.cars.find(function (c) { return c.car === car; }); };
      const pair = function (a, b) { return s.pairs.find(function (p) { return p.a === a && p.b === b; }); };
      const others = sel.slice(1);

      let head = '<tr><th>Sector</th><th class="num"><span class="swatch" style="background:' + model.carColor(ref) + '"></span>#' + ref + ' avg</th>';
      others.forEach(function (c) {
        head += '<th class="num"><span class="swatch" style="background:' + model.carColor(c) + '"></span>#' + c + ' avg</th><th class="num">vs #' + ref + '</th>';
      });
      let body = '';
      s.names.forEach(function (name, k) {
        body += '<tr><td>' + esc(name) + '</td><td class="num">' + BB.fmtSec(carData(ref).average[k], 3) + '</td>';
        others.forEach(function (c) {
          const gap = pair(c, ref).rows[k].averageGap;     // other minus reference
          body += '<td class="num">' + BB.fmtSec(carData(c).average[k], 3) + '</td><td class="num ' + (gap > 0 ? 'red' : 'green') + '">' +
            (gap > 0 ? '+' : '') + BB.fmtSec(gap, 3) + '</td>';
        });
        body += '</tr>';
      });
      const fb = s.fieldBest;
      ui.setBody(root, 'dr-sectors',
        '<div style="margin-bottom:10px"><span class="chip hold">Modelled, not measured</span> ' +
          '<span class="muted" style="font-size:12px">A lap is split in proportion to the track’s sector times, so sector gaps are the lap gap shared out.</span></div>' +
        '<table class="table"><thead>' + head + '</tr></thead><tbody>' + body + '</tbody></table>' +
        '<p class="seg-l" style="margin:12px 0 4px">Fastest sector in the field</p><div class="legend" style="margin-top:0">' +
        fb.map(function (b, k) { return '<span>' + esc(s.names[k]) + ' <b>' + BB.fmtSec(b.time, 3) + '</b> #' + b.car + ' L' + b.lap + '</span>'; }).join('') +
        '</div>');
    }

    // ------------------------------------------------------------ ranking
    function metricValue(d) {
      if (metric === 'bestLap') { return BB.fmtClock(d.bestLap.time); }
      if (metric === 'averageLap') { return BB.fmtClock(d.averageLap); }
      return BB.fmtSec(d[metric], 3) + ' s';
    }
    function renderRank() {
      const order = analytics().rankings[metric];
      const info = { averageLap: 'Average lap time, quickest first.', bestLap: 'Single quickest lap.',
                     lapRange: 'Worst lap minus best lap; smaller is more consistent.',
                     meanAbsDeviation: 'Mean distance of a lap from the car’s own average; smaller is steadier.' };
      ui.setBody(root, 'dr-rank',
        '<div class="dp-bar" style="margin-bottom:10px">' + ui.segmented('metric', METRICS, metric, 'Rank by') + '</div>' +
        '<table class="table"><thead><tr><th>#</th><th>Car</th><th class="num">Value</th></tr></thead><tbody>' +
        order.map(function (car, i) {
          const d = driver(car);
          return '<tr class="pick' + (sel.indexOf(car) >= 0 ? ' sel' : '') + '" data-rankcar="' + car + '" tabindex="0" title="Click to add or remove this car">' +
            '<td class="num">' + (i + 1) + '</td><td><span class="swatch" style="background:' + model.carColor(car) + '"></span><span class="carno">' + car + '</span> ' + esc(d.name) +
            '</td><td class="num">' + metricValue(d) + '</td></tr>';
        }).join('') + '</tbody></table><p class="muted" style="margin-top:8px;font-size:12px">' + info[metric] + ' Equal values go to the lower car number.</p>');
    }

    // ------------------------------------------------------------ top laps
    function renderTop() {
      const a = analytics().topLaps, r = run();
      const set = topScope === 'field' ? a.field['k' + topK] : a.cars.find(function (c) { return c.car === Number(topScope); })['k' + topK];
      const rows = set[topOrder];
      ui.setBody(root, 'dr-top',
        '<div class="dp-bar" style="margin-bottom:10px">' +
          ui.segmented('topk', a.sizes.map(function (n) { return { value: String(n), label: 'Top ' + n }; }), String(topK), 'Laps') +
          ui.segmented('toporder', [{ value: 'fastest', label: 'Fastest' }, { value: 'slowest', label: 'Slowest' }], topOrder, 'Order') +
          '<label class="seg-l" for="dr-scope">Whose</label><select class="select" id="dr-scope" aria-label="Whose laps">' +
            '<option value="field"' + (topScope === 'field' ? ' selected' : '') + '>Whole field</option>' +
            r.drivers.map(function (d) { return '<option value="' + d.car + '"' + (String(d.car) === topScope ? ' selected' : '') + '>#' + d.car + ' ' + esc(d.name) + '</option>'; }).join('') +
          '</select></div>' +
        '<table class="table"><thead><tr><th>#</th><th>Car</th><th class="num">Lap</th><th class="num">Time</th><th>Tyre</th></tr></thead><tbody>' +
        rows.map(function (l, i) {
          return '<tr><td class="num">' + (i + 1) + '</td><td><span class="swatch" style="background:' + model.carColor(l.car) + '"></span><span class="carno">' + l.car + '</span> ' +
            esc(model.carName(r, l.car)) + '</td><td class="num">' + l.lap + '</td><td class="num">' + BB.fmtClock(l.time) +
            '</td><td><span class="swatch" style="background:' + model.compoundColor(l.compound) + '"></span>' + esc(model.compoundName(l.compound)) + '</td></tr>';
        }).join('') + '</tbody></table>');
    }

    // ------------------------------------------------------------ range query
    /** Bars for tree nodes under a lap axis, with the queried laps shaded. rows = [{label,color,nodes}] */
    function nodeStrip(rows, from, to) {
      const W = 620, rowH = 22, labelH = 15, gap = 10, top = 4;
      const H = top + rows.length * (labelH + rowH + gap) + 18;
      const x = function (lap) { return lap / N * W; };
      let s = '<svg class="chart" viewBox="0 0 ' + W + ' ' + H + '" role="img" aria-label="Tree nodes touched by the query">';
      s += '<rect x="' + x(from - 1).toFixed(1) + '" y="0" width="' + (x(to) - x(from - 1)).toFixed(1) + '" height="' + (H - 16) + '" fill="var(--text)" opacity=".06"/>';
      rows.forEach(function (row, i) {
        const y = top + i * (labelH + rowH + gap);
        s += '<text x="0" y="' + (y + 10) + '">' + esc(row.label) + '</text>';
        row.nodes.forEach(function (n) {
          const x0 = x(n[1] - 1), w = Math.max(x(n[2]) - x0 - 1.5, 2);
          s += '<rect x="' + x0.toFixed(1) + '" y="' + (y + labelH) + '" width="' + w.toFixed(1) + '" height="' + rowH + '" rx="3" fill="' + row.color + '" opacity=".85">' +
               '<title>Node ' + n[0] + ': laps ' + n[1] + '–' + n[2] + '</title></rect>';
          if (w > 26) {
            s += '<text x="' + (x0 + w / 2).toFixed(1) + '" y="' + (y + labelH + 15) + '" text-anchor="middle" style="fill:#0b0b10;font-weight:700">' + n[0] + '</text>';
          }
        });
      });
      const axisY = H - 16;
      s += '<line class="axis-l" x1="0" x2="' + W + '" y1="' + axisY + '" y2="' + axisY + '"/>';
      for (let lap = 0; lap <= N; lap += 10) {
        s += '<text x="' + x(lap).toFixed(1) + '" y="' + (H - 3) + '" text-anchor="' + (lap === 0 ? 'start' : 'middle') + '">' + lap + '</text>';
      }
      return s + '</svg>';
    }

    function renderRange() {
      const st = data.structures, ra = analytics().ranges;
      if (st === null || ra === null) {
        ui.setBody(root, 'dr-range', '<div class="empty">Range-query answers are exported for races of up to 100 laps; this one has ' + N + '.</div>');
        return;
      }
      rangeFrom = Math.min(rangeFrom, N); rangeTo = Math.min(Math.max(rangeTo, rangeFrom), N);
      ui.setBody(root, 'dr-range',
        '<div class="dp-bar" style="margin-bottom:8px"><label class="seg-l" for="dr-rcar">Car</label><select class="select" id="dr-rcar" aria-label="Car for the range query">' +
          run().drivers.map(function (d) { return '<option value="' + d.car + '"' + (d.car === rangeCar ? ' selected' : '') + '>#' + d.car + ' ' + esc(d.name) + '</option>'; }).join('') +
        '</select></div>' +
        '<div class="rng"><label class="seg-l" for="dr-rf">From lap</label><input id="dr-rf" type="range" min="1" max="' + N + '" step="1" value="' + rangeFrom + '" aria-label="First lap">' +
          '<span class="slider-v" id="dr-rf-v" style="font-size:16px;min-width:40px">' + rangeFrom + '</span></div>' +
        '<div class="rng"><label class="seg-l" for="dr-rl">To lap</label><input id="dr-rl" type="range" min="1" max="' + N + '" step="1" value="' + rangeTo + '" aria-label="Last lap">' +
          '<span class="slider-v" id="dr-rl-v" style="font-size:16px;min-width:40px">' + rangeTo + '</span></div>' +
        '<div id="dr-range-out"></div>');
      renderRangeOut();
    }

    function renderRangeOut() {
      const st = data.structures, ra = analytics().ranges;
      const out = document.getElementById('dr-range-out');
      if (!out || st === null || ra === null) { return; }
      const car = ra.cars.find(function (c) { return c.car === rangeCar; });
      const b = rangeFrom - 1, e = rangeTo, i = e - b - 1;
      const times = lapTimesOf(rangeCar);
      const fastestLap = car.fastestLap[b][i];
      const segNodes = st.segmentNodes[b][i];
      const fenE = st.fenwickNodes[e], fenB = st.fenwickNodes[b];
      const traffic = car.traffic === null ? null : car.traffic[b][i];

      out.innerHTML =
        nodeStrip([
          { label: 'Segment tree: ' + segNodes.length + ' nodes added for the sum and the minimum', color: 'var(--blue)', nodes: segNodes },
          { label: 'Fenwick prefix(' + e + '): ' + fenE.length + ' nodes added', color: 'var(--green)', nodes: fenE },
          { label: 'Fenwick prefix(' + b + '): ' + fenB.length + ' nodes subtracted', color: 'var(--red)', nodes: fenB }
        ], rangeFrom, rangeTo) +
        '<div class="kv">' +
          '<div><div class="kv-l">Total time, laps ' + rangeFrom + '–' + rangeTo + '</div><div class="kv-v">' + BB.fmtSec(car.sum[b][i], 2) + ' s</div></div>' +
          '<div><div class="kv-l">Quickest lap</div><div class="kv-v">' + BB.fmtClock(times[fastestLap - 1]) + ' <span class="muted" style="font-size:12px">L' + fastestLap + '</span></div></div>' +
          '<div><div class="kv-l">Lost to traffic</div><div class="kv-v">' + (traffic === null ? '0.00 s' : BB.fmtSec(traffic, 2) + ' s') + '</div></div>' +
        '</div>' +
        (traffic === null ? '<p class="muted" style="margin-top:8px;font-size:12px">Overtaking is off, so no car loses time behind another; switch the rule on to see traffic loss.</p>' : '');
    }

    function renderAllPanels() { renderPick(); renderLaps(); renderCards(); renderSectors(); renderRank(); renderTop(); renderRange(); }

    // ------------------------------------------------------------ events
    function onControlsClick(ev) {
      const r = ev.target.closest('[data-seg="run"]');
      if (r) { store.set({ run: r.getAttribute('data-value') }); }
    }
    function onClick(ev) {
      const p = ev.target.closest('[data-pick]');
      if (p) { pick(Number(p.getAttribute('data-pick'))); return; }
      const rk = ev.target.closest('tr[data-rankcar]');
      if (rk) { pick(Number(rk.getAttribute('data-rankcar'))); return; }
      const m = ev.target.closest('[data-seg="metric"]');
      if (m) { metric = m.getAttribute('data-value'); renderRank(); return; }
      const k = ev.target.closest('[data-seg="topk"]');
      if (k) { topK = Number(k.getAttribute('data-value')); renderTop(); return; }
      const o = ev.target.closest('[data-seg="toporder"]');
      if (o) { topOrder = o.getAttribute('data-value'); renderTop(); }
    }
    function onKey(ev) {
      if (ev.key !== 'Enter' && ev.key !== ' ') { return; }
      const rk = ev.target.closest('tr[data-rankcar]');
      if (rk) { ev.preventDefault(); pick(Number(rk.getAttribute('data-rankcar'))); }
    }
    function onChange(ev) {
      if (ev.target.id === 'dr-scope') { topScope = ev.target.value; renderTop(); }
      else if (ev.target.id === 'dr-rcar') { rangeCar = Number(ev.target.value); renderRangeOut(); }
    }
    function onInput(ev) {
      const id = ev.target.id;
      if (id !== 'dr-rf' && id !== 'dr-rl') { return; }
      if (id === 'dr-rf') { rangeFrom = Number(ev.target.value); if (rangeTo < rangeFrom) { rangeTo = rangeFrom; } }
      else { rangeTo = Number(ev.target.value); if (rangeFrom > rangeTo) { rangeFrom = rangeTo; } }
      document.getElementById('dr-rf').value = String(rangeFrom);
      document.getElementById('dr-rl').value = String(rangeTo);
      document.getElementById('dr-rf-v').textContent = String(rangeFrom);
      document.getElementById('dr-rl-v').textContent = String(rangeTo);
      renderRangeOut();
    }
    controls.addEventListener('click', onControlsClick);
    root.addEventListener('click', onClick);
    root.addEventListener('keydown', onKey);
    root.addEventListener('change', onChange);
    root.addEventListener('input', onInput);

    const unsubscribe = store.subscribe(function (changed) {
      if (changed.indexOf('run') >= 0) { renderControls(); renderAllPanels(); }
    });

    sel = initialSelection();
    rangeCar = sel[0];
    renderControls();
    renderAllPanels();

    return function unmount() {
      unsubscribe();
      controls.removeEventListener('click', onControlsClick);
      root.removeEventListener('click', onClick);
      root.removeEventListener('keydown', onKey);
      root.removeEventListener('change', onChange);
      root.removeEventListener('input', onInput);
    };
  }

  BB.pages = BB.pages || {};
  BB.pages.drivers = {
    title: 'Drivers',
    subtitle: 'Lap times, rankings, top laps and range queries, from the engine’s lap records',
    clock: false,
    mount: mount
  };
})(window.BB = window.BB || {});
