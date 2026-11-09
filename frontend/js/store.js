/* One shared state object with plain subscribe/notify. Holds UI state only. */
(function (BB) {
  'use strict';

  const state = {
    page: 'command',
    lap: 1,                  // the shared race clock: which lap is on screen
    t: 0,                    // race seconds, used by pages with a continuous clock (Race Sim)
    playing: false,
    speed: 1,                // 1, 2 or 4
    run: 'overtakeOff',      // which exported race replay is shown
    car: null                // focused car number, or null
  };
  const listeners = new Set();

  BB.store = {
    get: function () { return state; },

    /** Apply a patch; notify subscribers with the list of keys that really changed. */
    set: function (patch) {
      const changed = [];
      Object.keys(patch).forEach(function (key) {
        if (state[key] !== patch[key]) {
          state[key] = patch[key];
          changed.push(key);
        }
      });
      if (changed.length > 0) {
        Array.from(listeners).forEach(function (fn) { fn(changed); });
      }
    },

    subscribe: function (fn) {
      listeners.add(fn);
      return function () { listeners.delete(fn); };
    }
  };
})(window.BB = window.BB || {});
