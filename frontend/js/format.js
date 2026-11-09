/* Formatting only. No race logic lives in the frontend (FRONTEND.md sec. 1). */
(function (BB) {
  'use strict';

  const ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };

  BB.esc = function (value) {
    return String(value).replace(/[&<>"']/g, function (c) { return ESCAPES[c]; });
  };

  BB.fmtSec = function (seconds, digits) {
    return Number.isFinite(seconds) ? seconds.toFixed(digits === undefined ? 3 : digits) : '–';
  };

  /** Race-clock rendering: H:MM:SS.mmm, or M:SS.mmm under an hour. */
  BB.fmtClock = function (seconds) {
    if (!Number.isFinite(seconds) || seconds < 0) { return '–'; }
    let ms = Math.round(seconds * 1000);
    const h = Math.floor(ms / 3600000); ms -= h * 3600000;
    const m = Math.floor(ms / 60000); ms -= m * 60000;
    const s = Math.floor(ms / 1000); ms -= s * 1000;
    const pad = function (n, w) { return String(n).padStart(w, '0'); };
    return (h > 0 ? h + ':' + pad(m, 2) : String(m)) + ':' + pad(s, 2) + '.' + pad(ms, 3);
  };

  BB.fmtGap = function (seconds) {
    return Number.isFinite(seconds) ? '+' + seconds.toFixed(3) : '–';
  };
})(window.BB = window.BB || {});
