/* Read-only lookups into the exported bundle (window.BOXBOX). Selection and colour
   mapping only; every number shown comes from the C++ engine. */
(function (BB) {
  'use strict';

  // Cars get colours that never collide with the compound colours (red/yellow/white).
  const CAR_COLORS = ['#a855f7', '#3b82f6', '#22c55e', '#ec4899', '#22d3ee',
                      '#fb923c', '#a3e635', '#2dd4bf', '#e879f9', '#60a5fa'];

  const COMPOUND_BY_NAME = { SOFT: '#ff2d3d', MEDIUM: '#facc15', HARD: '#e8e8ee' };
  const COMPOUND_FALLBACK = ['#3b82f6', '#a855f7', '#22c55e', '#ec4899', '#22d3ee'];

  function data() { return window.BOXBOX; }

  BB.model = {
    data: data,

    laps: function () { return data().meta.laps; },

    /** The exported race replay for `key`, or the one currently selected. */
    run: function (key) { return data().race.runs[key || BB.store.get().run]; },

    compoundName: function (index) {
      const c = data().compounds[index];
      return c ? c.name : '?';
    },

    compoundColor: function (index) {
      const c = data().compounds[index];
      if (c && COMPOUND_BY_NAME[c.name.toUpperCase()]) { return COMPOUND_BY_NAME[c.name.toUpperCase()]; }
      return COMPOUND_FALLBACK[index % COMPOUND_FALLBACK.length];
    },

    carColor: function (car) { return CAR_COLORS[(car - 1) % CAR_COLORS.length]; },

    driver: function (run, car) {
      return run.drivers.find(function (d) { return d.car === car; });
    },

    carName: function (run, car) {
      const d = BB.model.driver(run, car);
      return d ? d.name : '#' + car;
    },

    /** Laps the leader has completed by race time t (0..N): a lookup over exported times. */
    lapAt: function (run, t) {
      let done = 0;
      for (let i = 0; i < run.laps.length; i++) {
        if (run.laps[i].order[0].time <= t) { done = run.laps[i].lap; } else { break; }
      }
      return done;
    },

    /** Race time at which the leader completes `lap`; 0 for lap 0. */
    leaderTime: function (run, lap) {
      return lap <= 0 ? 0 : run.laps[Math.min(lap, run.laps.length) - 1].order[0].time;
    },

    /** One car's row in lap `lap` (1-based), or undefined. */
    entry: function (run, lap, car) {
      const row = run.laps[lap - 1];
      return row ? row.order.find(function (e) { return e.car === car; }) : undefined;
    }
  };
})(window.BB = window.BB || {});
