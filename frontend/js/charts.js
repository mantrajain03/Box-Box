/* Hand-rolled SVG/HTML charts. They map exported numbers to pixels and nothing else.
   Hover text uses native <title> tooltips, so it works offline with no library. */
(function (BB) {
  'use strict';

  const esc = BB.esc;

  /** Axis tick positions for a data range (presentation only). */
  function niceTicks(min, max, count) {
    if (!(max > min)) { max = min + 1; }
    const raw = (max - min) / count;
    const mag = Math.pow(10, Math.floor(Math.log10(raw)));
    const norm = raw / mag;
    const step = (norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 5 ? 5 : 10) * mag;
    const lo = Math.floor(min / step) * step;
    const hi = Math.ceil(max / step) * step;
    const ticks = [];
    for (let v = lo; v <= hi + step / 2; v += step) { ticks.push(Number(v.toFixed(10))); }
    return { ticks: ticks, lo: lo, hi: hi };
  }

  const charts = {};

  /**
   * Line chart. o = { series:[{name,color,points:[[x,y]|null]}], width, height,
   *   yFmt(v), xLabel, marker (x value), xDomain:[min,max] }
   */
  charts.lines = function (o) {
    const W = o.width || 640, H = o.height || 250;
    const m = { l: 58, r: 14, t: 12, b: 28 };
    let xMin = Infinity, xMax = -Infinity, yMin = Infinity, yMax = -Infinity;
    o.series.forEach(function (s) {
      s.points.forEach(function (p) {
        if (!p || !Number.isFinite(p[1])) { return; }
        xMin = Math.min(xMin, p[0]); xMax = Math.max(xMax, p[0]);
        yMin = Math.min(yMin, p[1]); yMax = Math.max(yMax, p[1]);
      });
    });
    if (!Number.isFinite(xMin)) { return '<div class="empty">No data</div>'; }
    if (o.xDomain) { xMin = o.xDomain[0]; xMax = o.xDomain[1]; }
    if (xMax <= xMin) { xMax = xMin + 1; }
    const yt = niceTicks(yMin, yMax, 4);
    const xt = niceTicks(xMin, xMax, 6);
    const yFmt = o.yFmt || function (v) { return String(v); };

    const X = function (v) { return m.l + (v - xMin) / (xMax - xMin) * (W - m.l - m.r); };
    const Y = function (v) { return H - m.b - (v - yt.lo) / (yt.hi - yt.lo) * (H - m.t - m.b); };

    let svg = '<svg class="chart" viewBox="0 0 ' + W + ' ' + H + '" role="img" aria-label="' + esc(o.label || 'line chart') + '">';
    yt.ticks.forEach(function (v) {
      svg += '<line class="grid-l" x1="' + m.l + '" x2="' + (W - m.r) + '" y1="' + Y(v) + '" y2="' + Y(v) + '"/>' +
             '<text x="' + (m.l - 8) + '" y="' + (Y(v) + 3.5) + '" text-anchor="end">' + esc(yFmt(v)) + '</text>';
    });
    xt.ticks.forEach(function (v) {
      if (v < xMin || v > xMax) { return; }
      svg += '<text x="' + X(v) + '" y="' + (H - 8) + '" text-anchor="middle">' + v + '</text>';
    });
    svg += '<line class="axis-l" x1="' + m.l + '" x2="' + (W - m.r) + '" y1="' + (H - m.b) + '" y2="' + (H - m.b) + '"/>';

    o.series.forEach(function (s) {
      let d = '', pen = false;
      s.points.forEach(function (p) {
        if (!p || !Number.isFinite(p[1])) { pen = false; return; }
        d += (pen ? 'L' : 'M') + X(p[0]).toFixed(1) + ' ' + Y(p[1]).toFixed(1);
        pen = true;
      });
      svg += '<path d="' + d + '" fill="none" stroke="' + s.color + '" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>';
    });

    if (o.marker !== undefined && o.marker >= xMin && o.marker <= xMax) {
      svg += '<line class="marker" x1="' + X(o.marker) + '" x2="' + X(o.marker) + '" y1="' + m.t + '" y2="' + (H - m.b) + '"/>';
    }

    // Hover targets, one per point.
    o.series.forEach(function (s) {
      s.points.forEach(function (p) {
        if (!p || !Number.isFinite(p[1])) { return; }
        svg += '<circle class="hit" cx="' + X(p[0]).toFixed(1) + '" cy="' + Y(p[1]).toFixed(1) + '" r="5">' +
               '<title>' + esc(s.name) + ' · ' + esc(o.xLabel || 'x') + ' ' + p[0] + ': ' + esc(yFmt(p[1])) + '</title></circle>';
      });
    });
    return svg + '</svg>';
  };

  /** Vertical bars. o = { items:[{label,value,color,title,active}], height, integer } */
  charts.bars = function (o) {
    const W = o.width || 420, H = o.height || 220;
    const m = { l: 30, r: 8, t: 10, b: 28 };
    let max = 0;
    o.items.forEach(function (it) { max = Math.max(max, it.value); });
    const yt = niceTicks(0, Math.max(max, o.integer ? 2 : 1), 4);
    const Y = function (v) { return H - m.b - v / yt.hi * (H - m.t - m.b); };
    const slot = (W - m.l - m.r) / o.items.length;

    let svg = '<svg class="chart" viewBox="0 0 ' + W + ' ' + H + '" role="img" aria-label="' + esc(o.label || 'bar chart') + '">';
    yt.ticks.forEach(function (v) {
      if (o.integer && v !== Math.floor(v)) { return; }
      svg += '<line class="grid-l" x1="' + m.l + '" x2="' + (W - m.r) + '" y1="' + Y(v) + '" y2="' + Y(v) + '"/>' +
             '<text x="' + (m.l - 6) + '" y="' + (Y(v) + 3.5) + '" text-anchor="end">' + v + '</text>';
    });
    o.items.forEach(function (it, i) {
      const x = m.l + i * slot + slot * 0.15, w = slot * 0.7;
      // `nub` keeps a zero-height bar visible (the best choice has a delta of exactly 0).
      const y = Y(it.value), h = Math.max(H - m.b - y, it.value > 0 ? 1 : (it.nub ? 3 : 0));
      svg += '<rect class="bar" x="' + x.toFixed(1) + '" y="' + (it.nub && it.value <= 0 ? (H - m.b - h) : y).toFixed(1) + '" width="' + w.toFixed(1) +
             '" height="' + h.toFixed(1) + '" rx="2" fill="' + it.color + '" opacity="' + (it.active ? 1 : 0.55) + '">' +
             '<title>' + esc(it.title || (it.label + ': ' + it.value)) + '</title></rect>' +
             '<text x="' + (x + w / 2).toFixed(1) + '" y="' + (H - 10) + '" text-anchor="middle"' +
             (it.active ? ' style="fill:var(--text)"' : '') + '>' + esc(it.label) + '</text>';
    });
    svg += '<line class="axis-l" x1="' + m.l + '" x2="' + (W - m.r) + '" y1="' + (H - m.b) + '" y2="' + (H - m.b) + '"/>';
    return svg + '</svg>';
  };

  /** Donut. o = { slices:[{label,value,color}], center, sub, size } */
  charts.donut = function (o) {
    const S = o.size || 190, r = 70, cx = S / 2, cy = S / 2, C = 2 * Math.PI * r;
    let total = 0;
    o.slices.forEach(function (s) { total += s.value; });
    let svg = '<svg class="chart" style="max-width:' + S + 'px" viewBox="0 0 ' + S + ' ' + S + '" role="img" aria-label="' + esc(o.label || 'donut chart') + '">' +
              '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="none" stroke="var(--border)" stroke-width="22"/>';
    let offset = 0;
    if (total > 0) {
      o.slices.forEach(function (s) {
        if (s.value <= 0) { return; }
        const len = s.value / total * C;
        svg += '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="none" stroke="' + s.color + '" stroke-width="22"' +
               ' stroke-dasharray="' + len.toFixed(2) + ' ' + (C - len).toFixed(2) + '" stroke-dashoffset="' + (-offset).toFixed(2) +
               '" transform="rotate(-90 ' + cx + ' ' + cy + ')"><title>' + esc(s.label) + ': ' + s.value +
               ' (' + (s.value / total * 100).toFixed(1) + '%)</title></circle>';
        offset += len;
      });
    }
    svg += '<text class="donut-c" x="' + cx + '" y="' + (cy + 4) + '" text-anchor="middle">' + esc(o.center) + '</text>' +
           '<text class="donut-s" x="' + cx + '" y="' + (cy + 20) + '" text-anchor="middle">' + esc(o.sub || '') + '</text></svg>';
    return '<div class="donut-wrap">' + svg + '</div>';
  };

  /**
   * Stint timeline as HTML. o = { stints:[{laps,color,label,title}], total, marker }
   * Segment widths are laps / total, so the bar is to scale.
   */
  charts.stintBar = function (o) {
    let html = '<div class="stints">';
    o.stints.forEach(function (s) {
      html += '<div class="stint" style="flex:' + s.laps + ' 1 0;background:' + s.color + '" title="' + esc(s.title || s.label) + '">' +
              esc(s.label) + '</div>';
    });
    if (o.marker !== undefined) {
      html += '<div class="mark" style="left:' + (o.marker / o.total * 100).toFixed(2) + '%"></div>';
    }
    html += '</div><div class="stint-axis">';
    let lap = 0;
    o.stints.forEach(function (s) {
      html += '<span style="left:' + (lap / o.total * 100).toFixed(2) + '%">' + (lap + 1) + '</span>';
      lap += s.laps;
    });
    html += '<span style="left:100%;transform:translateX(-100%)">' + o.total + '</span></div>';
    return html;
  };

  BB.charts = charts;
})(window.BB = window.BB || {});
