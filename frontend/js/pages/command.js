/* Command Center: race overview. Every figure comes from the exported bundle. */
(function (BB) {
  'use strict';

  const esc = BB.esc, model = BB.model, ui = BB.ui, charts = BB.charts;

  function mount(root, controls) {
    const store = BB.store;
    const data = model.data();
    const N = model.laps();

    root.innerHTML =
      '<div class="tiles" id="cc-tiles"></div>' +
      '<div class="grid">' +
        ui.panel({ id: 'cc-standings', span: 5, title: 'Standings', algo: 'Leaderboard.cpp · min-heap',
                   algoTip: 'Running order comes from RaceSimulator::standings(), which drains a hand-written binary min-heap (Leaderboard). O(D log D) per lap.' }) +
        ui.panel({ id: 'cc-compounds', span: 3, title: 'Compounds', algo: 'DriverAnalytics.cpp · hash map',
                   algoTip: 'Laps per compound, summed from per-car summaries that DriverAnalytics keys by car number in a hash map.' }) +
        ui.panel({ id: 'cc-pit', span: 4, title: 'Pit-stop distribution', algo: 'PitAnalytics.cpp · prefix sums',
                   algoTip: 'Stops per lap bucket, answered in O(1) each from StopLapIndex, a prefix-sum table over stops per lap.' }) +
        ui.panel({ id: 'cc-strategy', span: 6, title: 'Strategy overview', algo: 'PitStrategy.cpp · DP',
                   algoTip: 'The optimal stint plan from PitStrategy::optimize(): dynamic programming over a prefix-sum stint-cost table, then backtracking.' }) +
        ui.panel({ id: 'cc-trend', span: 6, title: 'Lap-time trend', algo: 'LapRecorder · lap records',
                   algoTip: 'Per-lap field mean and quickest lap, taken from the LapRecords the simulator produced. Lap times include traffic and exclude pit loss.' }) +
      '</div>';

    function renderControls() {
      controls.innerHTML = ui.segmented('run', [
        { value: 'overtakeOff', label: 'OFF' },
        { value: 'overtakeOn', label: 'ON' }
      ], store.get().run, 'Overtake rule');
    }

    function renderTiles() {
      const o = model.run().overview;
      const fl = o.fastestLap;
      document.getElementById('cc-tiles').innerHTML =
        ui.tile('Race distance', N + ' laps', data.track.name, 'var(--blue)') +
        ui.tile('Pit loss', BB.fmtSec(data.meta.pitLoss, 1) + ' s', 'per stop', 'var(--yellow)') +
        ui.tile('Fastest lap', fl ? BB.fmtClock(fl.time) : '–',
                fl ? '#' + fl.car + ' ' + model.carName(model.run(), fl.car) + ' · lap ' + fl.lap : '', 'var(--purple)') +
        ui.tile('Average lap', BB.fmtClock(o.averageLap), 'field mean, green flag', 'var(--green)') +
        ui.tile('Total stops', String(o.totalStops), 'across ' + model.run().drivers.length + ' cars', 'var(--pink)');
    }

    function renderStandings() {
      const s = store.get(), run = model.run();
      const row = run.laps[s.lap - 1];
      const showTraffic = run.overtake.enabled;
      let html = '<table class="table"><thead><tr><th>P</th><th>Car</th><th>Tyre</th>' +
        '<th class="num">Gap</th><th class="num">Last lap</th>' + (showTraffic ? '<th class="num">Traffic</th>' : '') +
        '</tr></thead><tbody>';
      row.order.forEach(function (e) {
        html += '<tr class="pick' + (s.car === e.car ? ' sel' : '') + '" data-car="' + e.car + '" tabindex="0" ' +
          'title="Click to focus this car in the lap-time trend">' +
          '<td class="num">' + e.position + '</td>' +
          '<td><span class="swatch" style="background:' + model.carColor(e.car) + '"></span>' +
            '<span class="carno">' + e.car + '</span> ' + esc(model.carName(run, e.car)) + '</td>' +
          '<td><span class="swatch" style="background:' + model.compoundColor(e.fitted) + '"></span>' +
            esc(model.compoundName(e.fitted)) + ' <span class="chip">' + e.fittedAge + ' laps</span>' +
            (e.pitted ? ' <span class="chip pit">pit</span>' : '') + '</td>' +
          '<td class="num">' + (e.position === 1 ? 'LEADER' : BB.fmtGap(e.gap)) + '</td>' +
          '<td class="num">' + BB.fmtClock(e.lapTime) + '</td>' +
          (showTraffic ? '<td class="num">' + BB.fmtSec(e.traffic, 1) + ' s</td>' : '') +
          '</tr>';
      });
      html += '</tbody></table>' +
        '<p class="seg-l" style="margin-top:10px">After lap ' + s.lap + ' of ' + N +
        (s.lap === N ? ' · final classification' : '') + '</p>';
      ui.setBody(root, 'cc-standings', html);
    }

    function renderCompounds() {
      const o = model.run().overview;
      let total = 0;
      const slices = o.compoundLaps.map(function (laps, i) {
        total += laps;
        return { label: model.compoundName(i), value: laps, color: model.compoundColor(i) };
      });
      const legend = slices.map(function (s) {
        return '<span><i class="swatch" style="background:' + s.color + '"></i>' + esc(s.label) + '<b>' + s.value + '</b></span>';
      }).join('');
      ui.setBody(root, 'cc-compounds',
        charts.donut({ slices: slices, center: String(total), sub: 'car-laps', label: 'Laps run on each compound' }) +
        '<div class="legend" style="justify-content:center">' + legend + '</div>');
    }

    function renderPit() {
      const o = model.run().overview, lap = store.get().lap;
      const items = o.pitBuckets.map(function (b) {
        const label = b.from === b.to ? String(b.from) : b.from + '–' + b.to;
        return { label: label, value: b.stops, color: 'var(--blue)',
                 title: 'Laps ' + label + ': ' + b.stops + (b.stops === 1 ? ' stop' : ' stops'),
                 active: lap >= b.from && lap <= b.to };
      });
      ui.setBody(root, 'cc-pit',
        charts.bars({ items: items, integer: true, height: 210, label: 'Pit stops per lap range' }) +
        '<p class="seg-l" style="margin-top:6px">Stops per lap range, whole field. Bright bar = current lap.</p>');
    }

    function renderStrategy() {
      const opt = data.strategy.optimal, lap = store.get().lap;
      const stints = opt.stints.map(function (st) {
        return { laps: st.laps, color: model.compoundColor(st.compound),
                 label: st.name + ' ' + st.laps,
                 title: st.name + ': laps ' + st.startLap + '–' + (st.startLap + st.laps - 1) + ', ' + BB.fmtClock(st.time) };
      });
      const closure = data.race.closure;
      ui.setBody(root, 'cc-strategy',
        '<div style="margin-bottom:12px"><span class="chip opt">Optimal plan</span> ' +
          '<span class="chip ' + (closure.passed ? 'ok' : '') + '" title="The simulator replays this plan alone and must post the same total (FR-8)">' +
          'FR-8 closure ' + (closure.passed ? 'pass' : 'FAIL') + ' · Δ ' + closure.difference.toExponential(1) + ' s</span></div>' +
        charts.stintBar({ stints: stints, total: N, marker: lap }) +
        '<div class="kv">' +
          '<div><div class="kv-l">Total time</div><div class="kv-v">' + BB.fmtClock(opt.totalTime) + '</div></div>' +
          '<div><div class="kv-l">Stops</div><div class="kv-v">' + opt.stops + ' · lap ' + (opt.pitLaps.join(', ') || '–') + '</div></div>' +
          '<div><div class="kv-l">Green time</div><div class="kv-v">' + BB.fmtSec(opt.greenTime, 2) + ' s</div></div>' +
          '<div><div class="kv-l">Pit time</div><div class="kv-v">' + BB.fmtSec(opt.pitTime, 2) + ' s</div></div>' +
        '</div>');
    }

    function renderTrend() {
      const s = store.get(), run = model.run(), o = run.overview;
      const series = [
        { name: 'Field mean', color: 'var(--blue)', points: o.lapTrend.map(function (t) { return [t.lap, t.average]; }) },
        { name: 'Quickest lap', color: 'var(--green)', points: o.lapTrend.map(function (t) { return [t.lap, t.fastest]; }) }
      ];
      let legend = '<span><i class="swatch" style="background:var(--blue)"></i>Field mean</span>' +
                   '<span><i class="swatch" style="background:var(--green)"></i>Quickest lap</span>';
      if (s.car !== null) {
        series.push({
          name: '#' + s.car + ' ' + model.carName(run, s.car), color: model.carColor(s.car),
          points: run.laps.map(function (l) {
            const e = l.order.find(function (x) { return x.car === s.car; });
            return e ? [l.lap, e.lapTime] : null;
          })
        });
        legend += '<span><i class="swatch" style="background:' + model.carColor(s.car) + '"></i>#' + s.car + ' ' + esc(model.carName(run, s.car)) + '</span>';
      }
      ui.setBody(root, 'cc-trend',
        charts.lines({ series: series, marker: s.lap, xLabel: 'lap', label: 'Lap time by lap',
                       yFmt: function (v) { return BB.fmtClock(v); } }) +
        '<div class="legend">' + legend + (s.car === null ? '<span>Click a car in the standings to add its laps.</span>' : '') + '</div>');
    }

    function renderAll() { renderTiles(); renderStandings(); renderCompounds(); renderPit(); renderStrategy(); renderTrend(); }

    // --- events (delegated, so re-rendering never loses a handler) ---
    function onControlsClick(ev) {
      const b = ev.target.closest('[data-seg="run"]');
      if (b) { store.set({ run: b.getAttribute('data-value') }); }
    }
    function toggleCar(tr) {
      const car = Number(tr.getAttribute('data-car'));
      store.set({ car: store.get().car === car ? null : car });
    }
    function onStandingsClick(ev) {
      const tr = ev.target.closest('tr[data-car]');
      if (tr) { toggleCar(tr); }
    }
    function onStandingsKey(ev) {
      if (ev.key !== 'Enter' && ev.key !== ' ') { return; }
      const tr = ev.target.closest('tr[data-car]');
      if (tr) { ev.preventDefault(); toggleCar(tr); }
    }
    controls.addEventListener('click', onControlsClick);
    const standings = root.querySelector('#cc-standings');
    standings.addEventListener('click', onStandingsClick);
    standings.addEventListener('keydown', onStandingsKey);

    const unsubscribe = store.subscribe(function (changed) {
      if (changed.indexOf('run') >= 0) { renderControls(); renderAll(); return; }
      if (changed.indexOf('lap') >= 0) { renderStandings(); renderPit(); renderStrategy(); renderTrend(); return; }
      if (changed.indexOf('car') >= 0) {
        const focused = document.activeElement && document.activeElement.getAttribute && document.activeElement.getAttribute('data-car');
        renderStandings(); renderTrend();
        if (focused) {
          const again = root.querySelector('tr[data-car="' + focused + '"]');
          if (again) { again.focus(); }
        }
      }
    });

    renderControls();
    renderAll();

    return function unmount() {
      unsubscribe();
      controls.removeEventListener('click', onControlsClick);
    };
  }

  BB.pages = BB.pages || {};
  BB.pages.command = {
    title: 'Command Center',
    subtitle: 'Race overview, driven by the C++ engine’s own results',
    clock: true,
    mount: mount
  };
})(window.BB = window.BB || {});
