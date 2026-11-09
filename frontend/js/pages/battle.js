/* Battle: pick plans from a catalogue and compare them with the optimum. Every figure is the
   engine's: each plan was costed by PitStrategy::evaluatePlan, against optimizers built at each
   pit loss, and raced against the optimum with the overtaking rule on. The page selects and draws. */
(function (BB) {
  'use strict';

  const esc = BB.esc, model = BB.model, ui = BB.ui, charts = BB.charts;

  const MAX_PICKED = 4;
  const PAGE_ROWS = 12;
  const SLOT_COLORS = ['var(--pink)', 'var(--blue)', 'var(--green)', '#22d3ee'];

  function mount(root, controls) {
    const store = BB.store;
    const data = model.data();
    const N = model.laps();
    const battle = data.battle;

    if (battle === null) {
      root.innerHTML = '<div class="panel"><div class="panel-b"><div class="empty">The plan catalogue is exported for races of 2 to 100 laps; this one has ' + N + '.</div></div></div>';
      return function unmount() {};
    }

    const plans = battle.plans;
    const optimum = plans[0];
    let picked = [];                       // plan ids, oldest first
    const colorOf = {};                    // plan id -> slot colour, stable while it stays picked
    let stopsFilter = 'all', compoundFilter = 'any', onlyNamed = false, page = 0;

    const stopCounts = Array.from(new Set(plans.map(function (p) { return p.stops; }))).sort(function (a, b) { return a - b; });

    function describe(p) {
      return p.stints.map(function (s) {
        return '<span class="swatch" style="background:' + model.compoundColor(s.compound) + '"></span>' + esc(model.compoundName(s.compound)) + ' ' + s.laps;
      }).join(' <span class="muted">·</span> ');
    }

    function pick(id) {
      const i = picked.indexOf(id);
      if (i >= 0) { picked.splice(i, 1); delete colorOf[id]; }
      else {
        if (picked.length >= MAX_PICKED) { delete colorOf[picked.shift()]; }
        const used = Object.keys(colorOf).map(function (k) { return colorOf[k]; });
        colorOf[id] = SLOT_COLORS.find(function (c) { return used.indexOf(c) < 0; });
        picked.push(id);
      }
      renderAll();
    }

    function defaultPicks() {
      picked = []; Object.keys(colorOf).forEach(function (k) { delete colorOf[k]; });
      const wanted = ['1-stop', '3-stop', 'all-MEDIUM'];
      const ids = [];
      wanted.forEach(function (n) { const p = plans.find(function (x) { return x.name === n; }); if (p && ids.indexOf(p.id) < 0) { ids.push(p.id); } });
      plans.forEach(function (p) { if (ids.length < 3 && p.id !== 0 && ids.indexOf(p.id) < 0) { ids.push(p.id); } });
      ids.slice(0, MAX_PICKED).forEach(function (id) {
        const used = Object.keys(colorOf).map(function (k) { return colorOf[k]; });
        colorOf[id] = SLOT_COLORS.find(function (c) { return used.indexOf(c) < 0; });
        picked.push(id);
      });
    }

    // The optimum plan is the best one at THIS race's pit loss; at other pit losses it can lose time.
    const label = function (p) { return p.id === 0 ? 'optimal (' + BB.fmtSec(data.meta.pitLoss, 0) + ' s)' : p.name; };
    const chosen = function () { return picked.map(function (id) { return plans[id]; }); };
    const slot = function (p) { return p.id === 0 ? 'var(--purple)' : colorOf[p.id]; };
    const rowsOf = function () { return [optimum].concat(chosen().filter(function (p) { return p.id !== 0; })); };

    root.innerHTML =
      '<div class="grid">' +
        ui.panel({ id: 'bt-cat', span: 7, title: 'Plan catalogue', algo: 'StrategyAnalytics.cpp · evaluateAgainst',
                   algoTip: 'The core’s standard candidates (the optimum, single-compound plans, the best plan per stop count) plus the best grid plans for each stop count, with stints in multiples of ' + battle.gridStep +
                            ' laps. Every plan is costed by PitStrategy::evaluatePlan, the same code path as the optimizer. Ranked by total time.' }) +
        ui.panel({ id: 'bt-compare', span: 5, title: 'Stints compared', algo: 'PitStrategy.cpp · evaluatePlan',
                   algoTip: 'The optimum on top, then your picks, on one lap axis. Bars are to scale in laps.' }) +
        ui.panel({ id: 'bt-metrics', span: 6, title: 'Time lost and where', algo: 'StrategyAnalytics.cpp · StrategyReport',
                   algoTip: 'Green and pit time split, compound use, and a two-car race against the optimum with the overtaking rule on (cleanAirVsTraffic).' }) +
        ui.panel({ id: 'bt-rank', span: 6, title: 'Gap to the optimum', algo: 'StrategyAnalytics.cpp · deltaVsOptimum',
                   algoTip: 'Total time minus the optimum’s, by the same arithmetic that totalled both.' }) +
        ui.panel({ id: 'bt-robust', span: 12, title: 'Robustness to the pit loss', algo: 'PitStrategy.cpp · optimizer per pit loss',
                   algoTip: 'Each point costs the plan with an optimizer built at that pit loss and subtracts that optimizer’s best total. Zero means the plan is optimal at that pit loss. Where two lines cross, which of those plans is better changes.' }) +
      '</div>';

    function renderControls() {
      controls.innerHTML = '<div class="seg" role="group" aria-label="Selection"><button type="button" data-bt="defaults">Default picks</button><button type="button" data-bt="clear">Clear</button></div>';
    }

    // ------------------------------------------------------------ catalogue
    function filtered() {
      return plans.filter(function (p) {
        if (stopsFilter !== 'all' && p.stops !== Number(stopsFilter)) { return false; }
        if (compoundFilter !== 'any' && !p.compoundUsage.some(function (u) { return u.compound === Number(compoundFilter) && u.laps > 0; })) { return false; }
        if (onlyNamed && p.source !== 'named') { return false; }
        return true;
      });
    }

    function renderCat() {
      const rows = filtered(), pages = Math.max(1, Math.ceil(rows.length / PAGE_ROWS));
      page = Math.min(page, pages - 1);
      const shown = rows.slice(page * PAGE_ROWS, (page + 1) * PAGE_ROWS);
      ui.setBody(root, 'bt-cat',
        '<div class="dp-bar" style="margin-bottom:10px">' +
          ui.segmented('bstops', [{ value: 'all', label: 'All' }].concat(stopCounts.map(function (n) { return { value: String(n), label: n + (n === 1 ? ' stop' : ' stops') }; })), stopsFilter, 'Stops') +
          '<label class="seg-l" for="bt-comp">Uses</label><select class="select" id="bt-comp" aria-label="Compound filter"><option value="any">Any compound</option>' +
            data.compounds.map(function (c, i) { return '<option value="' + i + '"' + (String(i) === compoundFilter ? ' selected' : '') + '>' + esc(c.name) + '</option>'; }).join('') + '</select>' +
          '<button type="button" class="tog' + (onlyNamed ? ' on' : '') + '" data-named="1" aria-pressed="' + onlyNamed + '">Named plans only</button>' +
        '</div>' +
        (shown.length === 0 ? '<div class="empty">No plan matches.</div>' :
          '<table class="table"><thead><tr><th class="num">#</th><th>Plan</th><th class="num">Stops</th><th class="num">Total</th><th class="num">Δ optimum</th></tr></thead><tbody>' +
          shown.map(function (p) {
            const on = picked.indexOf(p.id) >= 0;
            // The optimum is always in the comparison, so its row is not a control.
            const attrs = p.id === 0 ? ' title="The optimum is always shown"' :
              ' class="pick' + (on ? ' sel' : '') + '" data-plan="' + p.id + '" tabindex="0" title="' + (on ? 'Click to remove from the comparison' : 'Click to compare') + '"' +
              (on ? ' style="box-shadow:inset 3px 0 0 ' + slot(p) + '"' : '');
            return '<tr' + attrs + '><td class="num">' + (p.id + 1) + '</td><td><span class="chip ' + (p.id === 0 ? 'opt' : p.source === 'named' ? 'pit' : '') + '">' + esc(p.name) + '</span> ' + describe(p) + '</td>' +
              '<td class="num">' + p.stops + '</td><td class="num">' + BB.fmtClock(p.totalTime) + '</td><td class="num">' + (p.id === 0 ? 'optimum' : '+' + BB.fmtSec(p.deltaVsOptimum, 2) + ' s') + '</td></tr>';
          }).join('') + '</tbody></table>') +
        '<div class="dp-bar" style="margin-top:10px"><button class="btn" type="button" data-page="-1" title="Previous page" aria-label="Previous page"' + (page === 0 ? ' disabled' : '') + '><svg viewBox="0 0 24 24"><path d="M15 6l-6 6 6 6"/></svg></button>' +
          '<button class="btn" type="button" data-page="1" title="Next page" aria-label="Next page"' + (page >= pages - 1 ? ' disabled' : '') + '><svg viewBox="0 0 24 24"><path d="M9 6l6 6-6 6"/></svg></button>' +
          '<span class="seg-l">' + (rows.length ? 'Plans ' + (page * PAGE_ROWS + 1) + '–' + (page * PAGE_ROWS + shown.length) + ' of ' + rows.length : '0 plans') + ' · click up to ' + MAX_PICKED + ' to compare</span></div>');
    }

    // ------------------------------------------------------------ comparison
    function renderCompare() {
      const rows = rowsOf();
      ui.setBody(root, 'bt-compare', rows.map(function (p) {
        const stints = p.stints.map(function (s) {
          return { laps: s.laps, color: model.compoundColor(s.compound), label: model.compoundName(s.compound).charAt(0) + ' ' + s.laps,
                   title: model.compoundName(s.compound) + ': laps ' + s.firstLap + '–' + s.lastLap + ', ' + BB.fmtClock(s.time) };
        });
        return '<div style="margin-bottom:14px"><div class="dp-bar" style="margin-bottom:6px;gap:8px"><i class="swatch" style="background:' + slot(p) + '"></i><b>' + esc(label(p)) + '</b>' +
          '<span class="muted" style="font-size:12px">' + BB.fmtClock(p.totalTime) + (p.id === 0 ? ' · optimum' : ' · +' + BB.fmtSec(p.deltaVsOptimum, 2) + ' s') + '</span></div>' +
          charts.stintBar({ stints: stints, total: N }) + '</div>';
      }).join('') + (picked.filter(function (id) { return id !== 0; }).length === 0 ? '<p class="muted">Pick plans in the catalogue to put them under the optimum.</p>' : ''));
    }

    // ------------------------------------------------------------ metrics
    function renderMetrics() {
      const rows = rowsOf();
      ui.setBody(root, 'bt-metrics',
        '<table class="table"><thead><tr><th>Plan</th><th class="num">Lost</th><th class="num">%</th><th class="num">Green</th><th class="num">Pit time</th><th>Compounds</th></tr></thead><tbody>' +
        rows.map(function (p) {
          return '<tr><td><i class="swatch" style="background:' + slot(p) + '"></i>' + esc(label(p)) + '</td><td class="num">' + (p.id === 0 ? '–' : '+' + BB.fmtSec(p.deltaVsOptimum, 2) + ' s') + '</td>' +
            '<td class="num">' + (p.id === 0 ? '–' : BB.fmtSec(p.percentVsOptimum, 3) + '%') + '</td><td class="num">' + BB.fmtSec(p.greenTime, 1) + ' s</td>' +
            '<td class="num">' + BB.fmtSec(p.pitTime, 1) + ' s <span class="muted">(' + p.stops + ')</span></td><td>' +
            p.compoundUsage.map(function (u) { return '<span class="swatch" style="background:' + model.compoundColor(u.compound) + '"></span>' + u.laps; }).join(' ') + '</td></tr>';
        }).join('') + '</tbody></table>' +
        '<p class="seg-l" style="margin:16px 0 6px">Two-car race against the optimum, overtaking rule on</p>' +
        '<table class="table"><thead><tr><th>Plan</th><th class="num">Clean air</th><th class="num">With traffic</th><th class="num">Lost to traffic</th><th class="num">Finish</th></tr></thead><tbody>' +
        rows.filter(function (p) { return p.headToHead !== null; }).map(function (p) {
          const h = p.headToHead;
          return '<tr><td><i class="swatch" style="background:' + slot(p) + '"></i>' + esc(p.name) + '</td><td class="num">' + BB.fmtClock(h.cleanTime) + '</td><td class="num">' + BB.fmtClock(h.trafficTime) +
            '</td><td class="num">' + BB.fmtSec(h.trafficLoss, 2) + ' s</td><td class="num">' + h.cleanPosition + ' → ' + h.trafficPosition + '</td></tr>';
        }).join('') + '</tbody></table>' +
        '<p class="muted" style="margin-top:8px;font-size:12px">Clean air is the plan’s own time. The finish is the plan’s place in a two-car race against the optimum, without and with the rule.</p>');
    }

    // ------------------------------------------------------------ gap bars
    function renderRank() {
      const rows = rowsOf().slice().sort(function (a, b) { return a.id - b.id; });
      const max = rows.reduce(function (m, p) { return Math.max(m, p.deltaVsOptimum); }, 0) || 1;
      ui.setBody(root, 'bt-rank', rows.map(function (p, i) {
        const w = p.id === 0 ? 0 : Math.max(p.deltaVsOptimum / max * 100, 1.5);
        return '<div class="hbar"><div class="hbar-l"><span class="muted">' + (i + 1) + '</span> <i class="swatch" style="background:' + slot(p) + '"></i>' + esc(label(p)) + '</div>' +
          '<div class="hbar-t"><div class="hbar-f" style="width:' + (p.id === 0 ? 1.5 : w).toFixed(1) + '%;background:' + slot(p) + '" title="' + esc(label(p)) + '"></div></div>' +
          '<div class="hbar-v">' + (p.id === 0 ? 'optimum' : '+' + BB.fmtSec(p.deltaVsOptimum, 2) + ' s') + '</div></div>';
      }).join('') + '<p class="muted" style="margin-top:10px;font-size:12px">Ordered by total time. Bar length is the gap to the optimum, in seconds, over this race.</p>');
    }

    // ------------------------------------------------------------ robustness
    function renderRobust() {
      const rows = rowsOf();
      const series = rows.map(function (p) {
        return { name: label(p), color: slot(p), points: battle.pitLosses.map(function (loss, k) { return [loss, p.deltaByPitLoss[k]]; }) };
      });
      ui.setBody(root, 'bt-robust',
        charts.lines({ series: series, marker: data.meta.pitLoss, xLabel: 'pit loss (s)', label: 'Seconds lost to the optimum at each pit loss', width: 1100, height: 280,
                       yFmt: function (v) { return v.toFixed(0) + ' s'; } }) +
        '<div class="legend">' + rows.map(function (p) { return '<span><i class="swatch" style="background:' + slot(p) + '"></i>' + esc(label(p)) + '</span>'; }).join('') +
        '<span>Dashed line = this race’s pit loss (' + BB.fmtSec(data.meta.pitLoss, 1) + ' s).</span></div>' +
        '<p class="muted" style="margin-top:8px;font-size:12px">Each point costs the plan with an optimizer built at that pit loss and subtracts that optimizer’s best total. Zero means the plan is optimal there; where two lines cross, the better of those plans changes. ' +
        'The optimum plan is the best one for this race’s pit loss only, so it climbs off zero at any other.</p>');
    }

    function renderAll() { renderCat(); renderCompare(); renderMetrics(); renderRank(); renderRobust(); }

    // ------------------------------------------------------------ events
    function onControlsClick(ev) {
      const b = ev.target.closest('[data-bt]');
      if (!b) { return; }
      if (b.getAttribute('data-bt') === 'clear') { picked = []; Object.keys(colorOf).forEach(function (k) { delete colorOf[k]; }); }
      else { defaultPicks(); }
      renderAll();
    }
    function onClick(ev) {
      const row = ev.target.closest('tr[data-plan]');
      if (row) { pick(Number(row.getAttribute('data-plan'))); return; }
      const sf = ev.target.closest('[data-seg="bstops"]');
      if (sf) { stopsFilter = sf.getAttribute('data-value'); page = 0; renderCat(); return; }
      if (ev.target.closest('[data-named]')) { onlyNamed = !onlyNamed; page = 0; renderCat(); return; }
      const pg = ev.target.closest('[data-page]');
      if (pg && !pg.disabled) { page += Number(pg.getAttribute('data-page')); renderCat(); }
    }
    function onKey(ev) {
      if (ev.key !== 'Enter' && ev.key !== ' ') { return; }
      const row = ev.target.closest('tr[data-plan]');
      if (row) { ev.preventDefault(); pick(Number(row.getAttribute('data-plan'))); }
    }
    function onChange(ev) {
      if (ev.target.id === 'bt-comp') { compoundFilter = ev.target.value; page = 0; renderCat(); }
    }
    controls.addEventListener('click', onControlsClick);
    root.addEventListener('click', onClick);
    root.addEventListener('keydown', onKey);
    root.addEventListener('change', onChange);

    defaultPicks();
    renderControls();
    renderAll();

    return function unmount() {
      controls.removeEventListener('click', onControlsClick);
      root.removeEventListener('click', onClick);
      root.removeEventListener('keydown', onKey);
      root.removeEventListener('change', onChange);
    };
  }

  BB.pages = BB.pages || {};
  BB.pages.battle = {
    title: 'Battle',
    subtitle: 'Put plans against the optimum: time lost, where, and how the answer moves with the pit loss',
    clock: false,
    mount: mount
  };
})(window.BB = window.BB || {});
