import {countryFlagPosition, dxccForCall} from '/dxcc.js';
import {waterfallColors, minimumLevel} from '/plots.js';

const $ = id => document.getElementById(id);
const emptyState = {mode: 'off', state: 'receiving', auto: 'off'};
const stateLabels = {
  'calling-cq': 'CQ rufen',
  report: 'Warte auf Rapport',
  'roger-report': 'Warte auf bestätigten Rapport',
  acknowledgment: 'Warte auf Bestätigung',
  closing: 'QSO abschließen',
  'free-text': 'Freitext senden',
};

export function createFtPanel({send, preferences, savePreferences}) {
  let radio = {connected: false, dial: 0};
  let state = emptyState;
  let active = false;
  let mode = preferences.ftMode === 'FT4' ? 'FT4' : 'FT8';
  let error = '', outcome = '';
  let revision = 0;
  let settingsDirty = false;
  let clock;
  const requests = new Map();
  const ft8Log = {output:$('ft8-log'), lines:[], follow:true};
  const ftOwnLog = {output:$('ft8-own-log'), lines:[], follow:true};
  const view = $('receiver-view');
  const zoomChoice = view.querySelector('input[value="zoom"]');
  const ftChoice = view.querySelector('input[value="ft"]');
  const panel = $('ft8-panel');
  const canvas = $('ft-waterfall');
  const context = canvas.getContext('2d');
  const row = context.createImageData(canvas.width, 1);
  const pixels = new Uint32Array(row.data.buffer);
  let palette = waterfallColors('blue', -120, -25);
  let waterfallAt = 0;
  const modeSelect = $('ft-mode');
  const callInput = $('ft-own-call'), gridInput = $('ft-own-grid');
  const offsetInput = $('ft8-audio-hz'), levelInput = $('ft8-level');
  const paritySelect = $('ft8-parity');
  const settings = $('ft-settings');
  const arm = $('ft-arm'), auto = $('ft-auto');

  function request(op, values = {}) {
    error = '';
    outcome = '';
    const id = ++revision;
    if (send({op, ...values, request_id: id})) {
      const timeout = setTimeout(() => {
        requests.delete(id);
        error = 'Keine Antwort vom Backend.';
        renderView();
      }, 6000);
      requests.set(id, {op, timeout});
    } else {
      error = 'Backend nicht verbunden.';
    }
    renderView();
  }
  function pending(op) {
    return Array.from(requests.values()).some(request => request.op === op);
  }
  function clearFt8() {
    clearFtLog(ft8Log);
    clearFtLog(ftOwnLog);
  }
  function appendFtLine(log, line) {
    log.lines.push(line);
    log.output.append(line);
    if (log.lines.length > 50) log.lines.shift().remove();
    if (log.follow) scheduleFtLogScroll(log);
  }
  function utcLabel(ms) {
    const time = new Date(ms);
    return time.toISOString().slice(11, 19) + (ms % 1000 ? '.5' : '');
  }
  function canStart() {
    return active && state.armed && state.tx_ready && !state.busy && !requests.size;
  }
  function appendDecode(message) {
    const {source, destination, value, text, slot_utc_ms: slot, snr_db, dt_s, audio_hz: hz} = message;
    const own = state.callsign;
    const clickable = !!source && source !== own;
    const line = document.createElement(clickable ? 'button' : 'div');
    line.className = 'ft-decode-line';
    line.toggleAttribute('data-ft-cq', message.cq);
    const metadata = `${utcLabel(slot)} ${String(snr_db).padStart(3)} dB ${dt_s.toFixed(2).padStart(5)} s ${hz.toFixed(0).padStart(4)} Hz  `;
    // Keep the exact on-air text, including hash brackets and directed CQ qualifiers.
    const words = text.split(' ');
    const sourceIndex = words.findIndex(word => word.replace(/^<|>$/g, '') === source);
    const fields = source && destination && sourceIndex > 0
      ? {destination: words.slice(0, sourceIndex).join(' '), source: words[sourceIndex], exchange: words.slice(sourceIndex + 1).join(' ') || value}
      : null;
    ftDecodeFields(line, metadata, text, fields);
    if (clickable) {
      line.type = 'button';
      line.disabled = !canStart();
      line.onclick = () => {
        if (canStart()) request('ft_start_qso', {peer: source, decode_id: message.decode_id});
      };
    }
    appendFtLine(ft8Log, line);
    if (own && (source === own || destination === own)) {
      const copy = line.cloneNode(true);
      copy.onclick = line.onclick;
      appendFtLine(ftOwnLog, copy);
    }
  }
  function appendTx(message) {
    const line = document.createElement('div');
    line.className = 'ft-decode-line ft-sent-line';
    line.textContent = `${utcLabel(message.slot_utc_ms)} TX  ${String(message.audio_hz).padStart(4)} Hz  ${message.text}`;
    appendFtLine(ftOwnLog, line);
  }
  for (const log of [ft8Log, ftOwnLog]) {
    const {output} = log;
    const resize = new ResizeObserver(() => scheduleFtLogScroll(log));
    resize.observe(output, {box: 'border-box'});
    output.addEventListener('scroll', () => {
      if (!log.height || log.height !== output.clientHeight || log.width !== output.clientWidth ||
          log.scrollTop === output.scrollTop) return;
      log.scrollTop = output.scrollTop;
      log.follow = output.scrollHeight - output.clientHeight - log.scrollTop < 2;
      output.toggleAttribute('data-ft-reading-history', !log.follow);
    }, {passive: true});
    document.querySelector(`.ft-log-end[aria-controls="${output.id}"]`).onclick = () => scrollFtLogToEnd(log);
  }
  const cqOnly = $('ft-cq-only');
  function applyCqOnly() {
    ft8Log.output.toggleAttribute('data-cq-only', cqOnly.checked);
    ft8Log.output.setAttribute('aria-label', cqOnly.checked ? 'FT4/8-CQ-Rufe' : 'Alle FT4/8-Nachrichten');
    scheduleFtLogScroll(ft8Log);
  }
  cqOnly.checked = preferences.ftCqOnly === true;
  cqOnly.onchange = () => {
    preferences.ftCqOnly = cqOnly.checked;
    savePreferences();
    applyCqOnly();
  };
  applyCqOnly();

  function scheduleFtLogScroll(log) {
    if (log.scrollFrame) return;
    log.scrollFrame = requestAnimationFrame(() => {
      log.scrollFrame = 0;
      const {output} = log;
      const width = output.clientWidth, height = output.clientHeight;
      const resized = width !== log.width || height !== log.height;
      log.width = width;
      log.height = height;
      if (height && log.follow) {
        const end = Math.max(0, output.scrollHeight - height);
        if (output.scrollTop !== end) output.scrollTop = end;
        log.scrollTop = output.scrollTop;
      } else if (resized) log.scrollTop = output.scrollTop;
    });
  }
  function scrollFtLogToEnd(log) {
    log.follow = true;
    log.output.removeAttribute('data-ft-reading-history');
    scheduleFtLogScroll(log);
  }
  function clearFtLog(log) {
    log.lines.length = 0;
    log.output.textContent = '';
    scrollFtLogToEnd(log);
  }
  const ftRegionNames = new Intl.DisplayNames('de', {type: 'region', fallback: 'none'});
  function updateFtCountry(node) {
    const {region, entity} = node.dataset;
    const name = region === 'GB' ? 'UK' : (region && ftRegionNames.of(region)) || entity;
    node.querySelector('.ft-country-name').textContent = name;
  }
  function ftDecodeFields(line, metadata, text, fields) {
    if (!fields) {
      line.textContent = metadata + text;
      return;
    }
    line.classList.add('ft-decode-fields');
    line.dataset.message = text;
    const info = document.createElement('span');
    info.className = 'ft-decode-info';
    info.textContent = metadata;
    const body = document.createElement('span');
    body.className = 'ft-message-body';
    for (const [value, trailing] of [[fields.destination, ' '], [fields.source, fields.exchange ? ' ' : ''], [fields.exchange, '']]) {
      const column = document.createElement('span');
      column.textContent = value + trailing;
      body.append(column);
    }
    line.append(info, body);
    const location = dxccForCall(fields.source);
    if (!location) return;
    const country = document.createElement('span');
    country.className = 'ft-country';
    country.dataset.region = location.code;
    country.dataset.entity = location.entity;
    country.title = location.entity;
    const position = countryFlagPosition(location.code);
    if (position) {
      const flag = document.createElement('span');
      flag.className = 'ft-country-flag';
      flag.setAttribute('aria-hidden', 'true');
      flag.style.backgroundPosition = position;
      country.append(flag);
    }
    const name = document.createElement('span');
    name.className = 'ft-country-name';
    country.append(name);
    updateFtCountry(country);
    line.append(country);
  }

  function renderClock() {
    const progress = $('ft-slot-progress');
    const label = $('ft-utc-clock');
    if (!active || !clock || !state.slot_ms) {
      label.textContent = '--:--:-- UTC';
      progress.value = 0;
      return;
    }
    const now = Math.floor(clock.utc + performance.now() - clock.at);
    label.textContent = `${new Date(now).toISOString().slice(11, 19)} UTC`;
    progress.max = state.slot_ms;
    progress.value = now % state.slot_ms;
  }

  function renderView() {
    const changingMode = pending('ft_mode');
    zoomChoice.checked = !active;
    ftChoice.checked = active;
    ftChoice.disabled = !state.available || !radio.connected || changingMode || (!active && (radio.paused || radio.voiceBusy));
    zoomChoice.disabled = changingMode;
    panel.hidden = !active;
    $('zoom').hidden = active;
    $('ft-clear').hidden = !active;
    $('ft8-frequency').textContent = radio.dial
      ? `MAIN ${(radio.dial / 1e6).toLocaleString('de-DE', {minimumFractionDigits:6})} MHz · USB` : 'MAIN';
    modeSelect.value = mode;
    modeSelect.disabled = changingMode || radio.paused || radio.voiceBusy;
    arm.checked = !!state.armed;
    arm.disabled = requests.size > 0 || (!state.armed && (!state.tx_ready || settingsDirty));
    auto.value = state.auto;
    auto.disabled = !state.armed || requests.size > 0;
    const readyToStart = canStart();
    $('ft-cq').disabled = !readyToStart;
    $('ft8-send').disabled = !readyToStart;
    $('ft8-stop').disabled = !state.busy && state.auto === 'off';
    const canEdit = !state.armed && !state.busy && !requests.size;
    for (const control of [callInput, gridInput, offsetInput, levelInput, $('ft-save')]) {
      control.disabled = !canEdit;
    }
    for (const log of [ft8Log, ftOwnLog]) {
      for (const line of log.lines) {
        if (line.tagName === 'BUTTON') line.disabled = !readyToStart;
      }
    }
    const qso = [state.peer, stateLabels[state.state],
      state.retries ? `Wiederholungen ${state.retries}` : ''].filter(Boolean).join(' · ');
    $('ft-contact').hidden = !qso;
    $('ft-qso-status').textContent = qso;
    const status = $('ft8-status');
    const needsStation = active && (!state.callsign || !state.locator);
    status.textContent = error || (needsStation ? 'Rufzeichen und QTH-Locator eingeben und speichern.' :
      state.sending ? 'Sendet …' : outcome);
    status.classList.toggle('error', !!error);
    status.classList.toggle('sending', state.sending && !error);
  }

  function fillSettings() {
    const saved = !!state.callsign;
    callInput.value = state.callsign || preferences.ftOwnCall || '';
    gridInput.value = state.locator || preferences.ftOwnGrid || '';
    offsetInput.value = saved ? state.tx_hz : Math.max(200, Math.min(Number(offsetInput.max), Number(preferences.ftAudioHz) || 1500));
    levelInput.value = saved ? state.tx_level : preferences.ftTxLevel ?? 25;
  }

  function update(next) {
    if (!next.connected && radio.connected) {
      for (const request of requests.values()) clearTimeout(request.timeout);
      requests.clear();
      state = emptyState;
      active = false;
      clock = null;
      settingsDirty = false;
      error = outcome = '';
      clearFt8();
      clearWaterfall();
    }
    if (radio.dial !== next.dial || radio.band?.id !== next.band?.id) {
      clearFt8();
      clearWaterfall();
    }
    radio = next;
    renderView();
  }

  function handle(message) {
    switch (message.type) {
      case 'ft-reply': {
        const entry = requests.get(message.request_id);
        if (!entry) return;
        clearTimeout(entry.timeout);
        requests.delete(message.request_id);
        if (!message.ok) error = message.text || 'FT-Befehl fehlgeschlagen.';
        else if (entry.op === 'ft_config') {
          settingsDirty = false;
          fillSettings();
        }
        break;
      }
      case 'ft-state': {
        const lastMode = state.mode;
        state = message;
        active = message.mode !== 'off';
        if (Number.isFinite(message.utc_ms)) clock = {utc: message.utc_ms, at: performance.now()};
        if (active && message.mode !== mode) {
          preferences.ftMode = mode = message.mode;
          savePreferences();
        }
        if (lastMode !== message.mode) {
          clearFt8();
          clearWaterfall();
          for (const id of ['ft-adif-status', 'ft-qrz-status']) $(id).textContent = '';
          offsetInput.max = mode === 'FT4' ? 3930 : 3950;
          const labels = mode === 'FT4' ? [':00 / :15 / :30 / :45', ':07.5 / :22.5 / :37.5 / :52.5'] : [':00 / :30', ':15 / :45'];
          paritySelect.replaceChildren(new Option('Automatisch', 'auto'), ...labels.map((label, index) => new Option(label, index)));
        }
        if (!settingsDirty) fillSettings();
        renderClock();
        break;
      }
      case 'ft-decode':
      case 'ft-tx':
        if (active && message.mode === mode && message.dial_hz === radio.dial) {
          if (message.type === 'ft-decode') appendDecode(message);
          else appendTx(message);
        }
        break;
      case 'ft-notice':
        if (message.kind === 'adif' || message.kind === 'qrz') {
          const log = $(`ft-${message.kind}-status`);
          log.textContent = message.text;
          log.classList.toggle('setting-error', message.outcome === 'error');
        } else if (message.kind === 'error' || message.kind === 'slot') {
          error = message.text;
        } else {
          outcome = [message.peer, message.outcome === 'completed' && message.kind === 'qso' ? 'QSO abgeschlossen' : message.text]
            .filter(Boolean).join(' · ');
          if (message.outcome === 'completed') error = '';
        }
        break;
    }
    renderView();
  }

  function clearWaterfall() {
    context.clearRect(0, 0, canvas.width, canvas.height);
    waterfallAt = 0;
  }

  function addFrame(levels, halfSpan) {
    const now = performance.now();
    if (!active || now - waterfallAt < 90 || levels.length < 3) return;
    waterfallAt = now;
    const center = Math.floor(levels.length / 2);
    const bins = 4000 * center / halfSpan;
    for (let x = 0; x < canvas.width; ++x) {
      const position = center + (x + .5) * bins / canvas.width;
      const low = Math.floor(position);
      if (low + 1 >= levels.length) break;
      const level = levels[low] + (levels[low + 1] - levels[low]) * (position - low);
      pixels[x] = palette[Math.max(0, Math.min(palette.length - 1, Math.round(level) - minimumLevel))];
    }
    context.drawImage(canvas, 0, 1);
    context.putImageData(row, 0, 0);
  }


  view.addEventListener('change', ({target}) => {
    if (target.name === 'receiver-view') request('ft_mode', {mode: target.value === 'ft' ? mode : 'off'});
  });
  modeSelect.onchange = () => request('ft_mode', {mode: modeSelect.value});
  settings.oninput = () => {
    settingsDirty = true;
    error = '';
    renderView();
  };
  settings.onsubmit = event => {
    event.preventDefault();
    request('ft_config', {callsign: callInput.value, locator: gridInput.value,
      tx_hz: offsetInput.valueAsNumber, tx_level: levelInput.valueAsNumber});
  };
  arm.onchange = () => request('ft_arm', {armed: arm.checked});
  auto.onchange = () => request('ft_auto', {auto: auto.value});
  $('ft-cq').onclick = () => request('ft_cq', paritySelect.value === 'auto' ? {} : {parity: +paritySelect.value});
  $('ft8-stop').onclick = () => request('ft_abort');
  $('ft8-tx-form').onsubmit = event => {
    event.preventDefault();
    request('ft_free_text', {text: $('ft8-message').value});
  };
  $('ft-clear').onclick = clearFt8;
  for (const section of [view, panel]) {
    section.addEventListener('keydown', event => {
      if (event.code === 'Space' || event.key === ' ') event.stopPropagation();
    });
  }
  setInterval(renderClock, 200);

  return {
    update, handle, addFrame, active: () => active, busy: () => !!(state.armed || state.busy),
    setStyle(lower, upper, color) { palette = waterfallColors(color, lower, upper); }
  };
}
