/* Strategy Lab: the optimizer's dynamic program, shown. Every number is exported by the
   C++ engine for each pit-loss scenario; the page picks a scenario and animates the
   DP trace step by step. It does no optimisation of its own. */
(function (BB) {
  'use strict';

  const esc = BB.esc, model = BB.model, ui = BB.ui, charts = BB.charts;

  const STEP_MS = 650;   // pause between DP steps at 1x

  function mount(root, controls) {
    const store = BB.store;
    const data = model.data();
    const strat = data.strategy;
    const N = model.laps();
    const scenarios = strat.scenarios;
    const table = strat.stintCostTable;

    // Start on the scenario that is the race's own pit loss.
    let sc = 0;
    scenarios.forEach(function (s, i) {
      if (Math.abs(s.pitLoss - data.meta.pitLoss) < data.meta.timeEpsilon) { sc = i; }
    });
    let step = 0;          // DP step; set to the last step when a scenario is shown
    let speed = 1;
    let playing = false;
    let timer = null;

    const scen = function () { return scenarios[sc]; };
    const totalSteps = function () { return N + scen().backtrack.length; };

    root.innerHTML =
      '<div class="grid">' +
        ui.panel({ id: 'st-scenario', span: 12, title: 'Scenario', algo: 'StrategyAnalytics.cpp · pitLossSweep',
                   algoTip: 'Each pit loss on the slider is a full optimizer run: a PitStrategy is built with that pit loss and optimize() is called. Nothing is interpolated.' }) +
        ui.panel({ id: 'st-plan', span: 6, title: 'Optimal plan', algo: 'PitStrategy.cpp · DP + prefix sums',
                   algoTip: 'PitStrategy::optimize(): dynamic programming over a prefix-sum stint-cost table, then backtracking the stored choices.' }) +
        ui.panel({ id: 'st-stops', span: 6, title: 'Best plan per stop count', algo: 'StrategyAnalytics.cpp · bestForStopCount',
                   algoTip: 'bestForStopCount: the same DAG, with the number of stops as an extra dimension. O(S*N*C*M).' }) +
        ui.panel({ id: 'st-dp', span: 12, title: 'DP visualiser', algo: 'PitStrategy.cpp · dp[0..N] + backtrack',
                   algoTip: 'dp[L] is the cheapest way to run L laps: the minimum over compound c and stint length l of dp[L-l] + stintCost[c][l] (+ pit loss if it is not the first stint). The trace replays the optimizer’s own recurrence and is checked against optimize() before export.' }) +
        ui.panel({ id: 'st-gap', span: 6, title: 'Optimality check', algo: 'brute force · reference',
                   algoTip: 'A short race is solved twice: by the DP and by trying every stint decomposition. Written separately from the DP, so agreement means something. Checked for every scenario before export.' }) +
        ui.panel({ id: 'st-sens', span: 6, title: 'Pit-loss sensitivity', algo: 'StrategyAnalytics.cpp · pitLossSweep',
                   algoTip: 'The optimal total time and stop count as the pit loss varies. Each point is a separate optimizer run.' }) +
      '</div>';

    // ----------------------------------------------------------- panels
    /** Built once: the slider must survive being dragged, so input only updates the readout. */
    function renderScenario() {
      const s = scen();
      ui.setBody(root, 'st-scenario',
        '<div class="slider-row">' +
          '<label class="seg-l" for="st-range">Pit loss</label>' +
          '<input id="st-range" type="range" min="0" max="' + (scenarios.length - 1) + '" step="1" value="' + sc + '" aria-label="Pit loss scenario">' +
          '<div class="slider-v" id="st-readout">' + BB.fmtSec(s.pitLoss, 1) + ' s</div>' +
        '</div>' +
        '<div class="slider-meta"><span class="chip" id="st-which">Scenario ' + (sc + 1) + ' of ' + scenarios.length + '</span> ' +
          '<span class="chip">' + N + ' laps</span> ' +
          '<span class="seg-l">More scenarios: </span><code class="cmd">boxbox --laps ' + N + ' --pit-loss ' + BB.fmtSec(data.meta.pitLoss, 1) +
          ' export --sweep ' + BB.fmtSec(strat.sweep.from, 0) + ':' + BB.fmtSec(strat.sweep.to, 0) + ':' + BB.fmtSec(strat.sweep.step, 0) + '</code></div>');
    }

    function renderPlan() {
      const o = scen().optimal;
      const stints = o.stints.map(function (st) {
        return { laps: st.laps, color: model.compoundColor(st.compound), label: st.name + ' ' + st.laps,
                 title: st.name + ': laps ' + st.startLap + '–' + (st.startLap + st.laps - 1) + ', ' + BB.fmtClock(st.time) };
      });
      ui.setBody(root, 'st-plan',
        '<div style="margin-bottom:12px"><span class="chip opt">Optimal</span> <span class="chip">' + o.stops +
          (o.stops === 1 ? ' stop' : ' stops') + '</span></div>' +
        charts.stintBar({ stints: stints, total: N }) +
        '<div class="kv">' +
          '<div><div class="kv-l">Total time</div><div class="kv-v">' + BB.fmtClock(o.totalTime) + '</div></div>' +
          '<div><div class="kv-l">Stops at lap</div><div class="kv-v">' + (o.pitLaps.join(', ') || '–') + '</div></div>' +
          '<div><div class="kv-l">Green time</div><div class="kv-v">' + BB.fmtSec(o.greenTime, 2) + ' s</div></div>' +
          '<div><div class="kv-l">Pit time</div><div class="kv-v">' + BB.fmtSec(o.pitTime, 2) + ' s</div></div>' +
        '</div>' +
        '<table class="table" style="margin-top:16px"><thead><tr><th>#</th><th>Compound</th><th>Laps</th><th class="num">Length</th><th class="num">Stint time</th></tr></thead><tbody>' +
        o.stints.map(function (st, i) {
          return '<tr><td>' + (i + 1) + '</td><td><span class="swatch" style="background:' + model.compoundColor(st.compound) + '"></span>' + esc(st.name) +
            '</td><td>' + st.startLap + '–' + (st.startLap + st.laps - 1) + '</td><td class="num">' + st.laps + ' laps</td><td class="num">' + BB.fmtClock(st.time) + '</td></tr>';
        }).join('') + '</tbody></table>' +
        '<p class="muted" style="margin-top:10px">Stint time excludes the pit loss, which is added once per stop.</p>');
    }

    function planText(plan) {
      return plan.map(function (p) { return esc(model.compoundName(p.compound)) + '×' + p.laps; }).join(' · ');
    }

    function renderStops() {
      const rows = scen().stopCounts;
      const best = scen().optimal.stops;
      const items = rows.map(function (r) {
        const ok = r.feasible;
        return { label: r.stops + (r.stops === 1 ? ' stop' : ' stops'), value: ok ? r.report.deltaVsOptimum : 0,
                 color: !ok ? 'var(--muted)' : (r.stops === best ? 'var(--purple)' : 'var(--blue)'),
                 nub: ok, active: ok,
                 title: ok ? r.stops + ' stops: +' + BB.fmtSec(r.report.deltaVsOptimum, 2) + ' s vs the optimum' : r.stops + ' stops: infeasible (' + r.reason + ')' };
      });
      let html = charts.bars({ items: items, height: 190, label: 'Time lost against the optimum, by stop count' }) +
        '<p class="seg-l" style="margin:4px 0 10px">Time lost against the optimum, seconds</p>' +
        '<table class="table"><thead><tr><th>Stops</th><th class="num">Total time</th><th class="num">Δ optimum</th><th>Plan</th></tr></thead><tbody>';
      rows.forEach(function (r) {
        if (!r.feasible) {
          html += '<tr class="off"><td>' + r.stops + '</td><td class="num">–</td><td class="num">–</td><td title="' + esc(r.reason) + '">infeasible</td></tr>';
          return;
        }
        html += '<tr' + (r.stops === best ? ' class="best"' : '') + '><td>' + r.stops + '</td><td class="num">' + BB.fmtClock(r.report.totalTime) +
          '</td><td class="num">' + (r.stops === best ? 'optimum' : '+' + BB.fmtSec(r.report.deltaVsOptimum, 2) + ' s') + '</td><td>' + planText(r.report.plan) + '</td></tr>';
      });
      ui.setBody(root, 'st-stops', html + '</tbody></table>');
    }

    // ------------------------------------------------------ DP visualiser
    /** Everything the visualiser shows at `step`, read from the exported trace. */
    function dpState() {
      const s = scen(), bt = s.backtrack, B = bt.length;
      if (step <= N) {
        return { phase: 'fill', lap: step, cell: s.dp[step], resolved: [], unresolved: N };
      }
      const j = step - N;                       // stints resolved so far (1..B)
      const ends = [];
      let run = 0;
      bt.forEach(function (st) { run += st.laps; ends.push(run); });
      const idx = B - j;                        // the stint just resolved, counting from the end
      const from = idx > 0 ? ends[idx - 1] : 0;
      return { phase: 'back', j: j, idx: idx, end: ends[idx], from: from, stint: bt[idx],
               resolved: bt.slice(idx), unresolved: from, ends: ends };
    }

    function renderDP() {
      const s = scen(), st = dpState(), B = s.backtrack.length, total = totalSteps();

      // Which cells are filled, current, the source of the current cell, or on the path.
      const path = {};
      let current = -1, source = -1;
      if (st.phase === 'fill') {
        current = st.lap;
        if (st.cell && st.cell.from !== undefined) { source = st.cell.from; }
      } else {
        st.ends.forEach(function (e, i) { if (i >= st.idx) { path[e] = true; } });
        path[st.from] = true;
        current = st.end; source = st.from;
      }

      let cells = '';
      s.dp.forEach(function (c) {
        const filled = st.phase === 'back' || c.lap <= st.lap;
        const cls = 'dp-cell' + (filled ? ' filled' : '') + (c.lap === current ? ' current' : '') +
          (c.lap === source && c.lap !== current ? ' source' : '') + (path[c.lap] ? ' path' : '');
        cells += '<div class="' + cls + '"><span class="dp-i">' + c.lap + '</span><span class="dp-v">' +
          (filled && c.cost !== null ? BB.fmtSec(c.cost, 1) : '–') + '</span></div>';
      });

      // The explanation card for this step.
      let info = '';
      if (st.phase === 'fill' && st.lap === 0) {
        info = '<b>dp[0] = 0</b>: running zero laps costs nothing. Every other cell is the cheapest way to reach it from an earlier one.';
      } else if (st.phase === 'fill') {
        const c = st.cell;
        info = '<div class="dp-form"><b>dp[' + c.lap + ']</b> = min over compound c and stint length l of ' +
          '<span class="t">dp[' + c.lap + ' − l]</span> + <span class="t">stintCost[c][l]</span> + <span class="t">pit loss</span> ' +
          '<span class="muted">(pit loss only when it is not the first stint)</span></div>' +
          '<table class="table dp-cand"><thead><tr><th>Compound</th><th class="num">Best l</th><th class="num">Cost</th><th></th></tr></thead><tbody>' +
          c.byCompound.map(function (b, i) {
            const chosen = b && c.compound === i;
            return '<tr' + (chosen ? ' class="best"' : '') + '><td><span class="swatch" style="background:' + model.compoundColor(i) + '"></span>' + esc(model.compoundName(i)) + '</td>' +
              (b ? '<td class="num">' + b.laps + '</td><td class="num">' + BB.fmtSec(b.cost, 2) + '</td><td>' + (chosen ? '<span class="chip opt">chosen</span>' : '') + '</td>'
                 : '<td class="num">–</td><td class="num">–</td><td class="muted">no legal stint</td>') + '</tr>';
          }).join('') + '</tbody></table>' +
          dpBreakdown(c);
      } else {
        const stint = st.stint;
        info = '<div class="dp-form"><b>Backtrack ' + st.j + ' of ' + B + '</b>: choice[' + st.end + '] = <span class="swatch" style="background:' +
          model.compoundColor(stint.compound) + '"></span><b>' + esc(model.compoundName(stint.compound)) + '</b>, ' + stint.laps +
          ' laps. Step back to lap <b>' + st.from + '</b>.</div>' +
          (st.from === 0 ? '<p class="muted" style="margin-top:6px">Lap 0 reached: the plan is complete.</p>' : '');
      }

      // The plan as it is recovered: unresolved laps first, then the stints found so far.
      const parts = [];
      if (st.unresolved > 0) { parts.push({ laps: st.unresolved, color: 'var(--panel-2)', label: '?', title: st.unresolved + ' laps not yet resolved' }); }
      st.resolved.forEach(function (r) {
        parts.push({ laps: r.laps, color: model.compoundColor(r.compound), label: model.compoundName(r.compound) + ' ' + r.laps,
                     title: model.compoundName(r.compound) + ' ' + r.laps + ' laps' });
      });
      const planBar = st.phase === 'back' ? '<p class="seg-l" style="margin:14px 0 6px">Plan recovered by backtracking</p>' + charts.stintBar({ stints: parts, total: N }) : '';

      const label = st.phase === 'fill' ? 'Fill · dp[' + st.lap + ']' : 'Backtrack · stint ' + st.j + ' of ' + B;
      ui.setBody(root, 'st-dp',
        '<div class="dp-bar">' +
          '<div class="clock-btns">' +
            dpBtn('reset', 'Back to the start', '<path d="M6 5v14M18 5l-9 7 9 7z"/>') +
            dpBtn('back', 'Previous step', '<path d="M15 6l-6 6 6 6"/>') +
            dpBtn('play', playing ? 'Pause' : 'Play', playing ? '<path d="M8 5v14M16 5v14"/>' : '<path d="M8 5l11 7-11 7z"/>', true) +
            dpBtn('fwd', 'Next step', '<path d="M9 6l6 6-6 6"/>') +
            dpBtn('end', 'Skip to the finished plan', '<path d="M18 5v14M6 5l9 7-9 7z"/>') +
          '</div>' +
          ui.segmented('dpspeed', [{ value: '1', label: '1x' }, { value: '2', label: '2x' }, { value: '4', label: '4x' }], String(speed), 'Speed') +
          '<span class="chip ' + (st.phase === 'back' ? 'ok' : 'opt') + '">' + esc(label) + '</span>' +
          '<span class="seg-l">Step ' + step + ' / ' + total + '</span>' +
        '</div>' +
        '<div class="dp-grid">' + cells + '</div>' +
        '<div class="dp-info">' + info + '</div>' + planBar);
    }

    /** The arithmetic behind the chosen candidate: three exported numbers and the exported result. */
    function dpBreakdown(c) {
      if (c.compound === undefined) { return ''; }
      const prev = scen().dp[c.from].cost;
      const cost = table[c.compound][c.laps - 1];
      const pit = c.from > 0 ? scen().pitLoss : 0;
      return '<div class="dp-sum"><span class="t">dp[' + c.from + '] = ' + BB.fmtSec(prev, 2) + '</span> + ' +
        '<span class="t">stintCost[' + esc(model.compoundName(c.compound)) + '][' + c.laps + '] = ' + BB.fmtSec(cost, 2) + '</span> + ' +
        '<span class="t">pit loss = ' + BB.fmtSec(pit, 2) + '</span> → <b>dp[' + c.lap + '] = ' + BB.fmtSec(c.cost, 2) + '</b> ' +
        '<span class="muted">(stintCost is one prefix-sum lookup)</span></div>';
    }

    function dpBtn(act, label, path, primary) {
      return '<button class="btn' + (primary ? ' primary' : '') + '" type="button" data-dp="' + act + '" title="' + label + '" aria-label="' + label + '">' +
        '<svg viewBox="0 0 24 24" aria-hidden="true">' + path + '</svg></button>';
    }

    function renderGap() {
      const v = scen().verification;
      const ok = Math.abs(v.gap) <= data.meta.closureTolerance;
      ui.setBody(root, 'st-gap',
        '<div style="margin-bottom:12px"><span class="chip ' + (ok ? 'ok' : '') + '">' + (ok ? 'DP = brute force' : 'MISMATCH') + '</span> ' +
          '<span class="chip">' + scenarios.length + ' of ' + scenarios.length + ' scenarios checked</span></div>' +
        '<div class="kv">' +
          '<div><div class="kv-l">Verification race</div><div class="kv-v">' + v.laps + ' laps</div></div>' +
          '<div><div class="kv-l">DP total</div><div class="kv-v">' + BB.fmtSec(v.dpTotal, 3) + ' s</div></div>' +
          '<div><div class="kv-l">Brute force</div><div class="kv-v">' + BB.fmtSec(v.bruteForceTotal, 3) + ' s</div></div>' +
          '<div><div class="kv-l">Optimality gap</div><div class="kv-v" style="color:var(--green)">' + BB.fmtSec(v.gap, 3) + ' s</div></div>' +
        '</div>' +
        '<p class="muted" style="margin-top:12px">The brute force tries every way to split the short race into stints. It is exponential, so it only ever sees a short race; the DP does the full ' + N + '-lap race in O(N·C·M).</p>');
    }

    function renderSens() {
      const rows = strat.sweep.rows, s = scen();
      const line = charts.lines({
        series: [{ name: 'Optimal total time', color: 'var(--purple)', points: rows.map(function (r) { return [r.pitLoss, r.totalTime]; }) }],
        marker: s.pitLoss, xLabel: 'pit loss (s)', label: 'Optimal race time against pit loss', height: 190,
        yFmt: function (v) { return Math.round(v) + ' s'; }
      });
      const bars = charts.bars({
        items: rows.map(function (r, i) {
          return { label: i % 4 === 0 ? String(r.pitLoss) : '', value: r.stops, color: 'var(--blue)', active: Math.abs(r.pitLoss - s.pitLoss) < data.meta.timeEpsilon,
                   title: 'Pit loss ' + BB.fmtSec(r.pitLoss, 1) + ' s: ' + r.stops + (r.stops === 1 ? ' stop' : ' stops') + ' · ' + BB.fmtClock(r.totalTime) };
        }),
        integer: true, height: 170, label: 'Optimal stop count against pit loss'
      });
      ui.setBody(root, 'st-sens',
        line + '<p class="seg-l" style="margin:2px 0 10px">Optimal race time by pit loss</p>' +
        bars + '<p class="seg-l" style="margin-top:4px">Optimal stop count by pit loss (bright = selected)</p>');
    }

    function renderAll() { renderScenario(); renderPlan(); renderStops(); renderDP(); renderGap(); renderSens(); }

    // ------------------------------------------------------ playback
    function stopPlay() { if (timer !== null) { clearInterval(timer); timer = null; } playing = false; }
    function startPlay() {
      stopPlay();
      if (step >= totalSteps()) { step = 0; }
      playing = true;
      timer = setInterval(function () {
        if (step >= totalSteps()) { stopPlay(); renderDP(); return; }
        step += 1;
        if (step >= totalSteps()) { stopPlay(); }
        renderDP();
      }, STEP_MS / speed);
      renderDP();
    }

    // ------------------------------------------------------ events
    function onClick(ev) {
      const b = ev.target.closest('[data-dp]');
      if (b) {
        const act = b.getAttribute('data-dp');
        if (act === 'play') { if (playing) { stopPlay(); renderDP(); } else { startPlay(); } return; }
        stopPlay();
        if (act === 'reset') { step = 0; }
        else if (act === 'back') { step = Math.max(0, step - 1); }
        else if (act === 'fwd') { step = Math.min(totalSteps(), step + 1); }
        else if (act === 'end') { step = totalSteps(); }
        renderDP();
        return;
      }
      const seg = ev.target.closest('[data-seg="dpspeed"]');
      if (seg) {
        speed = Number(seg.getAttribute('data-value'));
        if (playing) { startPlay(); } else { renderDP(); }
      }
    }
    function onInput(ev) {
      if (ev.target.id !== 'st-range') { return; }
      stopPlay();
      sc = Number(ev.target.value);
      step = totalSteps();     // show the finished DP for the new scenario
      document.getElementById('st-readout').textContent = BB.fmtSec(scen().pitLoss, 1) + ' s';
      document.getElementById('st-which').textContent = 'Scenario ' + (sc + 1) + ' of ' + scenarios.length;
      renderPlan(); renderStops(); renderDP(); renderGap(); renderSens();
    }
    root.addEventListener('click', onClick);
    root.addEventListener('input', onInput);

    step = totalSteps();
    renderAll();

    return function unmount() {
      stopPlay();
      root.removeEventListener('click', onClick);
      root.removeEventListener('input', onInput);
    };
  }

  BB.pages = BB.pages || {};
  BB.pages.lab = {
    title: 'Strategy Lab',
    subtitle: 'The optimizer’s dynamic program, step by step, for every pit loss',
    clock: false,
    mount: mount
  };
})(window.BB = window.BB || {});
