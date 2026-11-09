/* Algorithms: one card per data structure and algorithm, with the cost the code states, the
   source files it lives in, the pages that use it, and a small demo driven by exported results.
   The catalog and the operation counts come from the C++ export; costs are quoted from the
   code's own comments and docs/COMPLEXITY_AUDIT.md. Counts, never timings. */
(function (BB) {
  'use strict';

  const esc = BB.esc, model = BB.model, ui = BB.ui, charts = BB.charts;

  const CATEGORIES = ['All', 'Optimisation', 'Heaps', 'Ranges', 'Windows', 'Search', 'Rules'];
  const PAGE_NAMES = { command: 'Command Center', sim: 'Race Sim', lab: 'Strategy Lab', tires: 'Tires', drivers: 'Drivers', pit: 'Pit & Events' };

  function mount(root, controls) {
    const store = BB.store;
    const data = model.data();
    const alg = data.algorithms;
    const N = model.laps();
    const strat = data.strategy;

    let filter = 'All';
    const S = {
      dp: { lap: Math.min(18, N) },
      lb: { k: 0 },
      prefix: { c: 0, len: 10 },
      // Defaults are clamped to the race, which can be as short as one lap.
      segtree: { from: Math.min(11, N), to: Math.min(20, N) },
      fenwick: { from: Math.min(11, N), to: Math.min(20, N) },
      deque: { size: 5, lap: Math.min(26, N) },
      bsearch: { c: 0, limit: Math.min(4, data.tires.limits.length - 1) },
      topk: { k: 5, order: 'fastest' },
      greedy: { slack: 5 },
      hashmap: { car: 1 }
    };

    const run = function () { return model.run(); };
    const link = function (id) {
      return BB.pages[id] ? '<a class="lnk" href="#/' + id + '">' + esc(PAGE_NAMES[id] || id) + '</a>' : null;
    };
    const slider = function (id, label, min, max, value) {
      return '<div class="rng"><label class="seg-l" for="' + id + '">' + label + '</label><input id="' + id + '" type="range" min="' + min + '" max="' + max +
        '" step="1" value="' + value + '" aria-label="' + esc(label) + '"><span class="slider-v" id="' + id + '-v" style="font-size:15px;min-width:36px">' + value + '</span></div>';
    };
    /** Build a demo's slider head once; return the region that is re-rendered. */
    const region = function (el, head) {
      if (!el.querySelector('[data-dyn]')) { el.innerHTML = head + '<div data-dyn></div>'; }
      return el.querySelector('[data-dyn]');
    };
    const demoEl = function (id) { return root.querySelector('#al-' + id + ' .al-demo'); };

    // ---------------------------------------------------------------- cards
    function cardHtml(info) {
      const pages = info.usedIn.map(link).filter(Boolean);
      return ui.panel({
        id: 'al-' + info.id, span: 6, title: info.name,
        algo: info.files[0].split('/').pop(), algoTip: 'Source: ' + info.files.join(', '),
        body:
          '<div class="al-meta"><span class="chip opt">' + esc(info.category) + '</span>' +
            (pages.length ? '<span class="muted">Shown on</span> ' + pages.join(' · ') : '') + '</div>' +
          '<p class="al-purpose">' + esc(info.purpose) + '</p>' +
          '<table class="table al-costs"><thead><tr><th>Operation</th><th>Cost</th></tr></thead><tbody>' +
            info.costs.map(function (c) { return '<tr><td>' + esc(c.operation) + '</td><td class="mono">' + esc(c.cost) + '</td></tr>'; }).join('') + '</tbody></table>' +
          '<p class="muted al-note">' + esc(info.note) + '</p>' +
          '<p class="al-files">' + info.files.map(function (f) { return '<code class="cmd">' + esc(f) + '</code>'; }).join(' ') + '</p>' +
          '<div class="al-demo"></div>'
      });
    }

    function renderCards() {
      const shown = alg.catalog.filter(function (a) { return filter === 'All' || a.category === filter; });
      root.querySelector('#al-cards').innerHTML = shown.map(cardHtml).join('') || '<div class="empty span-12">No algorithms in this category.</div>';
      shown.forEach(function (a) { if (demos[a.id]) { demos[a.id](); } });
    }

    // ---------------------------------------------------------------- demos
    const demos = {
      dp: function () {
        const el = demoEl('dp'), dyn = region(el, slider('al-dp-lap', 'Lap', 1, N, S.dp.lap));
        const cell = strat.dp[S.dp.lap];
        const prev = strat.dp[cell.from], table = strat.stintCostTable[cell.compound];
        const pit = cell.from > 0 ? data.meta.pitLoss : 0;
        dyn.innerHTML = '<div class="dp-form"><b>dp[' + cell.lap + ']</b> = <b>' + BB.fmtSec(cell.cost, 2) + ' s</b>. Best last stint: <span class="swatch" style="background:' +
          model.compoundColor(cell.compound) + '"></span>' + esc(model.compoundName(cell.compound)) + ' for ' + cell.laps + ' laps, from <b>dp[' + cell.from + '] = ' + BB.fmtSec(prev.cost, 2) +
          '</b> + stintCost <b>' + BB.fmtSec(table[cell.laps - 1], 2) + '</b>' + (pit > 0 ? ' + pit loss <b>' + BB.fmtSec(pit, 2) + '</b>' : '') + '.</div>' +
          '<p class="muted" style="margin-top:6px">One cell is a minimum over every compound and stint length. The Strategy Lab steps through all of them.</p>';
      },

      leaderboard: function () {
        const el = demoEl('leaderboard'), r = run(), fin = r.final, D = fin.length;
        S.lb.k = Math.min(S.lb.k, D);
        const popped = fin.slice(0, S.lb.k), next = S.lb.k < D ? fin[S.lb.k] : null;
        el.innerHTML = '<div class="dp-bar" style="margin-bottom:8px"><div class="clock-btns">' + btn('lb', 'reset', 'Put them all back', '<path d="M6 5v14M18 5l-9 7 9 7z"/>') +
          btn('lb', 'pop', 'Pop the leader', '<path d="M9 6l6 6-6 6"/>', true) + btn('lb', 'all', 'Pop everything', '<path d="M18 5v14M6 5l9 7-9 7z"/>') + '</div>' +
          '<span class="chip opt">' + (D - S.lb.k) + ' in the heap</span></div>' +
          '<p class="seg-l" style="margin-bottom:4px">Pushed in grid order</p><div class="legend" style="margin:0 0 8px">' + r.drivers.map(function (d) {
            const out = popped.some(function (p) { return p.car === d.car; });
            return '<span style="opacity:' + (out ? '.3' : '1') + '"><i class="swatch" style="background:' + model.carColor(d.car) + '"></i>#' + d.car + '</span>';
          }).join('') + '</div><p class="seg-l" style="margin-bottom:4px">Popped, in classification order</p><div class="legend" style="margin:0">' +
          (popped.length ? popped.map(function (p, i) { return '<span>' + (i + 1) + '. <b>#' + p.car + '</b> ' + BB.fmtClock(p.time) + '</span>'; }).join('') : '<span class="muted">nothing yet</span>') + '</div>' +
          (next ? '<p class="muted" style="margin-top:8px">peek() is O(1): the next car out is #' + next.car + '. Each pop is O(log n).</p>' : '');
      },

      prefix: function () {
        const el = demoEl('prefix');
        const comps = data.compounds, M = comps[S.prefix.c].maxStintLaps;
        S.prefix.len = Math.min(S.prefix.len, M);
        const head = ui.segmented('pfc', comps.map(function (c, i) { return { value: String(i), label: c.name }; }), String(S.prefix.c), 'Compound') + slider('al-prefix-len', 'Stint laps', 1, M, S.prefix.len);
        const body = el.querySelector('[data-dyn]');
        if (body && body.getAttribute('data-c') !== String(S.prefix.c)) { el.innerHTML = ''; }
        const dyn = region(el, head);
        dyn.setAttribute('data-c', String(S.prefix.c));
        const v = strat.stintCostTable[S.prefix.c][S.prefix.len - 1];
        dyn.innerHTML = '<div class="dp-form">stintCost(' + esc(comps[S.prefix.c].name) + ', ' + S.prefix.len + ') = <b>' + BB.fmtSec(v, 2) + ' s</b></div>' +
          '<p class="muted" style="margin-top:6px">That is ' + S.prefix.len + ' lap times added up, read as one table lookup. The optimizer reads it ' + BB.fmtSec(alg.growth.dp.raceTransitions, 0) +
          ' times for this ' + N + '-lap race.</p>';
      },

      segtree: function () { rangeDemo('segtree'); },
      fenwick: function () { rangeDemo('fenwick'); },

      deque: function () {
        const el = demoEl('deque'), r = run(), w = r.overview.windows, car = w.cars[0];
        const head = ui.segmented('dqs', w.sizes.map(function (n) { return { value: String(n), label: n + ' laps' }; }), String(S.deque.size), 'Window') + slider('al-deque-lap', 'Lap', 1, N, S.deque.lap);
        const dyn = region(el, head);
        const s = car['w' + S.deque.size], k = S.deque.lap - 1;
        const first = Math.max(1, S.deque.lap - S.deque.size + 1);
        const times = car.lapTimes, hi = Math.max.apply(null, times), lo = Math.min.apply(null, times);
        const pts = times.map(function (t, i) { return [i + 1, t]; });
        const fl = s.fastestLap[k], sl = s.slowestLap[k];
        dyn.innerHTML = charts.lines({
          series: [{ name: '#' + car.car + ' lap time', color: model.carColor(car.car), points: pts,
                     marks: [[fl, times[fl - 1], 'Quickest in the window: lap ' + fl, 'var(--green)'], [sl, times[sl - 1], 'Slowest in the window: lap ' + sl, 'var(--red)']] }],
          bands: [{ name: 'The window', color: 'var(--blue)', upper: [[first, hi], [S.deque.lap, hi]], lower: [[first, lo], [S.deque.lap, lo]] }],
          xLabel: 'lap', label: 'A sliding window over lap times', height: 170, width: 520, yFmt: function (v) { return BB.fmtClock(v); }
        }) + '<p class="dp-form" style="margin-top:6px">Laps ' + first + '–' + S.deque.lap + ': quickest <b style="color:var(--green)">L' + fl + ' ' + BB.fmtClock(s.fastest[k]) +
          '</b>, slowest <b style="color:var(--red)">L' + sl + ' ' + BB.fmtClock(s.slowest[k]) + '</b>, mean ' + BB.fmtClock(s.average[k]) + '.</p>';
      },

      eventqueue: function () {
        const el = demoEl('eventqueue'), ev = run().events, q = run().pit.queue, E = ev.length;
        const W = 330, H = 150, m = { l: 12, r: 8, t: 8, b: 22 };
        const maxSeq = ev.reduce(function (mx, e) { return Math.max(mx, e.seq); }, 1);
        let dots = '';
        ev.forEach(function (e, i) {
          dots += '<circle cx="' + (m.l + i / Math.max(E - 1, 1) * (W - m.l - m.r)).toFixed(1) + '" cy="' + (H - m.b - e.seq / maxSeq * (H - m.t - m.b)).toFixed(1) + '" r="1.8" fill="var(--purple)">' +
            '<title>Released ' + (i + 1) + ', pushed as #' + e.seq + '</title></circle>';
        });
        el.innerHTML = '<svg class="chart" style="max-width:460px" viewBox="0 0 ' + W + ' ' + H + '" role="img" aria-label="Push order against release order">' +
          '<line class="axis-l" x1="' + m.l + '" x2="' + (W - m.r) + '" y1="' + (H - m.b) + '" y2="' + (H - m.b) + '"/>' + dots +
          '<text x="' + (W / 2) + '" y="' + (H - 5) + '" text-anchor="middle">released as (1st → last)</text></svg>' +
          '<p class="dp-form" style="margin-top:6px"><b>' + E + '</b> events went in and came out in time order. <b>' + q.inversions +
          '</b> pairs arrived in a different order from the one they left in: the work the heap did. Counted with a Fenwick tree in O(D log D).</p>';
      },

      bsearch: function () {
        const el = demoEl('bsearch'), tires = data.tires, p = tires.profiles[S.bsearch.c];
        const head = ui.segmented('bsc', tires.profiles.map(function (x, i) { return { value: String(i), label: x.name }; }), String(S.bsearch.c), 'Compound') +
          slider('al-bsearch-limit', 'Limit', 0, tires.limits.length - 1, S.bsearch.limit);
        const dyn = region(el, head);
        const limit = tires.limits[S.bsearch.limit], q = p.thresholds[S.bsearch.limit];
        document.getElementById('al-bsearch-limit-v').textContent = BB.fmtSec(limit, 2) + ' s';
        dyn.innerHTML = '<table class="table" style="margin-top:8px"><thead><tr><th>Step</th><th class="num">lo</th><th class="num">hi</th><th class="num">mid</th><th class="num">loss at mid</th><th>vs limit</th></tr></thead><tbody>' +
          q.steps.map(function (s, i) {
            return '<tr><td>' + (i + 1) + '</td><td class="num">' + s[0] + '</td><td class="num">' + s[1] + '</td><td class="num">' + s[2] + '</td><td class="num">' + BB.fmtSec(p.degradationVsFresh[s[2]], 3) +
              ' s</td><td class="' + (s[3] ? 'red' : 'green') + '">' + (s[3] ? 'over: hi = mid' : 'within: lo = mid + 1') + '</td></tr>';
          }).join('') + '</tbody></table><p class="dp-form" style="margin-top:6px">' + (q.age === null ? 'No age exceeds the limit.' : 'First age past the limit: <b>' + q.age + '</b>') +
          ' in <b>' + q.steps.length + '</b> steps; a scan could read up to ' + p.maxStint + ' ages.</p>';
      },

      topk: function () {
        const el = demoEl('topk'), a = run().analytics.topLaps;
        const head = ui.segmented('tkk', a.sizes.map(function (n) { return { value: String(n), label: 'Top ' + n }; }), String(S.topk.k), 'Laps') +
          ui.segmented('tko', [{ value: 'fastest', label: 'Fastest' }, { value: 'slowest', label: 'Slowest' }], S.topk.order, 'Order');
        const dyn = region(el, head);
        const rows = a.field['k' + S.topk.k][S.topk.order];
        dyn.innerHTML = '<table class="table" style="margin-top:8px"><tbody>' + rows.map(function (l, i) {
          return '<tr><td class="num">' + (i + 1) + '</td><td><span class="swatch" style="background:' + model.carColor(l.car) + '"></span>#' + l.car + '</td><td class="num">lap ' + l.lap + '</td><td class="num">' + BB.fmtClock(l.time) + '</td></tr>';
        }).join('') + '</tbody></table><p class="muted" style="margin-top:6px">The heap held at most ' + S.topk.k + ' laps while it scanned all ' + (N * run().drivers.length) + '.</p>';
      },

      greedy: function () {
        const el = demoEl('greedy'), g = run().pit.greedy;
        const dyn = region(el, slider('al-greedy-slack', 'Each stop may move ±', 0, g.length - 1, S.greedy.slack));
        const e = g[S.greedy.slack];
        dyn.innerHTML = '<p class="dp-form" style="margin-top:6px"><span class="chip ok">' + e.calls.length + (e.calls.length === 1 ? ' call' : ' calls') + '</span> at ' +
          (e.calls.length === 1 ? 'lap ' : 'laps ') + '<b>' + e.calls.join(', ') + '</b> cover all ' + e.windows.length + ' stop windows:</p><div class="legend" style="margin-top:4px">' +
          e.windows.map(function (w) { return '<span><i class="swatch" style="background:' + model.carColor(w.car) + '"></i>#' + w.car + ' ' + w.earliest + '–' + w.latest + '</span>'; }).join('') + '</div>';
      },

      overtake: function () {
        const el = demoEl('overtake'), on = data.race.runs.overtakeOn, off = data.race.runs.overtakeOff;
        const count = function (r, t) { return r.events.filter(function (e) { return e.type === t; }).length; };
        const hold = on.events.find(function (e) { return e.type === 'TrafficHold'; });
        const timeOf = function (r, car) { return r.final.find(function (f) { return f.car === car; }).time; };
        el.innerHTML = '<div class="kv" style="grid-template-columns:repeat(2,1fr);margin-top:0"><div><div class="kv-l">minGap</div><div class="kv-v">' + BB.fmtSec(on.overtake.minGap, 1) + ' s</div></div>' +
          '<div><div class="kv-l">passMargin</div><div class="kv-v">' + BB.fmtSec(on.overtake.passMargin, 1) + ' s</div></div></div>' +
          '<table class="table" style="margin-top:10px"><thead><tr><th>Events</th><th class="num">Rule off</th><th class="num">Rule on</th></tr></thead><tbody>' +
          ['OvertakeAttempt', 'Overtake', 'TrafficHold'].map(function (t) { return '<tr><td>' + t + '</td><td class="num">' + count(off, t) + '</td><td class="num">' + count(on, t) + '</td></tr>'; }).join('') + '</tbody></table>' +
          '<p class="dp-form" style="margin-top:8px">The rule costs #1 <b>' + BB.fmtSec(timeOf(on, 1) - timeOf(off, 1), 2) + ' s</b> over the race (' + BB.fmtClock(timeOf(off, 1)) + ' → ' + BB.fmtClock(timeOf(on, 1)) + ').' +
          (hold ? ' First hold: #' + hold.car + ' behind #' + hold.other + ' on lap ' + hold.lap + ', losing ' + BB.fmtSec(hold.loss, 2) + ' s.' : '') + '</p>';
      },

      hashmap: function () {
        const el = demoEl('hashmap'), r = run();
        const head = '<div class="dp-bar" style="margin-bottom:6px"><label class="seg-l" for="al-hm-car">Look up car</label><select class="select" id="al-hm-car">' +
          r.drivers.map(function (d) { return '<option value="' + d.car + '"' + (d.car === S.hashmap.car ? ' selected' : '') + '>#' + d.car + '</option>'; }).join('') + '</select></div>';
        const dyn = region(el, head);
        const d = r.overview.drivers.find(function (x) { return x.car === S.hashmap.car; });
        dyn.innerHTML = '<p class="dp-form">car ' + d.car + ' → <b>' + esc(d.name) + '</b>: ' + d.laps + ' laps, best ' + BB.fmtClock(d.bestLap.time) + ' (lap ' + d.bestLap.lap + '), ' + d.stops +
          (d.stops === 1 ? ' stop' : ' stops') + ', race time ' + BB.fmtClock(d.totalTime) + '.</p><p class="muted" style="margin-top:6px">One hash lookup, O(1) on average, then a read. The map is never iterated.</p>';
      }
    };

    /** Shared by the segment-tree and Fenwick cards: pick laps, see the nodes the query adds. */
    function rangeDemo(id) {
      const el = demoEl(id), st = data.structures;
      if (st === null) { el.innerHTML = '<p class="muted">Node lists are exported for races of up to 100 laps; this one has ' + N + '.</p>'; return; }
      const state = S[id];
      const head = slider('al-' + id + '-from', 'From lap', 1, N, state.from) + slider('al-' + id + '-to', 'To lap', 1, N, state.to);
      const dyn = region(el, head);
      const b = state.from - 1, e = state.to;
      if (id === 'segtree') {
        const nodes = st.segmentNodes[b][e - b - 1];
        dyn.innerHTML = charts.nodeStrip(N, [{ label: nodes.length + ' nodes add up laps ' + state.from + '–' + state.to + ' (a scan reads ' + (e - b) + ')', color: 'var(--blue)', nodes: nodes }], state.from, state.to);
      } else {
        const up = st.fenwickNodes[e], down = st.fenwickNodes[b];
        dyn.innerHTML = charts.nodeStrip(N, [
          { label: 'prefix(' + e + '): ' + up.length + ' nodes added', color: 'var(--green)', nodes: up },
          { label: 'prefix(' + b + '): ' + down.length + ' nodes subtracted', color: 'var(--red)', nodes: down }], state.from, state.to);
      }
    }

    function btn(group, act, label, path, primary) {
      return '<button class="btn' + (primary ? ' primary' : '') + '" type="button" data-grp="' + group + '" data-act="' + act + '" title="' + label + '" aria-label="' + label + '"><svg viewBox="0 0 24 24" aria-hidden="true">' + path + '</svg></button>';
    }

    // ---------------------------------------------------------------- growth
    function renderGrowth() {
      const g = alg.growth, d = g.dp;
      const logPts = function (arr) { return arr.map(function (v, i) { return [d.laps[i], Math.log10(v)]; }); };
      const dpChart = charts.lines({
        series: [{ name: 'Brute-force reference: calls', color: 'var(--red)', points: logPts(d.bruteForceCalls) },
                 { name: 'DP: candidates evaluated', color: 'var(--purple)', points: logPts(d.dpTransitions) }],
        xLabel: 'race laps', label: 'Brute-force calls against DP evaluations, log scale', height: 230, yFmt: function (v) { return '1e' + Math.round(v); }
      });
      let rangeChart = '<div class="empty">Range-work counts are exported for races of up to 100 laps.</div>', rows = '';
      if (g.ranges !== null) {
        const P = g.ranges.points;
        rangeChart = charts.lines({
          series: [{ name: 'Scan: laps read', color: 'var(--red)', points: P.map(function (p) { return [p.laps, p.scanMean]; }) },
                   { name: 'Fenwick: nodes touched', color: 'var(--green)', points: P.map(function (p) { return [p.laps, p.fenwickMean]; }) },
                   { name: 'Segment tree: nodes touched', color: 'var(--blue)', points: P.map(function (p) { return [p.laps, p.segmentMean]; }) }],
          xLabel: 'race laps', label: 'Average work per range query', height: 230, yFmt: function (v) { return String(Math.round(v * 10) / 10); }
        });
        const here = P.find(function (p) { return p.laps === N; });
        if (here) {
          rows = '<div class="kv"><div><div class="kv-l">Scan, ' + N + ' laps</div><div class="kv-v">' + BB.fmtSec(here.scanMean, 1) + ' laps</div></div>' +
            '<div><div class="kv-l">Segment tree</div><div class="kv-v">' + BB.fmtSec(here.segmentMean, 1) + ' nodes</div></div>' +
            '<div><div class="kv-l">Fenwick</div><div class="kv-v">' + BB.fmtSec(here.fenwickMean, 1) + ' nodes</div></div>' +
            '<div><div class="kv-l">Worst segment query</div><div class="kv-v">' + here.segmentMax + ' nodes</div></div></div>';
        }
      }
      ui.setBody(root, 'al-growth',
        '<p class="muted" style="margin-bottom:12px">Operations are <b>counted, not timed</b>, so these numbers are identical on every machine and in every export. They show the shape of the growth; they do not prove a bound. ' +
        '<code class="cmd">make bench</code> times the same structures on yours.</p>' +
        '<div class="grid" style="gap:20px"><div class="span-6"><p class="seg-l" style="margin-bottom:6px">Optimal stints: brute force against the DP (log scale)</p>' + dpChart +
        '<div class="legend"><span><i class="swatch" style="background:var(--red)"></i>Brute-force reference: ' + d.bruteForceCalls[d.bruteForceCalls.length - 1].toLocaleString('en-US') + ' calls at ' + d.laps[d.laps.length - 1] + ' laps</span>' +
        '<span><i class="swatch" style="background:var(--purple)"></i>DP: ' + d.dpTransitions[d.dpTransitions.length - 1].toLocaleString('en-US') + ' candidates</span></div>' +
        '<p class="muted" style="margin-top:8px">With ' + data.compounds.length + ' compounds the reference’s calls grow by about ' + (data.compounds.length + 1) + 'x per lap while every compound can run the whole stint. The DP evaluates ' + d.raceTransitions.toLocaleString('en-US') + ' candidates for this ' + d.raceLaps + '-lap race.</p></div>' +
        '<div class="span-6"><p class="seg-l" style="margin-bottom:6px">Range query over laps: tree nodes against a scan</p>' + rangeChart +
        '<div class="legend"><span><i class="swatch" style="background:var(--red)"></i>Scan</span><span><i class="swatch" style="background:var(--blue)"></i>Segment tree</span><span><i class="swatch" style="background:var(--green)"></i>Fenwick (two prefixes)</span></div>' + rows + '</div></div>');
    }

    // ---------------------------------------------------------------- shell
    root.innerHTML = '<div class="grid" id="al-cards"></div><div class="grid" style="margin-top:var(--gap)">' +
      ui.panel({ id: 'al-growth', span: 12, title: 'Operation counts', algo: 'ExportCommand.cpp · counted, not timed',
                 algoTip: 'The brute-force reference counts its own recursive calls; the DP count is its candidate evaluations; the tree counts replay the documented node walks, checked against the real trees’ answers before export.' }) + '</div>';

    function renderControls() {
      controls.innerHTML = ui.segmented('cat', CATEGORIES.map(function (c) { return { value: c, label: c }; }), filter, 'Category') +
        ui.segmented('run', [{ value: 'overtakeOff', label: 'OFF' }, { value: 'overtakeOn', label: 'ON' }], store.get().run, 'Overtake rule');
    }

    // ---------------------------------------------------------------- events
    function onControlsClick(ev) {
      const c = ev.target.closest('[data-seg="cat"]');
      if (c) { filter = c.getAttribute('data-value'); renderControls(); renderCards(); return; }
      const r = ev.target.closest('[data-seg="run"]');
      if (r) { store.set({ run: r.getAttribute('data-value') }); }
    }
    function onClick(ev) {
      const b = ev.target.closest('[data-grp="lb"]');
      if (b) {
        const act = b.getAttribute('data-act'), D = run().final.length;
        if (act === 'reset') { S.lb.k = 0; } else if (act === 'pop') { S.lb.k = Math.min(D, S.lb.k + 1); } else { S.lb.k = D; }
        demos.leaderboard(); return;
      }
      const pf = ev.target.closest('[data-seg="pfc"]');
      if (pf) { S.prefix.c = Number(pf.getAttribute('data-value')); demos.prefix(); return; }
      const dq = ev.target.closest('[data-seg="dqs"]');
      if (dq) { S.deque.size = Number(dq.getAttribute('data-value')); demoEl('deque').innerHTML = ''; demos.deque(); return; }
      const bs = ev.target.closest('[data-seg="bsc"]');
      if (bs) { S.bsearch.c = Number(bs.getAttribute('data-value')); demoEl('bsearch').innerHTML = ''; demos.bsearch(); return; }
      const tk = ev.target.closest('[data-seg="tkk"]');
      if (tk) { S.topk.k = Number(tk.getAttribute('data-value')); demoEl('topk').innerHTML = ''; demos.topk(); return; }
      const to = ev.target.closest('[data-seg="tko"]');
      if (to) { S.topk.order = to.getAttribute('data-value'); demoEl('topk').innerHTML = ''; demos.topk(); }
    }
    function setVal(id, v) { const e = document.getElementById(id + '-v'); if (e) { e.textContent = String(v); } }
    function onInput(ev) {
      const id = ev.target.id, v = Number(ev.target.value);
      if (id === 'al-dp-lap') { S.dp.lap = v; setVal(id, v); demos.dp(); }
      else if (id === 'al-prefix-len') { S.prefix.len = v; setVal(id, v); demos.prefix(); }
      else if (id === 'al-deque-lap') { S.deque.lap = v; setVal(id, v); demos.deque(); }
      else if (id === 'al-bsearch-limit') { S.bsearch.limit = v; demos.bsearch(); }
      else if (id === 'al-greedy-slack') { S.greedy.slack = v; setVal(id, v); demos.greedy(); }
      else if (id === 'al-segtree-from' || id === 'al-segtree-to' || id === 'al-fenwick-from' || id === 'al-fenwick-to') {
        const which = id.indexOf('segtree') >= 0 ? 'segtree' : 'fenwick', st = S[which];
        if (id.slice(-4) === 'from') { st.from = v; if (st.to < v) { st.to = v; } } else { st.to = v; if (st.from > v) { st.from = v; } }
        document.getElementById('al-' + which + '-from').value = String(st.from); document.getElementById('al-' + which + '-to').value = String(st.to);
        setVal('al-' + which + '-from', st.from); setVal('al-' + which + '-to', st.to);
        rangeDemo(which);
      }
    }
    function onChange(ev) {
      if (ev.target.id === 'al-hm-car') { S.hashmap.car = Number(ev.target.value); demos.hashmap(); }
    }
    controls.addEventListener('click', onControlsClick);
    root.addEventListener('click', onClick);
    root.addEventListener('input', onInput);
    root.addEventListener('change', onChange);

    const unsubscribe = store.subscribe(function (changed) {
      if (changed.indexOf('run') >= 0) { renderControls(); renderCards(); renderGrowth(); }
    });

    renderControls();
    renderCards();
    renderGrowth();

    return function unmount() {
      unsubscribe();
      controls.removeEventListener('click', onControlsClick);
      root.removeEventListener('click', onClick);
      root.removeEventListener('input', onInput);
      root.removeEventListener('change', onChange);
    };
  }

  BB.pages = BB.pages || {};
  BB.pages.algo = {
    title: 'Algorithms',
    subtitle: 'Every structure and algorithm behind the dashboard: its cost, its code, and a demo',
    clock: false,
    mount: mount
  };
})(window.BB = window.BB || {});
