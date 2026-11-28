import {countryFlagPosition, dxccForCall} from '/dxcc.js';
import {language, locale, setMessage, translate} from '/i18n.js';
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
const regionNames = Object.fromEntries(['de', 'en'].map(lang =>
  [lang, new Intl.DisplayNames(lang, {type: 'region', fallback: 'none'})]));

export function createFtPanel({send, preferences, savePreferences}) {
  let radio = {connected: false, dial: 0};
  let state = emptyState;
  let active = false;
  let mode = preferences.ftMode === 'FT4' ? 'FT4' : 'FT8';
  let error = '', outcome = null;
  let revision = 0;
  let settingsDirty = false, settingsSubmitted = false;
  let settingsRevision = 0, settingsTimer = null;
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
  const auto = $('ft-auto');
  const nextTx = $('ft-next-tx');
  const adifDownload = $('ft-adif-download');
  const adifPath = adifDownload.getAttribute('href');
  const adifMessage = $('ft-adif-message');
  const adifDownloadError = $('ft-adif-download-error');
  const qrzStatus = $('ft-qrz-status');
  const freeText = $('ft8-message');
  const stationInputs = [callInput, gridInput, offsetInput, levelInput];
  const requiredInputs = [...stationInputs, freeText];

  adifDownload.onclick = async event => {
    setMessage(adifDownloadError, '');
    if (typeof window.showSaveFilePicker !== 'function' || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
    event.preventDefault();
    try {
      const destination = await window.showSaveFilePicker({suggestedName: adifDownload.download});
      const response = await fetch(adifPath);
      if (!response.ok) {
        setMessage(adifDownloadError, await response.text() || 'ADIF-Download fehlgeschlagen.');
        return;
      }
      await response.body.pipeTo(await destination.createWritable());
    } catch (failure) {
      if (failure.name === 'AbortError') return;
      if (failure.name === 'SecurityError') {
        window.location.assign(adifDownload.href);
        return;
      }
      setMessage(adifDownloadError, 'ADIF-Download fehlgeschlagen.');
    }
  };

  function request(op, values = {}, draft) {
    if (pending(op)) return;
    error = '';
    outcome = null;
    const id = ++revision;
    if (send({op, ...values, request_id: id})) {
      const timeout = setTimeout(() => finishRequest(id, false, 'Keine Antwort vom Backend.'), 6000);
      requests.set(id, {op, timeout, values, draft});
    } else {
      error = 'Backend nicht verbunden.';
    }
    renderView();
  }
  function pending(op) {
    return Array.from(requests.values()).some(request => request.op === op);
  }
  function finishRequest(id, ok, text) {
    const entry = requests.get(id);
    if (!entry) return;
    clearTimeout(entry.timeout);
    requests.delete(id);
    if (entry.op === 'ft_config') {
      if (ok) {
        const {callsign, locator, tx_hz, tx_level} = entry.values;
        Object.assign(preferences, {ftOwnCall: callsign, ftOwnGrid: locator, ftAudioHz: tx_hz, ftTxLevel: tx_level});
        savePreferences();
      }
      if (entry.draft === settingsRevision) {
        if (ok) {
          settingsDirty = false;
          fillSettings();
        } else {
          error = text || 'FT-Befehl fehlgeschlagen.';
        }
      } else if (settingsTimer === null) {
        saveSettings();
      }
    } else if (!ok) {
      error = text || 'FT-Befehl fehlgeschlagen.';
    }
    renderView();
  }
  function canUseControls() {
    return active && radio.connected && state.available;
  }
  function requestTransmission(op, values) {
    if (!canUseControls() || !state.configured || settingsDirty || pending('ft_mode')) {
      renderView();
      return;
    }
    request(op, values);
  }
  function saveSettings() {
    if (!settingsDirty || !radio.connected || !state.available || pending('ft_config')) return;
    for (const input of stationInputs) input.setCustomValidity('');
    const callsign = callInput.value.trim().toUpperCase();
    const locator = gridInput.value.trim().toUpperCase();
    if (!settings.checkValidity() || !callsign || !locator) {
      error = 'Bitte gültige FT-Einstellungen eingeben.';
      renderView();
      return;
    }
    settingsSubmitted = true;
    request('ft_config', {callsign, locator, tx_hz: offsetInput.valueAsNumber, tx_level: levelInput.valueAsNumber},
      settingsRevision);
  }
  function restoreBrowserSettings() {
    if (!canUseControls() || state.configured !== false || settingsDirty || settingsSubmitted ||
        !callInput.value.trim() || !gridInput.value.trim() || !settings.checkValidity()) return;
    settingsDirty = true;
    saveSettings();
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
      line.disabled = !canUseControls() || pending('ft_start_qso');
      line.onclick = () => requestTransmission('ft_start_qso', {peer: source, decode_id: message.decode_id});
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
    ft8Log.output.setAttribute('aria-label', translate(cqOnly.checked ? 'FT4/8-CQ-Rufe' : 'Alle FT4/8-Nachrichten'));
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
  function updateFtCountry(node) {
    const {region, entity} = node.dataset;
    const name = region && region !== 'ZZ' && regionNames[language].of(region);
    node.querySelector('.ft-country-name').textContent = region === 'GB' ? 'UK' : name || translate(entity);
    node.title = name && entity.toLowerCase() === regionNames.en.of(region)?.toLowerCase()
      ? name : translate(entity);
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
      if (nextTx.textContent) nextTx.textContent = '';
      return;
    }
    const now = Math.floor(clock.utc + performance.now() - clock.at);
    label.textContent = `${new Date(now).toISOString().slice(11, 19)} UTC`;
    progress.max = state.slot_ms;
    progress.value = now % state.slot_ms;
    const seconds = state.armed && !state.sending && Number.isFinite(state.next_tx_ms)
      ? Math.ceil((state.next_tx_ms - now) / 1000) : 0;
    const countdown = seconds > 0
      ? `· ${translate(state.next_tx_planned ? 'TX in {seconds} s' : 'TX frühestens in {seconds} s', {seconds})}`
      : '';
    if (nextTx.textContent !== countdown) nextTx.textContent = countdown;
  }

  function renderView() {
    const changingMode = pending('ft_mode');
    const control = canUseControls();
    zoomChoice.checked = !active;
    ftChoice.checked = active;
    ftChoice.disabled = !state.available || !radio.connected || changingMode || (!active && (radio.paused || radio.voiceBusy));
    zoomChoice.disabled = changingMode;
    panel.hidden = !active;
    $('zoom').hidden = active;
    $('ft-clear').hidden = !active;
    $('ft8-frequency').textContent = radio.dial
      ? `MAIN ${(radio.dial / 1e6).toLocaleString(locale, {minimumFractionDigits:6})} MHz · USB` : 'MAIN';
    modeSelect.value = mode;
    modeSelect.disabled = !control || changingMode || radio.paused || radio.voiceBusy;
    auto.value = state.auto;
    auto.disabled = !control || pending('ft_auto');
    paritySelect.disabled = freeText.disabled = !control;
    $('ft-cq').disabled = !control || pending('ft_cq');
    $('ft8-send').disabled = !control || pending('ft_free_text');
    $('ft8-stop').disabled = !control || pending('ft_abort');
    for (const input of stationInputs) {
      input.disabled = !radio.connected || !state.available;
    }
    const replyDisabled = !control || pending('ft_start_qso');
    for (const log of [ft8Log, ftOwnLog]) {
      for (const line of log.lines) {
        if (line.tagName === 'BUTTON') line.disabled = replyDisabled;
      }
    }
    const qso = [state.peer, translate(stateLabels[state.state]),
      state.retries ? translate('Wiederholungen {count}', {count: state.retries}) : ''].filter(Boolean).join(' · ');
    $('ft-contact').hidden = !qso && !state.next_tx_ms;
    $('ft-qso-status').textContent = qso;
    const status = $('ft8-status');
    const needsStation = active && !state.configured;
    status.textContent = translate(error || (state.sending ? 'Sendet …' : settingsDirty ? 'Speichert …' :
      needsStation ? 'Für FT-TX Rufzeichen und QTH-Locator eingeben.' : '')) ||
      (outcome ? [outcome.peer, translate(outcome.text)].filter(Boolean).join(' · ') : '');
    status.classList.toggle('error', !!error);
    status.classList.toggle('sending', state.sending && !error);
  }

  function fillSettings() {
    const saved = state.configured;
    callInput.value = state.callsign || preferences.ftOwnCall || '';
    gridInput.value = state.locator || preferences.ftOwnGrid || '';
    offsetInput.value = saved ? state.tx_hz : Math.max(200, Math.min(Number(offsetInput.max), Number(preferences.ftAudioHz) || 1500));
    levelInput.value = saved ? state.tx_level : preferences.ftTxLevel ?? 25;
    for (const input of stationInputs) input.setCustomValidity('');
  }

  function update(next) {
    if (!next.connected && radio.connected) {
      for (const request of requests.values()) clearTimeout(request.timeout);
      requests.clear();
      state = emptyState;
      active = false;
      clock = null;
      clearTimeout(settingsTimer);
      settingsTimer = null;
      settingsDirty = settingsSubmitted = false;
      error = '';
      outcome = null;
      fillSettings();
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
      case 'ft-reply':
        finishRequest(message.request_id, message.ok, message.text);
        return;
      case 'ft-state': {
        const lastMode = state.mode;
        if (state.receive_revision !== message.receive_revision) clearFt8();
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
          setMessage(adifMessage, '');
          setMessage(adifDownloadError, '');
          setMessage(qrzStatus, '');
          offsetInput.max = mode === 'FT4' ? 3930 : 3950;
          const labels = mode === 'FT4' ? [':00 / :15 / :30 / :45', ':07.5 / :22.5 / :37.5 / :52.5'] : [':00 / :30', ':15 / :45'];
          paritySelect.replaceChildren(new Option(translate('Automatisch'), 'auto'), ...labels.map((label, index) => new Option(label, index)));
        }
        if (!settingsDirty) fillSettings();
        restoreBrowserSettings();
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
        if (message.kind === 'slot') return;
        if (message.kind === 'adif' || message.kind === 'qrz') {
          const adif = message.kind === 'adif';
          const log = adif ? adifMessage : qrzStatus;
          setMessage(log, adif ? message.text.replace(/^ADIF: */, '') : message.text);
          log.classList.toggle('setting-error', message.outcome === 'error');
        } else if (message.kind === 'error') {
          error = message.text;
        } else {
          outcome = {peer: message.peer,
            text: message.outcome === 'completed' && message.kind === 'qso' ? 'QSO abgeschlossen' : message.text};
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

  function languageChanged() {
    adifDownload.href = language === 'en' ? `${adifPath}?lang=en` : adifPath;
    paritySelect.options[0].textContent = translate('Automatisch');
    for (const node of panel.querySelectorAll('.ft-country')) updateFtCountry(node);
    for (const input of requiredInputs) input.setCustomValidity('');
    applyCqOnly();
    renderView();
    renderClock();
  }

  view.addEventListener('change', ({target}) => {
    if (target.name === 'receiver-view') request('ft_mode', {mode: target.value === 'ft' ? mode : 'off'});
  });
  modeSelect.onchange = () => request('ft_mode', {mode: modeSelect.value});
  settings.oninput = () => {
    ++settingsRevision;
    settingsDirty = true;
    error = '';
    clearTimeout(settingsTimer);
    settingsTimer = setTimeout(() => {
      settingsTimer = null;
      saveSettings();
    }, 1000);
    renderView();
  };
  settings.onsubmit = event => event.preventDefault();
  auto.onchange = () => {
    const values = {auto: auto.value};
    if (values.auto === 'off') request('ft_auto', values);
    else requestTransmission('ft_auto', values);
  };
  $('ft-cq').onclick = () => requestTransmission('ft_cq', paritySelect.value === 'auto' ? {} : {parity: +paritySelect.value});
  $('ft8-stop').onclick = () => request('ft_abort');
  $('ft8-tx-form').onsubmit = event => {
    event.preventDefault();
    requestTransmission('ft_free_text', {text: freeText.value});
  };
  for (const input of requiredInputs) {
    input.addEventListener('input', () => input.setCustomValidity(''));
    input.addEventListener('invalid', () => {
      input.setCustomValidity('');
      const validity = input.validity;
      let text = 'Einen gültigen Wert eingeben.';
      let values;
      if (validity.valueMissing) text = 'Dieses Feld ausfüllen.';
      else if (validity.tooShort) {
        text = 'Mindestens {min} Zeichen eingeben.';
        values = {min: input.minLength};
      } else if (validity.tooLong) {
        text = 'Höchstens {max} Zeichen eingeben.';
        values = {max: input.maxLength};
      } else if (validity.rangeUnderflow || validity.rangeOverflow || validity.stepMismatch) {
        text = 'Eine ganze Zahl zwischen {min} und {max} eingeben.';
        values = {min: input.min, max: input.max};
      } else if (validity.valid) return;
      input.setCustomValidity(translate(text, values));
    });
  }
  $('ft-clear').onclick = clearFt8;
  for (const section of [view, panel]) {
    section.addEventListener('keydown', event => {
      if (event.code === 'Space' || event.key === ' ') event.stopPropagation();
    });
  }
  setInterval(renderClock, 200);

  return {
    update, handle, addFrame, languageChanged, active: () => active, busy: () => state.busy || state.auto !== 'off',
    setStyle(lower, upper, color) { palette = waterfallColors(color, lower, upper); }
  };
}
