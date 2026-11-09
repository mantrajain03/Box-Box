/* Race Sim: cars on a 2D track map. The engine supplies when each car crosses the line,
   when it is in the pits and how long it runs; this page only draws those facts.
   Between line crossings a car is drawn moving at uniform speed: the engine models lap
   times, not speed within a lap. The outline is a generated drawing, not a real circuit. */
(function (BB) {
  'use strict';

  const esc = BB.esc, model = BB.model, ui = BB.ui;

  const SECTOR_COLORS = ['var(--blue)', 'var(--purple)', 'var(--green)', 'var(--yellow)', 'var(--pink)'];
  const ZOOM = 2.4;        // how far the camera zooms in when following a car
  const LANE = 8;          // sideways spacing between cars so identical cars stay visible
  const FEED_LENGTH = 12;
  const FEED_TYPES = { PitEntry: 'pit', Overtake: 'pass', TrafficHold: 'hold', RaceFinish: 'fin' };

  /** A point `progress` (0..1) along a polyline, plus the unit normal there. */
  function pointAt(pts, progress, closed) {
    const n = pts.length;
    const span = closed ? n : n - 1;
    const f = Math.min(Math.max(progress, 0), 1) * span;
    let i = Math.floor(f);
    if (!closed && i >= n - 1) { i = n - 2; }
    const u = f - i;
    const a = pts[i % n], b = pts[(i + 1) % n];
    const dx = b[0] - a[0], dy = b[1] - a[1];
    const len = Math.hypot(dx, dy) || 1;
    return { x: a[0] + dx * u, y: a[1] + dy * u, nx: -dy / len, ny: dx / len };
  }

  /** Where one car is at race time t, from its exported lap rows (`tl`, lap 1 first). */
  function carAt(tl, t) {
    let start = 0;
    for (let i = 0; i < tl.length; i++) {
      const e = tl[i];
      if (t < e.cross) {
        const span = e.cross - start;
        return { mode: 'track', p: span > 0 ? Math.max(0, (t - start) / span) : 0, e: e };
      }
      if (e.pitted && t < e.time) {
        const span = e.time - e.cross;
        return { mode: 'pit', q: span > 0 ? (t - e.cross) / span : 0, e: e };
      }
      start = e.time;
    }
    return { mode: 'done', e: tl[tl.length - 1] };
  }

  const pathOf = function (pts, closed) {
    return 'M' + pts.map(function (p) { return p[0] + ' ' + p[1]; }).join('L') + (closed ? 'Z' : '');
  };

  function mount(root, controls) {
    const store = BB.store;
    const data = model.data();
    const N = model.laps();
    const outline = data.track.outline;
    const W = outline.width, H = outline.height;

    let run, cars, shownLap, shownFeed, states;

    root.innerHTML =
      '<div class="tiles four" id="rs-tiles"></div>' +
      '<div class="grid">' +
        ui.panel({ id: 'rs-map', span: 8, title: 'Track', algo: 'RaceSimulator.cpp · lap crossings',
                   algoTip: 'When each car crosses the line, stops, and finishes comes from RaceSimulator. Between crossings a car is drawn at uniform speed: the engine models lap times, not speed within a lap. The outline is a generated drawing, not a real circuit.' }) +
        '<div class="span-4 stack">' +
          ui.panel({ id: 'rs-board', span: 12, title: 'Leaderboard', algo: 'Leaderboard.cpp · min-heap',
                     algoTip: 'Classification from RaceSimulator::standings(), which drains a hand-written binary min-heap. O(D log D) per lap.' }) +
          ui.panel({ id: 'rs-feed', span: 12, title: 'Race events', algo: 'EventPriorityQueue.cpp · heap',
                     algoTip: 'Events leave a hand-written priority queue in time order (then type, car number, sequence) and are validated by the pit-stop state machine. The engine charges a stop as one lump, so a stop is logged when the car rejoins, not when it enters the lane.' }) +
        '</div>' +
      '</div>';

    // ----------------------------------------------------------- map
    function buildMap() {
      const pts = outline.points, n = pts.length;
      let s = '<svg id="rs-svg" class="trackmap" viewBox="0 0 ' + W + ' ' + H + '" role="img" aria-label="Track map with cars">';
      s += '<path d="' + pathOf(pts, true) + '" fill="none" stroke="#2b2b3d" stroke-width="42" stroke-linejoin="round"/>' +
           '<path d="' + pathOf(pts, true) + '" fill="none" stroke="#161621" stroke-width="36" stroke-linejoin="round"/>';
      s += '<path d="' + pathOf(outline.pitLane, false) + '" fill="none" stroke="#3d3d57" stroke-width="20" stroke-linecap="round"/>' +
           '<path d="' + pathOf(outline.pitLane, false) + '" fill="none" stroke="#1d1d2b" stroke-width="14" stroke-linecap="round"/>';

      const starts = outline.sectorStart;
      starts.forEach(function (from, k) {
        const to = k + 1 < starts.length ? starts[k + 1] : n;
        if (to <= from) { return; }
        const seg = [];
        for (let i = from; i <= to; i++) { seg.push(pts[i % n]); }
        s += '<path d="' + pathOf(seg, false) + '" fill="none" stroke="' + SECTOR_COLORS[k % SECTOR_COLORS.length] +
             '" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round" opacity=".9"/>';
        const label = pts[from % n];
        const ox = label[0] - W / 2, oy = label[1] - H / 2, len = Math.hypot(ox, oy) || 1;
        s += '<text class="map-l" x="' + (label[0] + ox / len * 32).toFixed(1) + '" y="' + (label[1] + oy / len * 32 + 4).toFixed(1) +
             '" text-anchor="middle" style="fill:' + SECTOR_COLORS[k % SECTOR_COLORS.length] + '">' +
             esc(data.track.sectors[k] ? data.track.sectors[k].name : 'S' + (k + 1)) + '</text>';
      });

      const a = outline.startLine[0], b = outline.startLine[1];
      s += '<line x1="' + a[0] + '" y1="' + a[1] + '" x2="' + b[0] + '" y2="' + b[1] + '" stroke="#fff" stroke-width="4"/>';
      s += '<g id="rs-cars"></g></svg>';

      const legend =
        data.track.sectors.map(function (sec, k) {
          return '<span><i class="swatch" style="background:' + SECTOR_COLORS[k % SECTOR_COLORS.length] + '"></i>' + esc(sec.name) + '</span>';
        }).join('') +
        '<span>Ring = tyre compound:</span>' +
        data.compounds.map(function (c, i) {
          return '<span><i class="swatch" style="background:' + model.compoundColor(i) + '"></i>' + esc(c.name) + '</span>';
        }).join('') +
        '<span>Click a car in the leaderboard to follow it.</span>';
      ui.setBody(root, 'rs-map', s + '<div class="legend">' + legend + '</div>');
    }

    function buildCars() {
      const group = root.querySelector('#rs-cars');
      cars = run.drivers.map(function (d, index) {
        const tl = run.laps.map(function (l) { return l.order.find(function (e) { return e.car === d.car; }); });
        const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');
        g.setAttribute('class', 'car');
        g.setAttribute('data-car', d.car);
        g.innerHTML =
          '<circle class="car-halo" r="19"/>' +
          '<circle class="car-dot" r="10" fill="' + model.carColor(d.car) + '" stroke-width="4"/>' +
          '<text class="car-no" y="3.8" text-anchor="middle">' + d.car + '</text>' +
          '<text class="car-pit" y="-17" text-anchor="middle">PIT</text>' +
          '<title>#' + d.car + ' ' + esc(d.name) + '</title>';
        group.appendChild(g);
        return { car: d.car, index: index, g: g, dot: g.querySelector('.car-dot'), pit: g.querySelector('.car-pit'),
                 tl: tl, x: 0, y: 0 };
      });
    }

    /** Move every car (and the camera) to race time t. Positions only; no state is derived. */
    function updateCars() {
      const s = store.get(), t = s.t, last = cars.length - 1;
      states = cars.map(function (c) {
        const st = carAt(c.tl, t);
        const lane = (c.index - last / 2) * LANE;
        let pos, ring, dim = false;
        if (st.mode === 'pit') {
          pos = pointAt(outline.pitLane, st.q, false);
          ring = model.compoundColor(st.e.fitted);
        } else if (st.mode === 'done') {
          pos = pointAt(outline.points, 0, true);
          ring = model.compoundColor(st.e.fitted);
          dim = true;
        } else {
          pos = pointAt(outline.points, st.p, true);
          ring = model.compoundColor(st.e.compound);
        }
        c.x = pos.x + pos.nx * lane;
        c.y = pos.y + pos.ny * lane;
        c.g.setAttribute('transform', 'translate(' + c.x.toFixed(1) + ' ' + c.y.toFixed(1) + ')');
        c.dot.setAttribute('stroke', ring);
        c.g.classList.toggle('dim', dim);
        c.g.classList.toggle('focus', s.car === c.car);
        c.pit.style.display = st.mode === 'pit' ? '' : 'none';
        return { car: c.car, mode: st.mode };
      });

      // Camera: the whole map, or a zoomed window centred on the followed car.
      const svg = root.querySelector('#rs-svg');
      const target = s.car === null ? undefined : cars.find(function (c) { return c.car === s.car; });
      if (target) {
        const w = W / ZOOM, h = H / ZOOM;
        const x = Math.min(Math.max(target.x - w / 2, 0), W - w);
        const y = Math.min(Math.max(target.y - h / 2, 0), H - h);
        svg.setAttribute('viewBox', x.toFixed(1) + ' ' + y.toFixed(1) + ' ' + w.toFixed(1) + ' ' + h.toFixed(1));
      } else {
        svg.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
      }
    }

    /** Leader on top: SVG paints in document order, so append the back markers first. */
    function stackCars(order) {
      const group = root.querySelector('#rs-cars');
      order.slice().reverse().forEach(function (e) {
        const c = cars.find(function (x) { return x.car === e.car; });
        if (c) { group.appendChild(c.g); }
      });
    }

    // ------------------------------------------------------- panels
    function completedLaps() { return model.lapAt(run, store.get().t); }

    function renderControls() {
      const s = store.get();
      controls.innerHTML = ui.segmented('run', [
        { value: 'overtakeOff', label: 'OFF' }, { value: 'overtakeOn', label: 'ON' }
      ], s.run, 'Overtake rule') +
        '<label class="seg-l" for="rs-follow">Follow</label>' +
        '<select class="select" id="rs-follow" aria-label="Follow a car"><option value="">Whole track</option>' +
        run.drivers.map(function (d) {
          return '<option value="' + d.car + '"' + (s.car === d.car ? ' selected' : '') + '>#' + d.car + ' ' + esc(d.name) + '</option>';
        }).join('') + '</select>';
    }

    function renderTiles() {
      const s = store.get(), done = completedLaps();
      const row = done > 0 ? run.laps[done - 1] : null;
      const inPit = states.filter(function (x) { return x.mode === 'pit'; });
      const leader = row ? row.order[0].car : null;
      document.getElementById('rs-tiles').innerHTML =
        ui.tile('Lap', Math.min(N, done + (done >= N ? 0 : 1)) + ' / ' + N, done >= N ? 'leader has finished' : 'leader’s lap', 'var(--blue)') +
        ui.tile('Race clock', BB.fmtClock(s.t), 'of ' + BB.fmtClock(run.duration), 'var(--purple)') +
        ui.tile('Leader', leader === null ? '–' : '#' + leader, leader === null ? 'on the grid' : model.carName(run, leader), 'var(--green)') +
        ui.tile('In the pits', String(inPit.length),
                inPit.length ? inPit.map(function (x) { return '#' + x.car; }).join(' ') : 'none', 'var(--pink)');
    }

    function renderBoard() {
      const s = store.get(), done = completedLaps();
      let rows;
      if (done > 0) {
        rows = run.laps[done - 1].order;
      } else {
        rows = run.drivers.map(function (d, i) {
          return { car: d.car, position: i + 1, gap: 0, fitted: d.plan[0].compound, fittedAge: 0, pitted: false };
        });
      }
      let html = '<table class="table"><thead><tr><th>P</th><th>Car</th><th>Tyre</th><th class="num">Gap</th></tr></thead><tbody>';
      rows.forEach(function (e) {
        html += '<tr class="pick' + (s.car === e.car ? ' sel' : '') + '" data-car="' + e.car + '" tabindex="0" title="Click to follow this car">' +
          '<td class="num">' + e.position + '</td>' +
          '<td><span class="swatch" style="background:' + model.carColor(e.car) + '"></span><span class="carno">' + e.car + '</span> ' + esc(model.carName(run, e.car)) + '</td>' +
          '<td><span class="swatch" style="background:' + model.compoundColor(e.fitted) + '"></span>' + esc(model.compoundName(e.fitted)) +
            ' <span class="chip">' + e.fittedAge + '</span>' + (e.pitted ? ' <span class="chip pit">pit</span>' : '') + '</td>' +
          '<td class="num">' + (e.position === 1 ? 'LEADER' : BB.fmtGap(e.gap)) + '</td></tr>';
      });
      html += '</tbody></table><p class="seg-l" style="margin-top:10px">' +
        (done > 0 ? 'Classification at the end of lap ' + done : 'Grid order') + '</p>';
      ui.setBody(root, 'rs-board', html);
    }

    function feedText(ev) {
      const name = function (car) { return '#' + car + ' ' + esc(model.carName(run, car)); };
      if (ev.type === 'PitEntry') {
        const change = run.events.find(function (x) { return x.type === 'CompoundChange' && x.car === ev.car && x.lap === ev.lap; });
        return name(ev.car) + ' pit stop' + (change ? ' → ' + esc(model.compoundName(change.to)) : '');
      }
      if (ev.type === 'Overtake') { return name(ev.car) + ' passes #' + ev.other; }
      if (ev.type === 'TrafficHold') { return name(ev.car) + ' held behind #' + ev.other + ' · −' + BB.fmtSec(ev.loss, 1) + ' s'; }
      return name(ev.car) + ' takes the flag';
    }

    function feedEvents() {
      const t = store.get().t;
      return run.events.filter(function (ev) { return FEED_TYPES[ev.type] && ev.time <= t; });
    }

    function renderFeed() {
      const all = feedEvents();
      shownFeed = all.length;
      const items = all.slice(-FEED_LENGTH).reverse();
      const labels = { pit: 'pit', pass: 'pass', hold: 'hold', fin: 'finish' };
      ui.setBody(root, 'rs-feed', items.length === 0
        ? '<div class="empty">No events yet.</div>'
        : '<ul class="feed">' + items.map(function (ev) {
            return '<li><span class="chip ' + FEED_TYPES[ev.type] + '">' + labels[FEED_TYPES[ev.type]] + '</span>' +
              '<span class="feed-t">lap ' + ev.lap + '</span><span>' + feedText(ev) + '</span></li>';
          }).join('') + '</ul>');
    }

    /** Everything that depends on the clock. Cheap parts every frame, panels only when they change. */
    function tick() {
      updateCars();
      const done = completedLaps();
      if (done !== shownLap) {
        shownLap = done;
        renderBoard();
        if (done > 0) { stackCars(run.laps[done - 1].order); }
      }
      if (feedEvents().length !== shownFeed) { renderFeed(); }
      renderTiles();
    }

    function rebuild() {
      run = model.run();
      if (store.get().t > run.duration) { store.set({ t: run.duration }); }
      shownLap = -1; shownFeed = -1;
      renderControls();
      buildMap();
      buildCars();
      tick();
      renderFeed();
    }

    // ------------------------------------------------------- events
    function onControlsClick(ev) {
      const b = ev.target.closest('[data-seg="run"]');
      if (b) { store.set({ run: b.getAttribute('data-value') }); }
    }
    function onControlsChange(ev) {
      if (ev.target.id === 'rs-follow') { store.set({ car: ev.target.value === '' ? null : Number(ev.target.value) }); }
    }
    function toggleCar(tr) {
      const car = Number(tr.getAttribute('data-car'));
      store.set({ car: store.get().car === car ? null : car });
    }
    function onBoardClick(ev) {
      const tr = ev.target.closest('tr[data-car]');
      if (tr) { toggleCar(tr); }
    }
    function onBoardKey(ev) {
      if (ev.key !== 'Enter' && ev.key !== ' ') { return; }
      const tr = ev.target.closest('tr[data-car]');
      if (tr) { ev.preventDefault(); toggleCar(tr); }
    }
    controls.addEventListener('click', onControlsClick);
    controls.addEventListener('change', onControlsChange);
    const board = root.querySelector('#rs-board');
    board.addEventListener('click', onBoardClick);
    board.addEventListener('keydown', onBoardKey);

    const unsubscribe = store.subscribe(function (changed) {
      if (changed.indexOf('run') >= 0) { rebuild(); return; }
      if (changed.indexOf('car') >= 0) { renderControls(); renderBoard(); }
      if (changed.indexOf('t') >= 0 || changed.indexOf('car') >= 0) { tick(); }
    });

    rebuild();

    return function unmount() {
      unsubscribe();
      controls.removeEventListener('click', onControlsClick);
      controls.removeEventListener('change', onControlsChange);
    };
  }

  BB.pages = BB.pages || {};
  BB.pages.sim = {
    title: 'Race Sim',
    subtitle: 'Cars drawn from the engine’s line crossings and pit windows',
    clock: 'time',
    mount: mount
  };
})(window.BB = window.BB || {});
