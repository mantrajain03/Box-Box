/* Shell, router and the shared race clock. Pages plug in through BB.pages.
   A page declares clock:true (steps by lap) or clock:'time' (continuous race seconds). */
(function (BB) {
  'use strict';

  const esc = BB.esc;

  /** Race seconds that pass per real second at 1x on a continuous-clock page. */
  const SIM_RATE = 30;
  /** Within this many seconds of a lap boundary, "previous lap" means the one before it. */
  const BOUNDARY_EPS = 0.05;

  const ICONS = {
    command: '<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/>',
    sim: '<circle cx="12" cy="12" r="9"/><path d="M10 8l6 4-6 4z"/>',
    lab: '<path d="M9 3h6M10 3v6l-5 9a2 2 0 0 0 2 3h10a2 2 0 0 0 2-3l-5-9V3"/>',
    tires: '<circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="3"/><path d="M12 3v6M12 15v6M3 12h6M15 12h6"/>',
    drivers: '<circle cx="12" cy="8" r="4"/><path d="M4 21c1-6 15-6 16 0"/>',
    pit: '<circle cx="12" cy="13" r="8"/><path d="M12 9v4l3 2M9 2h6"/>',
    battle: '<path d="M4 20V11M10 20V4M16 20v-6M22 20H2"/>',
    algo: '<path d="M4 5h5v5H4zM15 14h5v5h-5zM9 7.5h3a3 3 0 0 1 3 3V14"/>'
  };

  const NAV = [
    { id: 'command', label: 'Command', hash: '#/command', key: '1' },
    { id: 'sim', label: 'Race Sim', hash: '#/sim', key: '2' },
    { id: 'lab', label: 'Strategy Lab', hash: '#/lab', key: '3' },
    { id: 'tires', label: 'Tires', hash: '#/tires', key: '4' },
    { id: 'drivers', label: 'Drivers', hash: '#/drivers', key: '5' },
    { id: 'pit', label: 'Pit & Events', hash: '#/pit', key: '6' },
    { id: 'battle', label: 'Battle', hash: '#/battle', key: '7' },
    { id: 'algo', label: 'Algorithms', hash: '#/algo', key: '8' }
  ];

  const CLOCK_ICONS = {
    first: '<path d="M6 5v14M18 5l-9 7 9 7z"/>',
    prev: '<path d="M15 6l-6 6 6 6"/>',
    play: '<path d="M8 5l11 7-11 7z"/>',
    pause: '<path d="M8 5v14M16 5v14"/>',
    next: '<path d="M9 6l6 6-6 6"/>',
    last: '<path d="M18 5v14M6 5l9 7-9 7z"/>'
  };

  const svg = function (inner) { return '<svg viewBox="0 0 24 24" aria-hidden="true">' + inner + '</svg>'; };
  const isReady = function (id) { return !!(BB.pages && BB.pages[id]); };
  const currentDef = function () { return BB.pages[BB.store.get().page]; };
  const isTimeMode = function () { const d = currentDef(); return !!d && d.clock === 'time'; };

  let unmountPage = null;
  let timer = null;       // setInterval id (lap-stepping pages)
  let frameId = null;     // requestAnimationFrame id (continuous-clock pages)
  let lastFrame = 0;

  // ------------------------------------------------------------------ shell
  function fatal(title, message) {
    document.getElementById('app').innerHTML =
      '<div class="fatal"><h1>' + esc(title) + '</h1>' + message + '</div>';
  }

  function buildShell() {
    const d = BB.model.data();
    const rail = NAV.map(function (n) {
      if (isReady(n.id)) {
        return '<a class="rail-item" data-nav="' + n.id + '" href="' + n.hash + '" title="' + esc(n.label) + ' (' + n.key + ')">' +
               svg(ICONS[n.id]) + '<span>' + esc(n.label) + '</span></a>';
      }
      return '<span class="rail-item disabled" aria-disabled="true" title="' + esc(n.label) + ' — not built yet">' +
             svg(ICONS[n.id]) + '<span>' + esc(n.label) + '</span><span class="soon">SOON</span></span>';
    }).join('');

    document.getElementById('app').innerHTML =
      '<div class="shell">' +
        '<header class="topbar">' +
          '<div class="logo"><div class="logo-mark">' + svg('<path d="M5 19L12 5l7 14M8.5 14h7"/>') + '</div>BOX<b>BOX</b></div>' +
          '<span class="chip">' + esc(d.track.name) + ' · ' + d.meta.laps + ' laps</span>' +
          '<div class="spacer"></div>' +
          '<div class="status"><i class="dot"></i><span>Offline <span class="long">· C++17 engine · bundle v' + d.meta.version + '</span></span></div>' +
        '</header>' +
        '<nav class="rail" aria-label="Pages">' + rail + '</nav>' +
        '<main class="main">' +
          '<div class="page-h"><div><h1 id="page-title"></h1><p class="sub" id="page-sub"></p></div>' +
          '<div class="page-controls" id="page-controls"></div></div>' +
          '<div id="page"></div>' +
        '</main>' +
        '<footer class="clock" id="clock" hidden>' +
          '<div class="clock-btns">' +
            '<button class="btn" type="button" data-act="first" title="Start (Home)" aria-label="Start">' + svg(CLOCK_ICONS.first) + '</button>' +
            '<button class="btn" type="button" data-act="prev" title="Previous lap (←)" aria-label="Previous lap">' + svg(CLOCK_ICONS.prev) + '</button>' +
            '<button class="btn primary" type="button" data-act="play" title="Play / pause (Space)" aria-label="Play">' + svg(CLOCK_ICONS.play) + '</button>' +
            '<button class="btn" type="button" data-act="next" title="Next lap (→)" aria-label="Next lap">' + svg(CLOCK_ICONS.next) + '</button>' +
            '<button class="btn" type="button" data-act="last" title="Finish (End)" aria-label="Finish">' + svg(CLOCK_ICONS.last) + '</button>' +
          '</div>' +
          '<div id="clock-speed"></div>' +
          '<div class="clock-lap"><input id="clock-range" type="range" min="1" max="' + d.meta.laps + '" step="1" value="1" aria-label="Race position"></div>' +
          '<div class="clock-read" id="clock-read"></div>' +
        '</footer>' +
      '</div>';
  }

  // ------------------------------------------------------------------ clock
  function lapFor(t) { return Math.max(1, BB.model.lapAt(BB.model.run(), t)); }

  function stopTimer() {
    if (timer !== null) { clearInterval(timer); timer = null; }
    if (frameId !== null) { cancelAnimationFrame(frameId); frameId = null; }
  }

  function frame(ts) {
    frameId = null;
    const s = BB.store.get();
    if (!s.playing || !isTimeMode()) { return; }
    const dt = lastFrame ? Math.min((ts - lastFrame) / 1000, 0.25) : 0;
    lastFrame = ts;
    const duration = BB.model.run().duration;
    const t = s.t + dt * SIM_RATE * s.speed;
    if (t >= duration) {
      BB.store.set({ t: duration, lap: lapFor(duration), playing: false });
      return;
    }
    BB.store.set({ t: t, lap: lapFor(t) });
    frameId = requestAnimationFrame(frame);
  }

  function syncTimer() {
    stopTimer();
    const s = BB.store.get();
    if (!s.playing) { return; }
    if (isTimeMode()) {
      lastFrame = 0;
      frameId = requestAnimationFrame(frame);
      return;
    }
    timer = setInterval(function () {
      const st = BB.store.get(), N = BB.model.laps();
      if (st.lap >= N) { BB.store.set({ playing: false }); return; }
      BB.store.set({ lap: st.lap + 1, playing: st.lap + 1 < N });
    }, 900 / s.speed);
  }

  function renderPlay() {
    const s = BB.store.get();
    const play = document.querySelector('#clock [data-act="play"]');
    if (!play) { return; }
    play.innerHTML = svg(s.playing ? CLOCK_ICONS.pause : CLOCK_ICONS.play);
    play.setAttribute('aria-label', s.playing ? 'Pause' : 'Play');
  }

  function renderSpeed() {
    document.getElementById('clock-speed').innerHTML = BB.ui.segmented('speed',
      [{ value: '1', label: '1x' }, { value: '2', label: '2x' }, { value: '4', label: '4x' }],
      String(BB.store.get().speed), 'Speed');
  }

  /** Slider range and readout. Cheap enough to run on every frame. */
  function renderPosition() {
    const s = BB.store.get(), N = BB.model.laps();
    const range = document.getElementById('clock-range');
    const read = document.getElementById('clock-read');
    if (isTimeMode()) {
      const run = BB.model.run();
      range.min = '0'; range.max = String(run.duration); range.step = '0.1';
      range.value = String(s.t);
      const done = BB.model.lapAt(run, s.t);
      read.innerHTML = 'Lap <b>' + Math.min(N, done + (done >= N ? 0 : 1)) + '</b> / ' + N +
        ' · race time <b>' + BB.fmtClock(s.t) + '</b>';
    } else {
      range.min = '1'; range.max = String(N); range.step = '1';
      range.value = String(s.lap);
      const row = BB.model.run().laps[s.lap - 1];
      read.innerHTML = 'Lap <b>' + s.lap + '</b> / ' + N + ' · leader <b>' + BB.fmtClock(row.order[0].time) + '</b>';
    }
  }

  function clockAction(act) {
    const s = BB.store.get(), N = BB.model.laps(), run = BB.model.run();

    if (isTimeMode()) {
      const done = BB.model.lapAt(run, s.t);
      const jump = function (t) { BB.store.set({ t: t, lap: lapFor(t), playing: false }); };
      if (act === 'first') { jump(0); }
      else if (act === 'last') { jump(run.duration); }
      else if (act === 'next') { jump(done >= N ? run.duration : BB.model.leaderTime(run, done + 1)); }
      else if (act === 'prev') {
        const here = BB.model.leaderTime(run, done);
        jump(s.t - here > BOUNDARY_EPS ? here : BB.model.leaderTime(run, done - 1));
      } else if (act === 'play') {
        if (s.playing) { BB.store.set({ playing: false }); }
        else if (s.t >= run.duration - BOUNDARY_EPS) { BB.store.set({ t: 0, lap: 1, playing: true }); }
        else { BB.store.set({ playing: true }); }
      }
      return;
    }

    if (act === 'first') { BB.store.set({ lap: 1, playing: false }); }
    else if (act === 'last') { BB.store.set({ lap: N, playing: false }); }
    else if (act === 'prev') { BB.store.set({ lap: Math.max(1, s.lap - 1), playing: false }); }
    else if (act === 'next') { BB.store.set({ lap: Math.min(N, s.lap + 1), playing: false }); }
    else if (act === 'play') {
      if (s.playing) { BB.store.set({ playing: false }); }
      else if (s.lap >= N) { BB.store.set({ lap: 1, playing: true }); }
      else { BB.store.set({ playing: true }); }
    }
  }

  function wireClock() {
    const clock = document.getElementById('clock');
    clock.addEventListener('click', function (ev) {
      const btn = ev.target.closest('[data-act]');
      if (btn) { clockAction(btn.getAttribute('data-act')); return; }
      const seg = ev.target.closest('[data-seg="speed"]');
      if (seg) { BB.store.set({ speed: Number(seg.getAttribute('data-value')) }); }
    });
    document.getElementById('clock-range').addEventListener('input', function (ev) {
      const v = Number(ev.target.value);
      if (isTimeMode()) { BB.store.set({ t: v, lap: lapFor(v), playing: false }); }
      else { BB.store.set({ lap: v, playing: false }); }
    });
    BB.store.subscribe(function (changed) {
      if (changed.indexOf('playing') >= 0 || changed.indexOf('speed') >= 0) { syncTimer(); }
      if (changed.indexOf('playing') >= 0) { renderPlay(); }
      if (changed.indexOf('speed') >= 0) { renderSpeed(); }
      if (changed.some(function (k) { return k === 'lap' || k === 't' || k === 'run'; })) { renderPosition(); }
    });
  }

  // ----------------------------------------------------------------- router
  function route() {
    const id = (location.hash.replace(/^#\//, '') || 'command');
    const page = isReady(id) ? BB.pages[id] : null;
    const target = page ? id : 'command';
    if (!page && location.hash !== '#/command') { location.replace('#/command'); return; }

    if (unmountPage) { unmountPage(); unmountPage = null; }
    stopTimer();
    BB.store.set({ page: target, playing: false });

    const def = BB.pages[target];
    document.title = def.title + ' · Box Box';
    document.getElementById('page-title').textContent = def.title;
    document.getElementById('page-sub').textContent = def.subtitle || '';
    const controls = document.getElementById('page-controls');
    const root = document.getElementById('page');
    controls.innerHTML = '';
    root.innerHTML = '';
    document.getElementById('clock').hidden = !def.clock;
    document.querySelectorAll('.rail-item[data-nav]').forEach(function (el) {
      const on = el.getAttribute('data-nav') === target;
      el.classList.toggle('active', on);
      if (on) { el.setAttribute('aria-current', 'page'); } else { el.removeAttribute('aria-current'); }
    });

    // A continuous-clock page starts from the lap the shared clock is on. At the
    // final lap (the default view) it starts from the grid instead of the flag.
    if (def.clock === 'time') {
      const run = BB.model.run(), s = BB.store.get();
      const t = s.lap >= BB.model.laps() ? 0 : BB.model.leaderTime(run, s.lap);
      BB.store.set({ t: t, lap: lapFor(t) });
    }

    unmountPage = def.mount(root, controls);
    if (def.clock) { renderPlay(); renderSpeed(); renderPosition(); }
  }

  function wireKeys() {
    document.addEventListener('keydown', function (ev) {
      if (ev.ctrlKey || ev.metaKey || ev.altKey) { return; }
      const tag = ev.target && ev.target.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') { return; }
      const nav = NAV.find(function (n) { return n.key === ev.key; });
      if (nav && isReady(nav.id)) { location.hash = nav.hash; return; }
      const def = currentDef();
      if (!def || !def.clock) { return; }
      if (ev.key === 'ArrowLeft') { ev.preventDefault(); clockAction('prev'); }
      else if (ev.key === 'ArrowRight') { ev.preventDefault(); clockAction('next'); }
      else if (ev.key === 'Home') { ev.preventDefault(); clockAction('first'); }
      else if (ev.key === 'End') { ev.preventDefault(); clockAction('last'); }
      else if (ev.key === ' ' && tag !== 'BUTTON' && tag !== 'A') { ev.preventDefault(); clockAction('play'); }
    });
  }

  // ------------------------------------------------------------------- boot
  function boot() {
    const d = window.BOXBOX;
    if (!d) {
      fatal('No data bundle found',
        '<p>Box Box renders results computed by its C++ engine, and <code>data/bundle.js</code> is missing.</p>' +
        '<p>Generate it from the project root with <code>make export</code>, then reload this page.</p>');
      return;
    }
    if (d.meta.version !== 1) {
      fatal('Unsupported bundle version',
        '<p>This frontend reads bundle version 1; the file is version ' + esc(d.meta.version) + '.</p>' +
        '<p>Re-run <code>make export</code> with a matching build.</p>');
      return;
    }
    BB.store.set({ lap: d.meta.laps });
    buildShell();
    wireClock();
    wireKeys();
    window.addEventListener('hashchange', route);
    route();
  }

  document.addEventListener('DOMContentLoaded', boot);
})(window.BB = window.BB || {});
