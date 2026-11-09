/* Pit & Events: pit timeline, stops per lap bucket, undercut/overcut, the event log, the
   event queue draining, and a greedy pit-call plan. Every figure comes from the engine's
   PitAnalytics, StrategyAnalytics and event log; the page selects and draws. */
(function (BB) {
  'use strict';

  const esc = BB.esc, model = BB.model, ui = BB.ui, charts = BB.charts;

  const PAGE_ROWS = 12;
  const QUEUE_MS = { 1: 400, 4: 100, 16: 30 };
  const TYPE_CHIP = {
    PitEntry: 'pit', CompoundChange: 'pit', PitExit: 'pit', LapCompleted: '', PositionChange: '',
    OvertakeAttempt: 'hold', Overtake: 'pass', TrafficHold: 'hold', RaceFinish: 'fin'
  };
  const TYPE_ORDER = ['PitEntry', 'CompoundChange', 'PitExit', 'LapCompleted', 'PositionChange',
                      'OvertakeAttempt', 'Overtake', 'TrafficHold', 'RaceFinish'];

  function mount(root, controls) {
    const store = BB.store;
    const data = model.data();
    const N = model.laps();

    let bucket = 5;                          // stops-per-bucket size
    let earlier = 17, later = 18;            // undercut laps
    let slack = 5;                           // greedy window width
    let typeOn = {};                         // event-type filter
    TYPE_ORDER.forEach(function (t) { typeOn[t] = (t !== 'LapCompleted' && t !== 'PositionChange'); });
    let logCar = 0, logPage = 0, uptoLap = false;
    let qStep = 0, qSpeed = 4, qPlaying = false, qTimer = null;

    const run = function () { return model.run(); };
    const pit = function () { return run().pit; };

    root.innerHTML =
      '<div class="grid">' +
        ui.panel({ id: 'pt-timeline', span: 12, title: 'Pit timeline', algo: 'RaceSimulator.cpp · stint plans',
                   algoTip: 'Each bar is a stint from the plan the simulator ran; the blue ticks are stops, logged at the end of a lap. The line is the shared race clock.' }) +
        ui.panel({ id: 'pt-hist', span: 4, title: 'Stops per lap', algo: 'PitAnalytics.cpp · prefix sums',
                   algoTip: 'StopLapIndex holds prefix sums of stops per lap, so the stops in any bucket of laps are one subtraction: O(1) each.' }) +
        ui.panel({ id: 'pt-summary', span: 4, title: 'Stops by car', algo: 'PitAnalytics.cpp · hash map',
                   algoTip: 'PitAnalytics reads the event log once, keying each car’s stops and stints through a hash map. The stints are checked to tile the race exactly.' }) +
        ui.panel({ id: 'pt-under', span: 4, title: 'Undercut or overcut', algo: 'StrategyAnalytics.cpp · DP',
                   algoTip: 'compareFirstStopLaps: the same car stops at the end of one lap or the other, then runs the best continuation, solved by the optimizer. Simplified model: clean air, no tire warm-up.' }) +
        ui.panel({ id: 'pt-log', span: 8, title: 'Event log', algo: 'EventLog.cpp · in queue order',
                   algoTip: 'Every event the simulator emitted, in the order the priority queue released them: time, then type, car number, and insertion order. Replayed through the pit-stop state machine and validated before export.' }) +
        ui.panel({ id: 'pt-queue', span: 4, title: 'Queue draining', algo: 'EventPriorityQueue.cpp · min-heap',
                   algoTip: 'Events go into a hand-written binary min-heap as the simulator produces them and come out in time order. Inversions are the pairs the heap had to put right, counted with a Fenwick tree (countOrderSwaps).' }) +
        ui.panel({ id: 'pt-greedy', span: 12, title: 'Fewest pit calls (greedy)', algo: 'PitAnalytics.cpp · interval stabbing',
                   algoTip: 'minimumStopLaps: sort the windows by latest lap and take a window’s latest lap whenever no chosen lap is inside it. Optimal by an exchange argument, and checked against brute force in the tests. The widths are a what-if, not a rule of the engine.' }) +
      '</div>';

    function renderControls() {
      controls.innerHTML = ui.segmented('run', [{ value: 'overtakeOff', label: 'OFF' }, { value: 'overtakeOn', label: 'ON' }],
                                        store.get().run, 'Overtake rule');
    }

    // ------------------------------------------------------------ timeline
    function renderTimeline() {
      const r = run(), lap = store.get().lap;
      const left = 130, W = 1000, rowH = 38, top = 8, right = 14;
      const x = function (l) { return left + l / N * (W - left - right); };
      const H = top + r.drivers.length * rowH + 26;
      let s = '<svg class="chart" viewBox="0 0 ' + W + ' ' + H + '" role="img" aria-label="Stints and pit stops by car">';
      for (let l = 0; l <= N; l += 5) {
        s += '<line class="grid-l" x1="' + x(l).toFixed(1) + '" x2="' + x(l).toFixed(1) + '" y1="' + top + '" y2="' + (H - 22) + '"/>' +
             '<text x="' + x(l).toFixed(1) + '" y="' + (H - 6) + '" text-anchor="middle">' + l + '</text>';
      }
      r.drivers.forEach(function (d, i) {
        const y = top + i * rowH;
        const p = pit().drivers.find(function (c) { return c.car === d.car; });
        s += '<text x="0" y="' + (y + 23) + '" style="fill:var(--text)">#' + d.car + ' ' + esc(d.name) + '</text>';
        d.plan.forEach(function (st) {
          const x0 = x(st.startLap - 1), w = x(st.startLap - 1 + st.laps) - x0 - 1;
          s += '<rect x="' + x0.toFixed(1) + '" y="' + (y + 6) + '" width="' + w.toFixed(1) + '" height="' + (rowH - 12) + '" rx="4" fill="' + model.compoundColor(st.compound) + '">' +
               '<title>' + esc(model.compoundName(st.compound)) + ': laps ' + st.startLap + '–' + (st.startLap + st.laps - 1) + ' (' + st.laps + ' laps)</title></rect>';
          if (w > 70) {
            s += '<text x="' + (x0 + w / 2).toFixed(1) + '" y="' + (y + 23) + '" text-anchor="middle" style="fill:#0b0b10;font-weight:700">' + esc(model.compoundName(st.compound)) + ' ' + st.laps + '</text>';
          }
        });
        (p ? p.stopLaps : []).forEach(function (stopLap) {
          const t = p.trafficAfter.find(function (a) { return a.lap === stopLap; });
          s += '<rect x="' + (x(stopLap) - 2).toFixed(1) + '" y="' + (y + 1) + '" width="4" height="' + (rowH - 2) + '" fill="var(--blue)">' +
               '<title>#' + d.car + ' stops at the end of lap ' + stopLap + (t ? '; traffic lost in the next ' + t.laps + ' laps: ' + BB.fmtSec(t.seconds, 2) + ' s' : '') + '</title></rect>';
        });
      });
      s += '<line class="marker" x1="' + x(lap).toFixed(1) + '" x2="' + x(lap).toFixed(1) + '" y1="' + (top - 4) + '" y2="' + (H - 22) + '"/></svg>';
      ui.setBody(root, 'pt-timeline', s + '<div class="legend">' + data.compounds.map(function (c, i) {
        return '<span><i class="swatch" style="background:' + model.compoundColor(i) + '"></i>' + esc(c.name) + '</span>';
      }).join('') + '<span><i class="swatch" style="background:var(--blue);border-radius:1px"></i>pit stop</span><span>Line = race clock (lap ' + lap + ').</span></div>');
    }

    // ------------------------------------------------------------ histogram
    function renderHist() {
      const lap = store.get().lap;
      const items = pit().stopBuckets['b' + bucket].map(function (b) {
        const label = b.from === b.to ? String(b.from) : b.from + '–' + b.to;
        return { label: bucket === 1 && b.from % 5 !== 0 ? '' : label, value: b.stops, color: 'var(--blue)', active: lap >= b.from && lap <= b.to,
                 title: 'Laps ' + label + ': ' + b.stops + (b.stops === 1 ? ' stop' : ' stops') };
      });
      ui.setBody(root, 'pt-hist',
        '<div class="dp-bar" style="margin-bottom:6px">' + ui.segmented('bucket', [1, 3, 5].map(function (n) { return { value: String(n), label: n + ' laps' }; }), String(bucket), 'Bucket') + '</div>' +
        charts.bars({ items: items, integer: true, height: 200, label: 'Stops per lap bucket' }) +
        '<p class="seg-l" style="margin-top:4px">Whole field. Bright bar = the clock’s lap.' +
        (pit().window.hasStops ? ' Stops fall between laps ' + pit().window.earliest + ' and ' + pit().window.latest + '.' : ' No stops.') + '</p>');
    }

    // ------------------------------------------------------------ summary
    function renderSummary() {
      const r = run(), p = pit(), f = p.field;
      ui.setBody(root, 'pt-summary',
        '<table class="table"><thead><tr><th>Car</th><th class="num">Stops</th><th class="num">Pit time</th><th>Stints</th></tr></thead><tbody>' +
        p.drivers.map(function (c) {
          return '<tr title="' + (c.stops ? 'Stops at the end of lap ' + c.stopLaps.join(', ') : 'No stops') + '"><td><span class="swatch" style="background:' + model.carColor(c.car) + '"></span><span class="carno">' + c.car + '</span> ' +
            esc(model.carName(r, c.car)) + '</td><td class="num">' + c.stops + '</td><td class="num">' + BB.fmtSec(c.pitTime, 1) + ' s</td><td>' +
            c.compounds.map(function (k, i) { return '<span class="swatch" style="background:' + model.compoundColor(k) + '"></span>' + c.stintLengths[i]; }).join(' ') + '</td></tr>';
        }).join('') + '</tbody></table>' +
        '<div class="kv">' +
          '<div><div class="kv-l">Field pit time</div><div class="kv-v">' + BB.fmtSec(f.pitTime, 1) + ' s</div></div>' +
          '<div><div class="kv-l">Stints</div><div class="kv-v">' + f.stints.count + '</div></div>' +
          '<div><div class="kv-l">Shortest–longest</div><div class="kv-v">' + f.stints.shortest + '–' + f.stints.longest + ' laps</div></div>' +
          '<div><div class="kv-l">Mean stint</div><div class="kv-v">' + BB.fmtSec(f.stints.mean, 1) + ' laps</div></div>' +
        '</div>' +
        '<p class="muted" style="margin-top:10px;font-size:12px">Traffic lost in the 3 laps after each stop: ' +
        p.drivers.map(function (c) { return '#' + c.car + ' ' + BB.fmtSec(c.trafficAfterTotal, 2) + ' s'; }).join(' · ') + '.</p>');
    }

    // ------------------------------------------------------------ undercut
    function renderUnder() {
      const u = data.strategy.undercut;
      if (u === null) { ui.setBody(root, 'pt-under', '<div class="empty">Needs a race of 3 to 100 laps.</div>'); return; }
      const body = root.querySelector('#pt-under [data-body]');
      if (!body.querySelector('#pt-ue')) {
        body.innerHTML =
          '<div class="rng"><label class="seg-l" for="pt-ue">Stop at lap</label><input id="pt-ue" type="range" min="1" max="' + (N - 2) + '" step="1" value="' + earlier + '" aria-label="Earlier stop lap">' +
            '<span class="slider-v" id="pt-ue-v" style="font-size:16px;min-width:34px"></span></div>' +
          '<div class="rng"><label class="seg-l" for="pt-ul">or lap</label><input id="pt-ul" type="range" min="2" max="' + (N - 1) + '" step="1" value="' + later + '" aria-label="Later stop lap">' +
            '<span class="slider-v" id="pt-ul-v" style="font-size:16px;min-width:34px"></span></div><div id="pt-under-out"></div>';
      }
      document.getElementById('pt-ue-v').textContent = String(earlier);
      document.getElementById('pt-ul-v').textContent = String(later);
      const rowIdx = earlier - 1, col = later - earlier - 1;
      const a = u.earlierTotal[rowIdx][col], b = u.laterTotal[rowIdx][col], d = u.delta[rowIdx][col];
      const comp = model.compoundName(u.firstCompound);
      let verdict;
      if (d === null) {
        verdict = '<span class="chip">not feasible</span> <span class="muted">A first stint of ' + (a === null ? earlier : later) + ' laps is longer than ' + esc(comp) + ' allows.</span>';
      } else if (d > 0) {
        verdict = '<span class="chip ok">Undercut wins</span> stopping at lap ' + earlier + ' is faster by <b>' + BB.fmtSec(d, 2) + ' s</b>.';
      } else if (d < 0) {
        verdict = '<span class="chip hold">Overcut wins</span> stopping at lap ' + later + ' is faster by <b>' + BB.fmtSec(-d, 2) + ' s</b>.';
      } else {
        verdict = '<span class="chip">Level</span> both stops cost the same.';
      }
      const row = u.delta[rowIdx].map(function (v, i) { return v === null ? null : [earlier + 1 + i, v]; });
      document.getElementById('pt-under-out').innerHTML =
        '<div class="kv" style="grid-template-columns:repeat(2,1fr)">' +
          '<div><div class="kv-l">Stop at lap ' + earlier + '</div><div class="kv-v" style="font-size:13px">' + (a === null ? '–' : BB.fmtClock(a)) + '</div></div>' +
          '<div><div class="kv-l">Stop at lap ' + later + '</div><div class="kv-v" style="font-size:13px">' + (b === null ? '–' : BB.fmtClock(b)) + '</div></div></div>' +
        '<p style="margin:10px 0 6px">' + verdict + '</p>' +
        charts.lines({ series: [{ name: 'Gained by stopping at lap ' + earlier, color: 'var(--purple)', points: row }], marker: later, xLabel: 'later stop lap',
                       label: 'Time gained by stopping at lap ' + earlier + ' instead of a later lap', height: 150, width: 420,
                       yFmt: function (v) { return v.toFixed(1) + ' s'; } }) +
        '<p class="seg-l" style="margin-top:2px">Seconds gained by stopping at lap ' + earlier + ' instead of the lap on the axis (first stint on ' + esc(comp) + ').</p>';
    }

    // ------------------------------------------------------------ event log
    function eventDetail(ev) {
      const r = run();
      if (ev.type === 'CompoundChange') { return esc(model.compoundName(ev.from)) + ' → ' + esc(model.compoundName(ev.to)); }
      if (ev.type === 'LapCompleted') { return 'on ' + esc(model.compoundName(ev.compound)); }
      if (ev.type === 'PositionChange') { return 'P' + ev.oldPosition + ' → P' + ev.newPosition; }
      if (ev.type === 'OvertakeAttempt') { return 'caught #' + ev.other + ' ' + esc(model.carName(r, ev.other)); }
      if (ev.type === 'Overtake') { return 'passed #' + ev.other + ' ' + esc(model.carName(r, ev.other)); }
      if (ev.type === 'TrafficHold') { return 'behind #' + ev.other + ', lost ' + BB.fmtSec(ev.loss, 2) + ' s'; }
      return '';
    }

    function filteredEvents() {
      const lap = store.get().lap;
      return run().events.filter(function (ev) {
        return typeOn[ev.type] && (logCar === 0 || ev.car === logCar) && (!uptoLap || ev.lap <= lap);
      });
    }

    function renderLog() {
      const r = run(), all = run().events, rows = filteredEvents();
      const pages = Math.max(1, Math.ceil(rows.length / PAGE_ROWS));
      logPage = Math.min(logPage, pages - 1);
      const shown = rows.slice(logPage * PAGE_ROWS, (logPage + 1) * PAGE_ROWS);
      const counts = {};
      all.forEach(function (ev) { counts[ev.type] = (counts[ev.type] || 0) + 1; });
      ui.setBody(root, 'pt-log',
        '<div class="legend togs" style="margin:0 0 8px">' + TYPE_ORDER.filter(function (t) { return counts[t]; }).map(function (t) {
          return '<button type="button" class="tog' + (typeOn[t] ? ' on' : '') + '" data-type="' + t + '" aria-pressed="' + typeOn[t] + '">' + t + ' <b>' + counts[t] + '</b></button>';
        }).join('') + '</div>' +
        '<div class="dp-bar" style="margin-bottom:8px"><label class="seg-l" for="pt-car">Car</label><select class="select" id="pt-car" aria-label="Car">' +
          '<option value="0"' + (logCar === 0 ? ' selected' : '') + '>All cars</option>' + r.drivers.map(function (d) {
            return '<option value="' + d.car + '"' + (logCar === d.car ? ' selected' : '') + '>#' + d.car + ' ' + esc(d.name) + '</option>';
          }).join('') + '</select>' +
          '<button type="button" class="tog' + (uptoLap ? ' on' : '') + '" data-upto="1" aria-pressed="' + uptoLap + '">Up to the clock’s lap ' + store.get().lap + '</button></div>' +
        (shown.length === 0 ? '<div class="empty">No events match.</div>' :
          '<table class="table"><thead><tr><th class="num">Time</th><th class="num">Lap</th><th>Car</th><th>Event</th><th>Detail</th><th class="num">Seq</th></tr></thead><tbody>' +
          shown.map(function (ev) {
            return '<tr><td class="num">' + BB.fmtClock(ev.time) + '</td><td class="num">' + ev.lap + '</td><td><span class="swatch" style="background:' + model.carColor(ev.car) + '"></span>#' + ev.car + '</td>' +
              '<td><span class="chip ' + TYPE_CHIP[ev.type] + '">' + ev.type + '</span></td><td>' + eventDetail(ev) + '</td><td class="num">' + ev.seq + '</td></tr>';
          }).join('') + '</tbody></table>') +
        '<div class="dp-bar" style="margin-top:10px"><button class="btn" type="button" data-page="-1" title="Previous page" aria-label="Previous page"' + (logPage === 0 ? ' disabled' : '') +
          '><svg viewBox="0 0 24 24"><path d="M15 6l-6 6 6 6"/></svg></button>' +
        '<button class="btn" type="button" data-page="1" title="Next page" aria-label="Next page"' + (logPage >= pages - 1 ? ' disabled' : '') + '><svg viewBox="0 0 24 24"><path d="M9 6l6 6-6 6"/></svg></button>' +
        '<span class="seg-l">' + (rows.length ? 'Rows ' + (logPage * PAGE_ROWS + 1) + '–' + (logPage * PAGE_ROWS + shown.length) + ' of ' + rows.length : '0 rows') + '</span></div>');
    }

    // ------------------------------------------------------------ queue
    function renderQueue() {
      const ev = run().events, q = pit().queue, E = ev.length;
      const W = 330, H = 190, m = { l: 34, r: 8, t: 8, b: 24 };
      const maxSeq = ev.reduce(function (mx, e) { return Math.max(mx, e.seq); }, 1);
      const X = function (i) { return m.l + i / Math.max(E - 1, 1) * (W - m.l - m.r); };
      const Y = function (sq) { return H - m.b - sq / maxSeq * (H - m.t - m.b); };
      let dots = '';
      ev.forEach(function (e, i) {
        const out = i < qStep, cur = i === qStep - 1;
        dots += '<circle cx="' + X(i).toFixed(1) + '" cy="' + Y(e.seq).toFixed(1) + '" r="' + (cur ? 4.5 : 2) + '" fill="' + (cur ? 'var(--text)' : out ? 'var(--purple)' : 'var(--muted)') +
          '" opacity="' + (out || cur ? 1 : 0.35) + '"><title>Released ' + (i + 1) + ': inserted as #' + e.seq + ' (' + e.type + ', car ' + e.car + ')</title></circle>';
      });
      const svg = '<svg class="chart" viewBox="0 0 ' + W + ' ' + H + '" role="img" aria-label="Insertion order against release order">' +
        '<line class="axis-l" x1="' + m.l + '" x2="' + (W - m.r) + '" y1="' + (H - m.b) + '" y2="' + (H - m.b) + '"/><line class="axis-l" x1="' + m.l + '" x2="' + m.l + '" y1="' + m.t + '" y2="' + (H - m.b) + '"/>' +
        '<text x="' + (W / 2) + '" y="' + (H - 6) + '" text-anchor="middle">released as (1st → last)</text>' +
        '<text x="4" y="' + (m.t + 8) + '">pushed</text>' + dots + '</svg>';

      const last = ev.slice(Math.max(0, qStep - 5), qStep).reverse();
      const next = qStep < E ? ev[qStep] : null;
      ui.setBody(root, 'pt-queue',
        '<div class="dp-bar" style="margin-bottom:8px"><div class="clock-btns">' +
          qBtn('reset', 'Back to the start', '<path d="M6 5v14M18 5l-9 7 9 7z"/>') + qBtn('back', 'Release one fewer', '<path d="M15 6l-6 6 6 6"/>') +
          qBtn('play', qPlaying ? 'Pause' : 'Play', qPlaying ? '<path d="M8 5v14M16 5v14"/>' : '<path d="M8 5l11 7-11 7z"/>', true) +
          qBtn('fwd', 'Release the next event', '<path d="M9 6l6 6-6 6"/>') + qBtn('end', 'Release them all', '<path d="M18 5v14M6 5l9 7-9 7z"/>') +
        '</div>' + ui.segmented('qspeed', [1, 4, 16].map(function (n) { return { value: String(n), label: n + 'x' }; }), String(qSpeed), 'Speed') + '</div>' +
        '<div style="margin-bottom:8px"><span class="chip opt">' + E + ' events</span> <span class="chip hold" title="Pairs of events the heap received in a different order from the one it released them in">' +
          q.inversions + ' pairs out of order</span> <span class="chip">' + (E - qStep) + ' waiting</span></div>' + svg +
        '<p class="seg-l" style="margin:4px 0 6px">Each dot: when an event was pushed against when it came out. In order would be a straight line.</p>' +
        (next ? '<p style="font-size:12.5px;margin-bottom:6px"><span class="muted">Next out:</span> <b>' + next.type + '</b> #' + next.car + ' at ' + BB.fmtClock(next.time) + ' <span class="muted">(pushed as #' + next.seq + ')</span></p>'
              : '<p class="muted" style="margin-bottom:6px">Queue empty: every event released.</p>') +
        (last.length ? '<ul class="feed" style="font-size:12px">' + last.map(function (e, i) {
          return '<li><span class="chip ' + TYPE_CHIP[e.type] + '">' + e.type + '</span><span class="feed-t">out ' + (qStep - i) + '</span><span>#' + e.car + ' · ' + BB.fmtClock(e.time) + ' · pushed #' + e.seq + '</span></li>';
        }).join('') + '</ul>' : ''));
    }
    function qBtn(act, label, path, primary) {
      return '<button class="btn' + (primary ? ' primary' : '') + '" type="button" data-q="' + act + '" title="' + label + '" aria-label="' + label + '"><svg viewBox="0 0 24 24" aria-hidden="true">' + path + '</svg></button>';
    }
    function stopQueue() { if (qTimer !== null) { clearInterval(qTimer); qTimer = null; } qPlaying = false; }
    function startQueue() {
      stopQueue();
      const E = run().events.length;
      if (qStep >= E) { qStep = 0; }
      qPlaying = true;
      qTimer = setInterval(function () {
        if (qStep >= E) { stopQueue(); renderQueue(); return; }
        qStep += 1;
        if (qStep >= E) { stopQueue(); }
        renderQueue();
      }, QUEUE_MS[qSpeed]);
      renderQueue();
    }

    // ------------------------------------------------------------ greedy
    function renderGreedy() {
      const body = root.querySelector('#pt-greedy [data-body]');
      if (!body.querySelector('#pt-slack')) {
        body.innerHTML =
          '<div class="slider-row"><label class="seg-l" for="pt-slack">Each stop may move</label>' +
          '<input id="pt-slack" type="range" min="0" max="' + (pit().greedy.length - 1) + '" step="1" value="' + slack + '" aria-label="Laps each stop may move">' +
          '<div class="slider-v" id="pt-slack-v" style="font-size:18px;min-width:110px"></div></div><div id="pt-greedy-out"></div>';
      }
      const g = pit().greedy[slack];
      document.getElementById('pt-slack-v').textContent = '±' + slack + (slack === 1 ? ' lap' : ' laps');
      if (g.windows.length === 0) { document.getElementById('pt-greedy-out').innerHTML = '<div class="empty">No stops to plan.</div>'; return; }

      const left = 130, W = 1000, rowH = 22, top = 8, right = 14;
      const x = function (l) { return left + (l - 0.5) / N * (W - left - right); };
      const H = top + g.windows.length * rowH + 40;
      let s = '<svg class="chart" viewBox="0 0 ' + W + ' ' + H + '" role="img" aria-label="Stop windows and the pit calls that cover them">';
      for (let l = 0; l <= N; l += 5) {
        s += '<line class="grid-l" x1="' + x(l + 0.5).toFixed(1) + '" x2="' + x(l + 0.5).toFixed(1) + '" y1="' + top + '" y2="' + (H - 28) + '"/><text x="' + x(l + 0.5).toFixed(1) + '" y="' + (H - 14) + '" text-anchor="middle">' + l + '</text>';
      }
      g.windows.forEach(function (w, i) {
        const y = top + i * rowH, x0 = x(w.earliest - 0.5), x1 = x(w.latest + 0.5);
        s += '<text x="0" y="' + (y + 15) + '">#' + w.car + ' stop</text>' +
             '<rect x="' + x0.toFixed(1) + '" y="' + (y + 3) + '" width="' + Math.max(x1 - x0, 4).toFixed(1) + '" height="' + (rowH - 6) + '" rx="3" fill="' + model.carColor(w.car) + '" opacity=".75">' +
             '<title>Car ' + w.car + ': laps ' + w.earliest + '–' + w.latest + '</title></rect>';
      });
      g.calls.forEach(function (lap) {
        s += '<line x1="' + x(lap).toFixed(1) + '" x2="' + x(lap).toFixed(1) + '" y1="' + (top - 4) + '" y2="' + (H - 28) + '" stroke="var(--green)" stroke-width="2.5"/>' +
             '<text x="' + x(lap).toFixed(1) + '" y="' + (H - 2) + '" text-anchor="middle" style="fill:var(--green);font-weight:700">call L' + lap + '</text>';
      });
      document.getElementById('pt-greedy-out').innerHTML =
        '<p style="margin:10px 0">' + '<span class="chip ok">' + g.calls.length + (g.calls.length === 1 ? ' pit call' : ' pit calls') + '</span> would put a call inside every one of the ' +
        g.windows.length + ' stop windows, at ' + (g.calls.length === 1 ? 'lap ' : 'laps ') + '<b>' + g.calls.join(', ') + '</b>.</p>' + s + '</svg>';
    }

    function renderAll() { renderTimeline(); renderHist(); renderSummary(); renderUnder(); renderLog(); renderQueue(); renderGreedy(); }

    // ------------------------------------------------------------ events
    function onControlsClick(ev) {
      const b = ev.target.closest('[data-seg="run"]');
      if (b) { store.set({ run: b.getAttribute('data-value') }); }
    }
    function onClick(ev) {
      const bk = ev.target.closest('[data-seg="bucket"]');
      if (bk) { bucket = Number(bk.getAttribute('data-value')); renderHist(); return; }
      const t = ev.target.closest('[data-type]');
      if (t) { const k = t.getAttribute('data-type'); typeOn[k] = !typeOn[k]; logPage = 0; renderLog(); return; }
      if (ev.target.closest('[data-upto]')) { uptoLap = !uptoLap; logPage = 0; renderLog(); return; }
      const pg = ev.target.closest('[data-page]');
      if (pg && !pg.disabled) { logPage += Number(pg.getAttribute('data-page')); renderLog(); return; }
      const sp = ev.target.closest('[data-seg="qspeed"]');
      if (sp) { qSpeed = Number(sp.getAttribute('data-value')); if (qPlaying) { startQueue(); } else { renderQueue(); } return; }
      const q = ev.target.closest('[data-q]');
      if (q) {
        const act = q.getAttribute('data-q'), E = run().events.length;
        if (act === 'play') { if (qPlaying) { stopQueue(); renderQueue(); } else { startQueue(); } return; }
        stopQueue();
        if (act === 'reset') { qStep = 0; } else if (act === 'back') { qStep = Math.max(0, qStep - 1); }
        else if (act === 'fwd') { qStep = Math.min(E, qStep + 1); } else if (act === 'end') { qStep = E; }
        renderQueue();
      }
    }
    function onChange(ev) {
      if (ev.target.id === 'pt-car') { logCar = Number(ev.target.value); logPage = 0; renderLog(); }
    }
    function onInput(ev) {
      const id = ev.target.id;
      if (id === 'pt-ue' || id === 'pt-ul') {
        if (id === 'pt-ue') { earlier = Number(ev.target.value); if (later <= earlier) { later = earlier + 1; } }
        else { later = Number(ev.target.value); if (earlier >= later) { earlier = later - 1; } }
        later = Math.min(later, N - 1); earlier = Math.max(1, Math.min(earlier, later - 1));
        document.getElementById('pt-ue').value = String(earlier); document.getElementById('pt-ul').value = String(later);
        renderUnder();
      } else if (id === 'pt-slack') { slack = Number(ev.target.value); renderGreedy(); }
    }
    controls.addEventListener('click', onControlsClick);
    root.addEventListener('click', onClick);
    root.addEventListener('change', onChange);
    root.addEventListener('input', onInput);

    const unsubscribe = store.subscribe(function (changed) {
      if (changed.indexOf('run') >= 0) { stopQueue(); qStep = 0; logPage = 0; renderControls(); renderAll(); return; }
      if (changed.indexOf('lap') >= 0) { renderTimeline(); renderHist(); if (uptoLap) { renderLog(); } }
    });

    earlier = Math.min(earlier, N - 2); later = Math.max(earlier + 1, Math.min(later, N - 1));
    qStep = run().events.length;
    renderControls();
    renderAll();

    return function unmount() {
      stopQueue();
      unsubscribe();
      controls.removeEventListener('click', onControlsClick);
      root.removeEventListener('click', onClick);
      root.removeEventListener('change', onChange);
      root.removeEventListener('input', onInput);
    };
  }

  BB.pages = BB.pages || {};
  BB.pages.pit = {
    title: 'Pit & Events',
    subtitle: 'Stops, undercuts, the event log and the queue behind it',
    clock: true,
    mount: mount
  };
})(window.BB = window.BB || {});
