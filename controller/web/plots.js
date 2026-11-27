import {language, translate} from '/i18n.js';

const rowCount = 250;
const averageCount = 8;
export const minimumLevel = -1600;
const plotBackground = '#0a1520';
const markerColor = '#a1bdff';
const palettes = {
  blue: ['#0a1520', '#173862', '#2263a7', '#388dd0', '#78ccf8'],
  red: ['#0a1520', '#511c2c', '#a63042', '#eb635c', '#ffd6ad'],
  green: ['#0a1520', '#124033', '#208358', '#43c984', '#c2ffcf'],
};
const levelPowers = Float64Array.from({length: 1601}, (_, i) => 10 ** ((i + minimumLevel) / 100));
const mhzFormat = Object.fromEntries(['de', 'en'].map(lang =>
  [lang, [3, 6].map(digits => new Intl.NumberFormat(lang, {
    minimumFractionDigits: digits, maximumFractionDigits: digits, useGrouping: false,
  }))]));

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function colorTable(palette, lower, upper) {
  const stops = palette.map(color => [1, 3, 5].map(offset => parseInt(color.slice(offset, offset + 2), 16)));
  const table = new Uint8ClampedArray(levelPowers.length * 4);
  for (let i = 0; i < levelPowers.length; ++i) {
    const level = (i + minimumLevel) / 10;
    const pos = 4 * clamp((level - lower) / (upper - lower), 0, 1);
    const index = Math.min(3, Math.floor(pos));
    const fraction = pos - index;
    for (let c = 0; c < 3; ++c) {
      table[i * 4 + c] = stops[index][c] + fraction * (stops[index + 1][c] - stops[index][c]);
    }
    table[i * 4 + 3] = 255;
  }
  return new Uint32Array(table.buffer);
}

export function waterfallColors(color, lower, upper) {
  return colorTable(palettes[color], lower, upper);
}

function box(canvas) {
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  const left = width < 500 ? 39 : 53;
  const right = width - (width < 500 ? 26 : 41);
  const bottom = height - (canvas.hasAttribute('data-spectrum') ? 2 : 22);
  return {left, right, width: Math.max(1, right - left), top: 4, bottom,
    height: Math.max(1, bottom - 4)};
}

function pixelRatio() {
  return Math.min(window.devicePixelRatio || 1, 2);
}

function resize(canvas, dpr) {
  const width = Math.round(canvas.clientWidth * dpr);
  const height = Math.round(canvas.clientHeight * dpr);
  if (!width || !height || (canvas.width === width && canvas.height === height)) {
    return false;
  }
  canvas.width = width;
  canvas.height = height;
  return true;
}

function prepare(canvas, transparent = false) {
  const dpr = pixelRatio();
  resize(canvas, dpr);
  const ctx = canvas.getContext('2d', {alpha: transparent});
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  if (transparent) {
    ctx.clearRect(0, 0, canvas.clientWidth, canvas.clientHeight);
  } else {
    ctx.fillStyle = '#111b24';
    ctx.fillRect(0, 0, canvas.clientWidth, canvas.clientHeight);
  }
  return ctx;
}

export class PlotPair {
  constructor(section, zoom, tune, step) {
    this.spectrum = section.querySelector('[data-spectrum]');
    this.waterfall = section.querySelector('[data-waterfall]');
    this.overlay = section.querySelector('[data-waterfall-overlay]');
    this.zoom = zoom;
    this.tune = tune;
    this.step = step;
    this.wheel = 0;
    this.waterfallCtx = this.waterfall.getContext('2d', {alpha: false});
    this.color = 'blue';
    this.lower = -120;
    this.upper = -25;
    this.table = colorTable(palettes.blue, this.lower, this.upper);
    this.main = 0;
    this.bounds = null;
    this.canvasFrame = 0;
    this.clear();

    this.observer = new ResizeObserver(() => {
      this.waterfallDirty = this.overlayDirty = true;
      this.schedule();
    });
    for (const canvas of [this.spectrum, this.waterfall]) {
      this.observer.observe(canvas);
      canvas.addEventListener('click', event => this.click(event));
      canvas.addEventListener('wheel', event => this.scroll(event), {passive: false});
    }
  }

  clear() {
    this.rows = new Array(rowCount);
    this.head = 0;
    this.count = 0;
    this.bins = 0;
    this.line = null;
    this.lineCenter = 0;
    this.lineDirty = false;
    this.pendingRows = 0;
    this.waterfallDirty = this.overlayDirty = true;
    this.wheel = 0;
    this.schedule();
  }

  setBand(band) {
    this.bounds = band ? {low: band.display_low_hz, high: band.display_high_hz,
      bandLow: band.low_hz, bandHigh: band.high_hz} : null;
    this.clear();
  }

  setMain(hz, halfSpan) {
    if (hz === this.main && (!this.zoom || halfSpan === this.halfSpan)) {
      return;
    }
    this.main = hz;
    if (this.zoom) {
      if (this.halfSpan !== halfSpan || hz === 0) {
        this.clear();
      }
      this.halfSpan = halfSpan;
      this.bounds = hz > 0 && halfSpan > 0 ? {low: hz - halfSpan, high: hz + halfSpan} : null;
      this.lineDirty = true;
      this.waterfallDirty = true;
    }
    this.overlayDirty = true;
    this.schedule();
  }

  setPassband(low, high) {
    if (this.passbandLow === low && this.passbandHigh === high) {
      return;
    }
    this.passbandLow = low;
    this.passbandHigh = high;
    this.overlayDirty = true;
    this.schedule();
  }

  languageChanged() {
    this.overlayDirty = true;
    this.schedule();
  }

  passband(ctx, area) {
    if (!this.zoom || !this.bounds || !this.passbandHigh) {
      return;
    }
    const left = this.xAt(this.main + this.passbandLow, area);
    const right = this.xAt(this.main + this.passbandHigh, area);
    ctx.fillStyle = '#55d9ee15';
    ctx.fillRect(left, area.top, right - left, area.height);
    ctx.strokeStyle = '#55d9ee60';
    ctx.lineWidth = 1;
    ctx.strokeRect(left, area.top, right - left, area.height);
  }

  setStyle(lower, upper, color) {
    if (lower === this.lower && upper === this.upper && color === this.color) {
      return;
    }
    this.lower = lower;
    this.upper = upper;
    this.color = color;
    this.table = colorTable(palettes[color], lower, upper);
    this.waterfallDirty = true;
    this.schedule();
  }

  addFrame(levels, center) {
    if (this.bins !== levels.length) {
      this.clear();
      this.bins = levels.length;
      this.powers = new Float64Array(this.bins);
    }
    this.head = (this.head + rowCount - 1) % rowCount;
    this.rows[this.head] = {center, levels};
    this.count = Math.min(rowCount, this.count + 1);
    ++this.pendingRows;
    this.lineDirty = true;
    this.schedule();
  }

  schedule() {
    if (this.canvasFrame === 0) {
      this.canvasFrame = requestAnimationFrame(() => this.draw());
    }
  }

  updateLine() {
    if (!this.lineDirty) {
      return;
    }
    this.lineDirty = false;
    const first = this.rows[this.head];
    if (!first || (this.zoom && first.center !== this.main)) {
      return;
    }
    this.powers.fill(0);
    let used = 0;
    for (; used < Math.min(this.count, averageCount); ++used) {
      const row = this.rows[(this.head + used) % rowCount];
      if (row.center !== first.center) {
        break;
      }
      for (let bin = 0; bin < this.bins; ++bin) {
        this.powers[bin] += levelPowers[row.levels[bin] - minimumLevel];
      }
    }
    if (this.line?.length !== this.bins) {
      this.line = new Float32Array(this.bins);
    }
    for (let bin = 0; bin < this.bins; ++bin) {
      this.line[bin] = 10 * Math.log10(this.powers[bin] / used);
    }
    this.lineCenter = first.center;
  }

  draw() {
    this.canvasFrame = 0;
    this.updateLine();
    this.drawSpectrum();
    this.drawWaterfall();
  }

  xAt(hz, area) {
    return area.left + (hz - this.bounds.low) * area.width / (this.bounds.high - this.bounds.low);
  }

  shift(center, area) {
    return this.zoom ? this.xAt(center, area) - (area.left + area.width / 2) : 0;
  }

  frequencyTicks(area) {
    let ticks = clamp(Math.floor(area.width / 126), 2, 8);
    if (this.zoom) {
      ticks -= ticks % 2;
    }
    return ticks;
  }

  drawFrequencyAxis(ctx, area) {
    if (!this.bounds) {
      return;
    }
    const ticks = this.frequencyTicks(area);
    const format = mhzFormat[language][Number(this.zoom)];
    ctx.font = '11px system-ui';
    ctx.textBaseline = 'middle';
    ctx.fillStyle = '#a1b6c9';
    ctx.textAlign = 'center';
    for (let i = 0; i <= ticks; ++i) {
      const fraction = i / ticks;
      const hz = this.bounds.low + fraction * (this.bounds.high - this.bounds.low);
      ctx.fillText(format.format(hz / 1e6), area.left + fraction * area.width, area.bottom + 12);
    }
  }

  reserve(ctx, area) {
    if (!this.bounds || this.zoom) {
      return;
    }
    const left = this.xAt(this.bounds.bandLow, area);
    const right = this.xAt(this.bounds.bandHigh, area);
    ctx.fillStyle = '#6f98be20';
    ctx.fillRect(area.left, area.top, left - area.left, area.height);
    ctx.fillRect(right, area.top, area.right - right, area.height);
  }

  marker(ctx, area) {
    if (!this.bounds || this.main <= 0 || this.main < this.bounds.low || this.main > this.bounds.high) {
      return;
    }
    const x = this.xAt(this.main, area);
    ctx.save();
    ctx.setLineDash([5, 4]);
    ctx.beginPath();
    ctx.moveTo(x, area.top);
    ctx.lineTo(x, area.bottom);
    ctx.lineWidth = 3;
    ctx.strokeStyle = '#09131e';
    ctx.stroke();
    ctx.lineWidth = 1.3;
    ctx.strokeStyle = markerColor;
    ctx.stroke();
    ctx.restore();
  }

  frame(ctx, area) {
    ctx.strokeStyle = '#2b3f53';
    ctx.lineWidth = 1;
    ctx.strokeRect(area.left, area.top, area.width, area.height);
  }

  levelY(db, area) {
    return area.bottom - area.height * clamp((db - this.lower) / (this.upper - this.lower), 0, 1);
  }

  grid(ctx, area) {
    const minInterval = (this.upper - this.lower) / Math.max(2, Math.floor(area.height / 26));
    const step = [1, 2, 5, 10, 20, 25, 50, 100].find(value => value >= minInterval);
    ctx.font = '11px system-ui';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    ctx.fillStyle = '#a1b6c9';
    ctx.strokeStyle = '#26394b';
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let db = Math.ceil(this.lower / step) * step; db < this.upper; db += step) {
      const y = this.levelY(db, area);
      if (y > area.top + 13 && y < area.bottom - 13) {
        ctx.moveTo(area.left, y);
        ctx.lineTo(area.right, y);
        ctx.fillText(db, area.left - 6, y);
      }
    }
    ctx.fillText(this.upper, area.left - 6, area.top + 2);
    ctx.fillText(this.lower, area.left - 6, area.bottom - 2);
    const ticks = this.frequencyTicks(area);
    for (let i = 1; i < ticks; ++i) {
      const x = area.left + area.width * i / ticks;
      ctx.moveTo(x, area.top);
      ctx.lineTo(x, area.bottom);
    }
    ctx.stroke();
  }

  drawSpectrum() {
    const ctx = prepare(this.spectrum);
    const area = box(this.spectrum);
    ctx.fillStyle = plotBackground;
    ctx.fillRect(area.left, area.top, area.width, area.height);
    this.reserve(ctx, area);
    this.passband(ctx, area);
    this.grid(ctx, area);

    if (this.line && this.bounds) {
      const left = area.left + this.shift(this.lineCenter, area);
      const points = Math.min(this.line.length, Math.max(1, Math.round(area.width)));
      const color = palettes[this.color][3];
      ctx.save();
      ctx.beginPath();
      ctx.rect(area.left, area.top, area.width, area.height);
      ctx.clip();
      ctx.beginPath();
      for (let point = 0; point < points; ++point) {
        const begin = Math.floor(point * this.line.length / points);
        const end = Math.floor((point + 1) * this.line.length / points);
        let strongest = this.line[begin];
        for (let bin = begin + 1; bin < end; ++bin) {
          strongest = Math.max(strongest, this.line[bin]);
        }
        const x = left + (point + .5) * area.width / points;
        const y = this.levelY(strongest, area);
        if (point === 0) {
          ctx.moveTo(x, y);
        } else {
          ctx.lineTo(x, y);
        }
      }
      ctx.lineWidth = 1;
      ctx.strokeStyle = color;
      ctx.stroke();
      ctx.lineTo(left + area.width, area.bottom);
      ctx.lineTo(left, area.bottom);
      ctx.closePath();
      const gradient = ctx.createLinearGradient(0, area.top, 0, area.bottom);
      gradient.addColorStop(0, color + '48');
      gradient.addColorStop(1, color + '08');
      ctx.fillStyle = gradient;
      ctx.fill();
      ctx.restore();
    } else {
      ctx.fillStyle = '#a1b6c9';
      ctx.font = '12px system-ui';
      ctx.textAlign = 'center';
      ctx.fillText(translate(this.zoom ? 'Warte auf I/Q-Daten um MAIN' : 'Warte auf Spektrum'),
        area.left + area.width / 2, area.top + area.height / 2);
    }
    this.marker(ctx, area);
    this.frame(ctx, area);
  }

  drawWaterfall() {
    const canvas = this.waterfall;
    const ctx = this.waterfallCtx;
    const dpr = pixelRatio();
    const area = box(this.waterfall);
    const left = Math.round(area.left * dpr);
    const top = Math.round(area.top * dpr);
    const width = Math.max(1, Math.round(area.right * dpr) - left);
    const height = Math.max(1, Math.round(area.bottom * dpr) - top);
    const rowHeight = Math.max(1, Math.round(dpr));
    if (resize(canvas, dpr)) {
      this.waterfallDirty = this.overlayDirty = true;
    }
    if (this.waterfallDirty) {
      ctx.fillStyle = '#111b24';
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      ctx.fillStyle = plotBackground;
      ctx.fillRect(left, top, width, height);
      this.paintWaterfallRows(left, top, width, Math.min(height, this.count * rowHeight), rowHeight);
      this.waterfallDirty = false;
    } else if (this.pendingRows) {
      const addedHeight = Math.min(height, this.count * rowHeight, this.pendingRows * rowHeight);
      const retainedHeight = height - addedHeight;
      // Copy existing pixels at whole backing-pixel coordinates, then paint the new rows once.
      if (retainedHeight) {
        ctx.imageSmoothingEnabled = false;
        ctx.drawImage(canvas, left, top, width, retainedHeight,
          left, top + addedHeight, width, retainedHeight);
      }
      this.paintWaterfallRows(left, top, width, addedHeight, rowHeight);
    }
    this.pendingRows = 0;
    if (this.overlayDirty) {
      const overlay = prepare(this.overlay, true);
      this.reserve(overlay, area);
      this.passband(overlay, area);
      this.marker(overlay, area);
      this.frame(overlay, area);
      this.drawFrequencyAxis(overlay, area);
      this.overlayDirty = false;
    }
  }

  paintWaterfallRows(left, top, width, height, rowHeight) {
    if (!height || !this.bounds) {
      return;
    }
    if (this.rowPixels?.width !== width || this.rowPixels.height !== height) {
      this.rowPixels = this.waterfallCtx.createImageData(width, height);
    }
    const pixels = new Uint32Array(this.rowPixels.data.buffer);
    pixels.fill(this.table[0]);
    for (let y = 0; y < height; y += rowHeight) {
      const row = this.rows[(this.head + y / rowHeight) % rowCount];
      const shift = this.zoom ? Math.round((row.center - this.main) * width / (2 * this.halfSpan)) : 0;
      const right = Math.min(width, width + shift);
      for (let x = Math.max(0, shift); x < right; ++x) {
        const start = Math.floor((x - shift) * this.bins / width);
        const end = Math.max(start + 1, Math.floor((x - shift + 1) * this.bins / width));
        let peak = row.levels[start];
        for (let bin = start + 1; bin < end; ++bin) {
          peak = Math.max(peak, row.levels[bin]);
        }
        pixels[y * width + x] = this.table[peak - minimumLevel];
      }
      for (let copy = 1; copy < rowHeight && y + copy < height; ++copy) {
        pixels.copyWithin((y + copy) * width, y * width, (y + 1) * width);
      }
    }
    this.waterfallCtx.putImageData(this.rowPixels, left, top);
  }

  plotPosition(event) {
    if (!this.bounds || this.count === 0) {
      return null;
    }
    const canvas = event.currentTarget;
    const rect = canvas.getBoundingClientRect();
    const area = box(canvas);
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    if (x < area.left || x > area.right || y < area.top || y > area.bottom) {
      return null;
    }
    return (x - area.left) / area.width;
  }

  click(event) {
    const position = this.plotPosition(event);
    if (position !== null && event.button === 0) {
      this.tune(Math.round(this.bounds.low + position * (this.bounds.high - this.bounds.low)));
    }
  }

  scroll(event) {
    if (this.plotPosition(event) === null || !event.deltaY) {
      return;
    }
    event.preventDefault();
    const units = event.deltaMode === 1 ? 3 : (event.deltaMode === 2 ? 1 : 100);
    const legacy = event.wheelDeltaY;
    const delta = legacy ? legacy / 120 : -event.deltaY / units;
    this.wheel += delta;
    const steps = Math.trunc(this.wheel);
    this.wheel -= steps;
    if (steps) {
      this.step(steps);
    }
  }
}
