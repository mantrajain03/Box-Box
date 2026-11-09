/* Shared markup builders so every page uses the same Panel / StatTile / segmented control. */
(function (BB) {
  'use strict';

  const esc = BB.esc;

  BB.ui = {
    /** o = { id, span, title, algo, algoTip, body } */
    panel: function (o) {
      return '<section class="panel span-' + (o.span || 12) + '"' + (o.id ? ' id="' + o.id + '"' : '') + '>' +
        '<header class="panel-h"><h2>' + esc(o.title) + '</h2>' +
        (o.algo ? '<span class="algo" title="' + esc(o.algoTip || 'C++ source of this panel\'s data') + '">' + esc(o.algo) + '</span>' : '') +
        '</header><div class="panel-b" data-body>' + (o.body || '') + '</div></section>';
    },

    /** Replace just the body of a panel that was built with panel(). */
    setBody: function (root, id, html) {
      const body = root.querySelector('#' + id + ' [data-body]');
      if (body) { body.innerHTML = html; }
    },

    tile: function (label, value, sub, accent) {
      return '<div class="tile" style="--accent:' + accent + '"><div class="tile-l">' + esc(label) + '</div>' +
        '<div class="tile-v" title="' + esc(value) + '">' + esc(value) + '</div>' +
        '<div class="tile-s" title="' + esc(sub) + '">' + esc(sub) + '</div></div>';
    },

    /** Segmented control. options = [{value,label}]. Buttons carry data-seg + data-value. */
    segmented: function (name, options, current, label) {
      return (label ? '<span class="seg-l">' + esc(label) + '</span>' : '') +
        '<div class="seg" role="group" aria-label="' + esc(label || name) + '">' +
        options.map(function (o) {
          return '<button type="button" data-seg="' + name + '" data-value="' + esc(o.value) + '"' +
            (o.value === current ? ' class="on" aria-pressed="true"' : ' aria-pressed="false"') + '>' + esc(o.label) + '</button>';
        }).join('') + '</div>';
    }
  };
})(window.BB = window.BB || {});
