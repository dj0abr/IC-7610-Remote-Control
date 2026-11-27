import {BrowserAudio, grantMicrophoneAccess} from '/audio.js';
import {PlotPair} from '/plots.js';
import {Microphone} from '/tx-audio.js';
import {createFtPanel} from '/ft-panel.js';
import {language, locale, setLanguage, setMessage, translate} from '/i18n.js';

const element = id => document.getElementById(id);
const bandSelect = element('band');
const modeSelect = element('sideband');
const volume = element('volume');
const filterLower = element('filter-lower');
const filterUpper = element('filter-upper');
const filterNoise = element('nr');
const notchModes = element('notch-mode');
const notchFrequency = element('notch-hz');
const notchManual = element('notch-manual');
const notchAuto = element('notch-auto');
const notchDetected = element('notch-detected');
const filterPresets = document.querySelectorAll('.filter-presets button');
const filterError = element('filter-error');
const rfPower = element('rf-power');
const rfPowerValue = element('rf-power-value');
const rfPowerError = element('rf-power-error');
const radioOptions = {
  compressor: {select: element('compressor'), error: element('compressor-error'), value: null, max: 1},
  preamp: {select: element('preamp'), error: element('preamp-error'), value: null, max: 2},
};
const setup = element('setup');
const languageSelect = element('language');
const addresses = ['radio-ip', 'pi-ip'].map(id => ({
  key: id.replace('-', '_'), input: element(id), error: element(`${id}-error`), pending: null,
}));
const status = element('status');
const audioButton = element('audio-toggle');
const audioError = element('audio-error');
const pttButton = element('ptt');
const pttModeControl = element('ptt-mode');
const pttHint = element('ptt-hint');
const txIndicator = element('tx-state');
const rxMeter = element('rx-meter');
const rxMeterBar = element('rx-meter-bar');
const rxMeterValue = element('rx-meter-value');
const txMeters = element('tx-meters');
const txPower = element('tx-output-power');
const txSwr = element('tx-swr');
const txPowerBar = element('tx-power-bar');
const txSwrBar = element('tx-swr-bar');
let txMeterFormat, frequencyFormat, rangeFormat;
const txError = element('tx-error');
const browserSpeaker = element('browser-speaker');
const browserMicrophone = element('browser-microphone');
const allowAudioDevices = element('allow-audio-devices');
const allowMicrophones = element('allow-microphones');
const mediaDevices = navigator.mediaDevices;
const audioDevicePicker = typeof mediaDevices?.selectAudioOutput === 'function';
const canCapture = typeof mediaDevices?.getUserMedia === 'function';
const lowerControl = element('lower');
const upperControl = element('upper');
const colorControl = element('color');
const supportedColors = ['blue', 'red', 'green'];
const littleEndian = new Uint8Array(new Uint16Array([1]).buffer)[0] === 1;
const bandDecoder = new TextDecoder();
const headerBytes = 28;
const preferenceKey = 'kmcontrol-display';

function getPreferences() {
  try {
    const saved = JSON.parse(localStorage.getItem(preferenceKey));
    return saved && typeof saved === 'object' && !Array.isArray(saved) ? saved : {};
  } catch {
    return {};
  }
}

const preferences = getPreferences();
const storedThresholds = preferences.bandThresholds;
const bandThresholds = Object.assign(Object.create(null),
  storedThresholds && typeof storedThresholds === 'object' && !Array.isArray(storedThresholds) ? storedThresholds : {});
const defaultThresholds = {
  lower: Number.isInteger(preferences.lower) ? preferences.lower : -120,
  upper: Number.isInteger(preferences.upper) ? preferences.upper : -25,
};
const requestedBand = new URLSearchParams(location.search).get('band');
let preferredBand = requestedBand || preferences.band || '40m';
let color = supportedColors.includes(preferences.color) ? preferences.color : 'blue';
let socket;
let reconnectTimer = 0;
let reconnectDelay = 1000;
let activeBand = null;
let bands = [];
let mainHz = 0;
let mainConfirmed = false;
let halfSpan = 30000;
let spectrumLive = false;
let setupState = null;
let rxFilter = null;
let detectedNotchHz = null;
let filterTimer = 0;
let filterRevision = 0;
let nextSetupId = 0;
let lastProblem = '';
let confirmedPower = null;
let editingPower = false;
let audioEnabled = false;
let audioStarting = false;
let audioRouting = Promise.resolve();
let browserDevices = [];
let deviceRefresh = 0;
let requestingDevices = false;
let tx = {phase: 'offline', ready: false};
let txMeterReadings;
let micStart = null;
let microphoneError = '';
let pttId = 0;
let nextPttId = 0;
let pttMode = preferences.pttMode === 'toggle' ? 'toggle' : 'hold';
const pttSources = new Set();

const audio = new BrowserAudio();
const microphone = new Microphone(sendMicrophoneFrame, () => {
  stopMicrophone();
  setMicrophoneError('Mikrofonverbindung unterbrochen.');
});
const overview = new PlotPair(element('overview'), false, tuneMain, stepMain);
const zoom = new PlotPair(element('zoom'), true, tuneMain, stepMain);
const ft = createFtPanel({send, preferences, savePreferences});

function updateFt() {
  ft.update({connected: socket?.readyState === WebSocket.OPEN, band: activeBand, dial: mainHz,
    paused: receivePaused(), voiceBusy: !!pttId});
}

function savePreferences() {
  try {
    localStorage.setItem(preferenceKey, JSON.stringify(preferences));
  } catch {
    // A browser can block persistent storage.
  }
}

function setStatus(text, phase = '') {
  setMessage(status, text, 'value');
  status.dataset.phase = phase;
}

function send(command) {
  if (socket?.readyState !== WebSocket.OPEN) {
    return false;
  }
  socket.send(JSON.stringify(command));
  return true;
}

function tuneMain(hz) {
  if (spectrumLive && !receivePaused() && !pttId) {
    send({op: 'set_main', hz});
  }
}

function stepMain(steps) {
  if (spectrumLive && !receivePaused() && !pttId) {
    send({op: 'step_main', steps});
  }
}

function receivePaused() {
  return tx.phase !== 'rx' && tx.phase !== 'offline';
}

function showTxError(text) {
  setMessage(txError, text, 'value');
  txError.hidden = !text;
}

function setMicrophoneError(text) {
  microphoneError = text;
  showTxError(tx.text || microphoneError);
}

function canStartPtt() {
  return audioEnabled && microphone.ready && tx.ready && tx.phase === 'rx' && !ft.busy();
}

function renderTx() {
  updateFt();
  renderAudioButton();
  renderRfPower();
  renderRadioOptions();
  pttButton.disabled = !pttId && !canStartPtt();
  pttButton.setAttribute('aria-pressed', String(!!pttId));
  pttHint.textContent = translate(pttMode === 'toggle' ? 'Rasten · Leertaste' : 'Halten · Leertaste');
  txIndicator.dataset.phase = tx.phase;
  txIndicator.value = translate(({
    rx: 'RX', starting: 'TX startet …', tx: 'TX', ending: 'RX wird geprüft …', unknown: 'RX prüfen',
  })[tx.phase]);
  bandSelect.disabled = !bands.length || !activeBand || receivePaused() || !!pttId;
  renderDeviceControls();
}

function sendMicrophoneFrame(id, packet) {
  if (pttId && id === pttId && socket?.readyState === WebSocket.OPEN && socket.bufferedAmount < 2048) {
    socket.send(packet);
  }
}

function pressPtt(source) {
  if (pttSources.has(source)) {
    return;
  }
  if (pttId) {
    if (pttMode === 'toggle') {
      stopPtt();
    } else {
      pttSources.add(source);
    }
    return;
  }
  if (!canStartPtt()) {
    return;
  }
  const id = nextPttId = (nextPttId + 1) >>> 0 || 1;
  if (!send({op: 'ptt', action: 'start', id, mode: pttMode})) {
    return;
  }
  pttSources.add(source);
  pttId = id;
  showTxError('');
  microphone.capture(id);
  renderTx();
}

function releasePtt(source) {
  const released = pttSources.delete(source);
  if (released && pttMode === 'hold' && !pttSources.size) {
    stopPtt();
  }
  return released;
}

function stopPtt() {
  pttSources.clear();
  if (!pttId) {
    return;
  }
  const id = pttId;
  pttId = 0;
  microphone.capture(0);
  send({op: 'ptt', action: 'stop', id});
  renderTx();
}

function stopMicrophone() {
  micStart = null;
  stopPtt();
  microphone.stop();
  renderTx();
}

function preferredMicrophone() {
  return typeof preferences.browserMicrophone === 'string' ? preferences.browserMicrophone : '';
}

async function startMicrophone() {
  const device = preferredMicrophone();
  const request = micStart = {};
  setMicrophoneError('');
  renderTx();
  try {
    await microphone.start(device);
    void refreshBrowserDevices();
  } catch (error) {
    if (request !== micStart) {
      return;
    }
    const missing = device && ['NotFoundError', 'OverconstrainedError'].includes(error.name);
    if (missing) {
      setMicrophoneError('Das gewählte Eingabegerät ist nicht verfügbar.');
      void refreshBrowserDevices();
    } else if (error.name === 'NotAllowedError') {
      setMicrophoneError('Der Browser hat den Mikrofonzugriff blockiert. PTT ist deaktiviert.');
    } else {
      setMicrophoneError(error instanceof DOMException || !error.message
        ? 'Mikrofon konnte nicht geöffnet werden.' : error.message);
    }
  } finally {
    if (request === micStart) {
      micStart = null;
      renderTx();
    }
  }
}

function selectMicrophone() {
  if (browserMicrophone.disabled) {
    browserMicrophone.value = preferredMicrophone();
    return;
  }
  preferences.browserMicrophone = browserMicrophone.value;
  savePreferences();
  if (audioEnabled) {
    stopMicrophone();
    void startMicrophone();
  }
}

function meterValue(raw, points) {
  for (let i = 1; i < points.length; ++i) {
    const [upper, high] = points[i];
    if (raw <= upper) {
      const [lower, low] = points[i - 1];
      return low + (high - low) * (raw - lower) / (upper - lower);
    }
  }
  return null;
}

function renderRxMeter(level = null) {
  // CI-V 15 02 uses S0 = 0, S9 = 120 and S9 +60 dB = 241.
  const maximum = 241;
  rxMeterBar.style.setProperty('--meter-level', `${Math.min(level ?? 0, maximum) / maximum * 100}%`);
  const aboveS9 = Math.round((Math.min(level, maximum) - 120) * 60 / (maximum - 120));
  rxMeterValue.textContent = level === null ? '—' :
    aboveS9 <= 0 ? `S${Math.min(9, Math.round(level * 9 / 120))}` :
    `S9 +${aboveS9} dB`;
}

function renderTxMeters(message) {
  txMeterReadings = message;
  const {po = null, swr = null} = message ?? {};
  // CI-V reports positions on the Icom's meter scales (reference, commands 15 11 and 15 12).
  const watts = po === null ? null : meterValue(po, [[0, 0], [143, 50], [212, 100]]);
  const ratio = swr === null ? null : meterValue(swr, [[0, 1], [48, 1.5], [80, 2], [120, 3]]);
  const hasSwr = po > 0 && swr !== null;
  const powerLevel = po === null ? 0 : watts ?? 100;
  const swrLevel = hasSwr ? ((ratio ?? 3) - 1) * 50 : 0;
  txPowerBar.style.setProperty('--meter-level', `${powerLevel}%`);
  txSwrBar.style.setProperty('--meter-level', `${swrLevel}%`);
  if (hasSwr) {
    const value = ratio ?? Infinity;
    txSwrBar.dataset.swrRange = value < 1.8 ? 'low' : value <= 2.5 ? 'medium' : 'high';
  } else {
    delete txSwrBar.dataset.swrRange;
  }
  txPower.textContent = po === null ? '—' : po > 212 ? '≥ 100 W' : watts === 0 ? '0 W' :
    `≈ ${watts < 10 ? txMeterFormat.format(watts) : Math.round(watts)} W`;
  txSwr.textContent = hasSwr ?
    (swr > 120 ? '> 3:1' : `${txMeterFormat.format(ratio)}:1`) : '—';
}

function updateTx(message) {
  const previous = tx.phase;
  tx = message;
  if (previous !== tx.phase) {
    const showTxMeters = ['starting', 'tx', 'ending'].includes(tx.phase);
    rxMeter.hidden = showTxMeters;
    txMeters.hidden = !showTxMeters;
    renderRxMeter();
    renderTxMeters();
  }
  if (!tx.ready || (tx.phase === 'rx' && (previous !== 'rx' || tx.text))) {
    stopPtt();
  }
  showTxError(tx.text || microphoneError);
  if (receivePaused()) {
    audio.reset();
  }
  renderTx();
}

function setMain(hz, confirmed = false) {
  mainHz = Number.isSafeInteger(hz) && hz > 0 ? hz : 0;
  mainConfirmed = confirmed;
  element('main-frequency').value = mainHz ?
    frequencyFormat.format(mainHz / 1e6).replace(/(\d{3})(\d{3})$/, '$1\u202f$2') : '—';
  overview.setMain(mainHz, halfSpan);
  zoom.setMain(mainHz, halfSpan);
  updateFt();
}

function resetPlots() {
  overview.clear();
  zoom.clear();
  spectrumLive = false;
  updateFt();
}

function updateThresholds(lower, upper) {
  lower = Math.max(-160, Math.min(-1, lower));
  upper = Math.max(lower + 1, Math.min(0, upper));
  lowerControl.value = lower;
  upperControl.value = upper;
  element('lower-value').value = `${String(lower).replace('-', '−')} dBFS`;
  element('upper-value').value = `${String(upper).replace('-', '−')} dBFS`;
  overview.setStyle(lower, upper, color);
  zoom.setStyle(lower, upper, color);
  ft.setStyle(lower, upper, color);
  return {lower, upper};
}

function restoreBandThresholds(id) {
  const saved = bandThresholds[id];
  updateThresholds(
    Number.isInteger(saved?.lower) ? saved.lower : defaultThresholds.lower,
    Number.isInteger(saved?.upper) ? saved.upper : defaultThresholds.upper,
  );
}

function saveBandThresholds(lower, upper) {
  const thresholds = updateThresholds(lower, upper);
  const id = bandSelect.value;
  if (id) {
    bandThresholds[id] = thresholds;
    preferences.bandThresholds = bandThresholds;
    savePreferences();
  }
}

function setVolume(percent) {
  volume.value = percent;
  element('volume-value').value = `${percent} %`;
  audio.setVolume(percent);
}

function renderRfPower() {
  rfPower.disabled = confirmedPower === null || tx.phase !== 'rx' || !!pttId;
  if (rfPower.disabled) {
    editingPower = false;
  }
  if (!editingPower) {
    rfPower.value = confirmedPower ?? 0;
    rfPowerValue.value = confirmedPower === null ? '—' : `${confirmedPower} %`;
  }
}

function updateRfPower(percent, text = '') {
  confirmedPower = Number.isInteger(percent) && percent >= 0 && percent <= 100 ? percent : null;
  setMessage(rfPowerError, text, 'value');
  rfPowerError.hidden = !text;
  renderRfPower();
}

function renderRadioOptions() {
  const busy = tx.phase !== 'rx' || !!pttId;
  for (const option of Object.values(radioOptions)) {
    option.select.disabled = busy || option.value === null || (option === radioOptions.compressor && ft.active());
    option.select.value = option.value ?? '';
  }
}

function updateRadioOption(option, value, text = '') {
  option.value = Number.isInteger(value) && value >= 0 && value <= option.max ? value : null;
  setMessage(option.error, text);
  option.error.hidden = !text;
  renderRadioOptions();
}

function paintFilterRange(control) {
  const min = Number(control.min);
  const span = Number(control.max) - min;
  control.style.setProperty('--fill', `${span > 0 ? (Number(control.value) - min) / span * 100 : 0}%`);
}

function renderNotch() {
  const mode = rxFilter?.notch_mode ?? 'off';
  notchModes.disabled = !rxFilter?.notch_mode || rxFilter.ft_bypass;
  for (const radio of notchModes.elements) {
    radio.checked = radio.value === mode;
  }
  notchManual.hidden = mode !== 'manual';
  notchFrequency.disabled = notchManual.hidden;
  notchAuto.hidden = mode !== 'auto';
  if (!notchManual.hidden) {
    notchFrequency.value = rxFilter.notch_hz;
    element('notch-hz-value').value = `${rxFilter.notch_hz.toLocaleString(locale)} Hz`;
    paintFilterRange(notchFrequency);
  }
  notchDetected.value = !audioEnabled ? translate('Audio aus') :
    detectedNotchHz == null ? '—' : `${detectedNotchHz.toLocaleString(locale)} Hz`;
}

function renderFilter() {
  renderNotch();
  const sideband = rxFilter?.ft_bypass ? 'USB' : modeSelect.value;
  element('filter-mode').textContent = sideband ? translate('MAIN · {sideband}-Filter', {sideband}) : translate('MAIN · RX-Filter');
  for (const control of [filterLower, filterUpper, ...filterPresets]) {
    control.disabled = !rxFilter;
  }
  filterNoise.disabled = !rxFilter || rxFilter.ft_bypass;
  const zoomLabel = element('zoom-filter');
  zoomLabel.hidden = !rxFilter || !sideband;
  if (!rxFilter) {
    zoom.setPassband(0, 0);
    return;
  }

  const {low_hz: low, high_hz: high, nr_percent: noise} = rxFilter;
  const lowNumber = low.toLocaleString(locale);
  const lowLabel = `${lowNumber} Hz`;
  const highLabel = `${high.toLocaleString(locale)} Hz`;
  filterLower.max = high - 500;
  filterUpper.min = low + 500;
  filterLower.value = low;
  filterUpper.value = high;
  filterNoise.value = noise;
  element('filter-lower-value').value = lowLabel;
  element('filter-upper-value').value = highLabel;
  element('filter-low').textContent = lowLabel;
  element('filter-high').textContent = highLabel;
  element('nr-value').value = `${noise} %`;
  for (const control of [filterLower, filterUpper, filterNoise]) {
    paintFilterRange(control);
  }
  for (const button of filterPresets) {
    button.setAttribute('aria-pressed', String(Number(button.dataset.bandwidth) === high - low));
  }
  zoomLabel.textContent = `${sideband} · ${lowNumber}–${highLabel}`;
  zoom.setPassband(sideband === 'LSB' ? -high : sideband === 'USB' ? low : 0,
    sideband === 'LSB' ? -low : sideband === 'USB' ? high : 0);
}

function changeFilter(patch, delay = 100) {
  if (!rxFilter) {
    return;
  }
  clearTimeout(filterTimer);
  if ('notch_mode' in patch || 'low_hz' in patch || 'high_hz' in patch) {
    detectedNotchHz = null;
  }
  rxFilter = {...rxFilter, ...patch};
  const request = {op: 'rx_filter', ...rxFilter, request_id: ++filterRevision};
  filterError.hidden = true;
  renderFilter();
  if (delay) {
    filterTimer = setTimeout(() => send(request), delay);
  } else {
    send(request);
  }
}

function installFilter(message) {
  if (message.request_id != null && message.request_id !== filterRevision) {
    return;
  }
  const {low_hz, high_hz, nr_percent, notch_mode, notch_hz, notch_detected_hz, ft_bypass} = message;
  rxFilter = {low_hz, high_hz, nr_percent, notch_mode, notch_hz, ft_bypass: ft_bypass === true};
  detectedNotchHz = notch_detected_hz;
  setMessage(filterError, message.text);
  filterError.hidden = !message.text;
  renderFilter();
}

function showAudioError(text) {
  setMessage(audioError, text);
  audioError.hidden = !text;
}

function renderAudioButton() {
  renderNotch();
  audioButton.disabled = audioStarting || !setupState || (requestingDevices && !audioEnabled);
  audioButton.textContent = translate(audioStarting ? 'Audio startet …' : audioEnabled ? 'Audio ausschalten' : 'Audio einschalten');
  audioButton.setAttribute('aria-pressed', String(audioEnabled));
}

function preferredBrowserSpeaker() {
  return typeof preferences.browserSpeaker === 'string' ? preferences.browserSpeaker : '';
}

function routeBrowserAudio() {
  audioRouting = audioRouting.then(async () => {
    const device = preferredBrowserSpeaker();
    try {
      await audio.setOutput(device);
      showAudioError('');
    } catch {
      if (device !== preferredBrowserSpeaker()) {
        return;
      }
      const standard = device && await audio.setOutput('').then(() => true, () => false);
      showAudioError(standard ?
        'Das gewählte Wiedergabegerät ist nicht verfügbar. Ton läuft über den Systemstandard.' :
        'Der Browser konnte das Wiedergabegerät nicht umschalten.');
    }
  });
  return audioRouting;
}

function selectBrowserSpeaker(device) {
  preferences.browserSpeaker = device;
  savePreferences();
  if (audioEnabled || audioStarting) {
    void routeBrowserAudio();
  }
}

function disableAudio() {
  audioEnabled = false;
  stopMicrophone();
  setMicrophoneError('');
  audio.stop();
  send({op: 'audio_output', output: 'off'});
}

async function toggleAudio() {
  showAudioError('');
  if (audioEnabled) {
    disableAudio();
    return;
  }
  const connection = socket;
  audioStarting = true;
  renderTx();
  try {
    // Start both audio contexts in the button's user gesture.
    const playback = audio.start();
    void startMicrophone();
    await playback;
    await routeBrowserAudio();
    if (connection !== socket || !send({op: 'audio_output', output: 'browser'})) {
      throw new Error('Backend nicht verbunden.');
    }
    audioEnabled = true;
  } catch (error) {
    disableAudio();
    showAudioError(error instanceof DOMException || !error.message
      ? 'Audio konnte nicht gestartet werden.' : error.message);
  } finally {
    audioStarting = false;
    renderTx();
  }
}

function selectBand(id) {
  const band = bands.find(entry => entry.id === id);
  if (!band) {
    return;
  }
  activeBand = band;
  preferredBand = id;
  preferences.band = id;
  savePreferences();
  bandSelect.value = id;
  modeSelect.value = '';
  modeSelect.disabled = true;
  renderFilter();
  overview.setBand(band);
  zoom.clear();
  restoreBandThresholds(id);
  spectrumLive = false;
  updateFt();
  renderBandRange();
  audio.reset();
  setStatus('Band wird eingestellt …', 'connecting');
  send({op: 'band', id});
}

function renderBandRange() {
  if (!activeBand) return;
  const band = activeBand;
  const low = rangeFormat.format(band.low_hz / 1e6);
  const high = rangeFormat.format(band.high_hz / 1e6);
  const center = rangeFormat.format(band.center_hz / 1e6);
  element('band-range').value = `${low}–${high} MHz`;
  element('sub-frequency').value = `${center} MHz`;
  element('fullband-range').textContent = `${rangeFormat.format((band.display_high_hz - band.display_low_hz) / 1000)} kHz`;
}

function installBands(message) {
  if (message.version !== 1 || !Array.isArray(message.bands) || message.bands.length === 0) {
    setStatus('Unbekannte Protokollversion', 'error');
    return;
  }
  bands = message.bands;
  halfSpan = message.zoom_half_span_hz || 30000;
  element('zoom-range').textContent = `MAIN ±${rangeFormat.format(halfSpan / 1000)} kHz`;
  zoom.setMain(mainHz, halfSpan);
  bandSelect.replaceChildren(...bands.map(band => new Option(band.label, band.id)));
  bandSelect.disabled = false;
  reconnectDelay = 1000;
  selectBand(bands.some(band => band.id === preferredBand) ? preferredBand : bands[0].id);
  lowerControl.disabled = upperControl.disabled = false;
}

function fillDeviceSelect(select, selected, devices) {
  select.replaceChildren(new Option(translate('Systemstandard'), ''));
  for (const device of devices) {
    select.add(new Option(device.label, device.deviceId));
  }
  if (selected && !devices.some(device => device.deviceId === selected)) {
    select.add(new Option(translate('Gespeichertes Gerät (nicht verfügbar)'), selected));
  }
  select.value = selected;
}

function renderDeviceControls() {
  const busy = !!micStart || requestingDevices || !!pttId || receivePaused();
  const needInputs = canCapture && !microphone.ready && !browserDevices.some(device => device.kind === 'audioinput');
  const needOutputs = audio.canSelectOutput && !browserDevices.some(device => device.kind === 'audiooutput');
  browserSpeaker.disabled = !audio.canSelectOutput;
  browserMicrophone.disabled = !canCapture || busy;
  allowMicrophones.hidden = !audioDevicePicker || !needInputs;
  allowMicrophones.disabled = busy;
  allowAudioDevices.disabled = audioDevicePicker ? requestingDevices : busy;
  element('browser-device-access').hidden = audioDevicePicker ? !audio.canSelectOutput :
    !canCapture || (!needInputs && !needOutputs);
  element('audio-device-hint').hidden = audioDevicePicker;
}

function renderBrowserDevices() {
  fillDeviceSelect(browserSpeaker, preferredBrowserSpeaker(),
    audio.canSelectOutput ? browserDevices.filter(device => device.kind === 'audiooutput') : []);
  fillDeviceSelect(browserMicrophone, preferredMicrophone(),
    browserDevices.filter(device => device.kind === 'audioinput'));
  renderDeviceControls();
}

async function listBrowserDevices() {
  const devices = await mediaDevices?.enumerateDevices?.() || [];
  return devices.filter(device => device.label && device.deviceId &&
    !['default', 'communications'].includes(device.deviceId));
}

function showBrowserDeviceError(text) {
  const error = element('browser-device-error');
  setMessage(error, text);
  error.hidden = !text;
}

async function refreshBrowserDevices() {
  if (!setup.open || requestingDevices) {
    return;
  }
  const refresh = ++deviceRefresh;
  try {
    const devices = await listBrowserDevices();
    if (refresh === deviceRefresh) {
      browserDevices = devices;
      renderBrowserDevices();
      showBrowserDeviceError('');
    }
  } catch {
    if (refresh === deviceRefresh) {
      showBrowserDeviceError('Die Audiogeräte konnten nicht geladen werden.');
    }
  }
}

async function grantBrowserDevices(inputOnly = false) {
  ++deviceRefresh;
  requestingDevices = true;
  renderTx();
  showBrowserDeviceError('');
  try {
    if (inputOnly) {
      await grantMicrophoneAccess();
    } else {
      const selected = await audio.requestOutputAccess();
      if (selected !== undefined) {
        selectBrowserSpeaker(selected);
      }
    }
    browserDevices = await listBrowserDevices();
    renderBrowserDevices();
  } catch {
    showBrowserDeviceError('Der Browser hat keine weiteren Audiogeräte freigegeben.');
  } finally {
    requestingDevices = false;
    renderTx();
  }
}

function installSetup(message) {
  const first = !setupState;
  setupState = message;
  if (!first) {
    return;
  }
  for (const address of addresses) {
    address.input.value = message[address.key];
    address.input.disabled = false;
    showAddressError(address, '');
  }
  setVolume(message.volume);
  volume.disabled = false;
  renderTx();
}

function showAddressError(address, text, invalid = false) {
  setMessage(address.error, text);
  address.error.hidden = !text;
  if (invalid) {
    address.input.setAttribute('aria-invalid', 'true');
  } else {
    address.input.removeAttribute('aria-invalid');
  }
}

function validIp(value) {
  const parts = value.split('.');
  return parts.length === 4 && parts.every(part => /^(0|[1-9]\d{0,2})$/.test(part) && Number(part) <= 255);
}

function commitAddress(address) {
  if (!setupState) {
    return;
  }
  const value = address.input.value.trim();
  if (!validIp(value)) {
    showAddressError(address, 'Gültige IPv4-Adresse eingeben.', true);
    return;
  }
  address.input.value = value;
  if (value === (address.pending?.value ?? setupState[address.key])) {
    return;
  }
  const id = ++nextSetupId;
  if (send({op: 'setup', [address.key]: value, request_id: id})) {
    address.pending = {id, value};
    showAddressError(address, '');
  } else {
    showAddressError(address, 'Backend nicht verbunden.');
  }
}

function setupResult(message) {
  const address = addresses.find(field => field.pending && field.pending.id === message.request_id);
  if (!address) {
    return;
  }
  if (!message.ok && address.input.value === address.pending.value) {
    showAddressError(address, message.text || 'Adresse konnte nicht übernommen werden.');
  }
  address.pending = null;
}

function handleState(message) {
  if (message.band && message.band !== activeBand?.id) {
    return;
  }
  if (message.phase !== 'idle' || !activeBand) {
    setStatus(message.text, message.phase);
  }
  if (message.phase === 'error' && !message.band) {
    lastProblem = message.text;
  }
  if (!receivePaused() && message.phase !== 'live' && message.phase !== 'idle') {
    resetPlots();
    audio.reset();
  }
}

function control(message) {
  switch (message.type) {
    case 'tx':
      updateTx(message);
      break;
    case 'tx-meters':
      if (tx.phase === 'tx') {
        renderTxMeters(message);
      }
      break;
    case 'rx-meter':
      if (tx.phase === 'rx') {
        renderRxMeter(message.level);
      }
      break;
    case 'bands':
      installBands(message);
      break;
    case 'state':
      handleState(message);
      break;
    case 'setup':
      installSetup(message);
      break;
    case 'rf-power':
      updateRfPower(message.percent, message.text);
      break;
    case 'compressor':
    case 'preamp':
      updateRadioOption(radioOptions[message.type], message.value, message.text);
      break;
    case 'sideband':
      if (message.band === activeBand?.id && ['USB', 'LSB'].includes(message.mode)) {
        modeSelect.value = message.mode;
        modeSelect.disabled = false;
        renderFilter();
      }
      break;
    case 'main-sideband':
      setMessage(modeSelect, message.text || `MAIN am IC-7610: ${message.mode}`, 'title');
      break;
    case 'rx-filter':
      installFilter(message);
      break;
    case 'notch-frequency':
      if (rxFilter?.notch_mode === 'auto') {
        detectedNotchHz = message.hz;
        renderNotch();
      }
      break;
    case 'setup-result':
      setupResult(message);
      break;
    case 'audio-reset':
      audio.reset();
      break;
    case 'main':
      setMain(message.hz, message.confirmed === true);
      break;
    case 'ft-state':
      ft.handle(message);
      renderTx();
      break;
    case 'ft-reply':
    case 'ft-tx':
    case 'ft-decode':
    case 'ft-notice':
      ft.handle(message);
      break;
    case 'main-status':
      setMessage(element('main-frequency'), message.text, 'title');
      break;
    case 'bridge-status':
      if (!lastProblem || message.text !== 'Backend-Verbindung beendet') {
        lastProblem = message.text;
      }
      setStatus(lastProblem, 'error');
      break;
  }
}

function binary(data) {
  if (receivePaused()) {
    return;
  }
  if (data.byteLength < headerBytes) {
    return;
  }
  const view = new DataView(data);
  const kind = view.getUint8(5);
  const count = view.getUint16(6, true);
  if (view.getUint32(0) !== 0x49433736 || view.getUint8(4) !== 1 ||
      count === 0 || headerBytes + count * 2 !== data.byteLength) {
    return;
  }
  if (kind === 3) {
    if (audioEnabled) {
      audio.receive(view, headerBytes, count);
    }
    return;
  }
  if (!activeBand || (kind !== 1 && kind !== 2)) {
    return;
  }
  const name = new Uint8Array(data, 12, 16);
  const end = name.indexOf(0);
  const band = bandDecoder.decode(name.subarray(0, end < 0 ? name.length : end));
  const center = view.getUint32(8, true);
  if (band !== activeBand.id || (kind === 2 && center !== mainHz)) {
    return;
  }
  const levels = littleEndian ? new Int16Array(data, headerBytes, count) :
    Int16Array.from({length: count}, (_, bin) => view.getInt16(headerBytes + bin * 2, true));
  if (kind === 1) {
    overview.addFrame(levels, 0);
    if (!spectrumLive) {
      spectrumLive = true;
      updateFt();
    }
  } else {
    if (!element('zoom').hidden) {
      zoom.addFrame(levels, center);
    }
    ft.addFrame(levels, halfSpan);
  }
}

function connect() {
  reconnectTimer = 0;
  const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const connection = new WebSocket(`${protocol}//${location.host}/ws`);
  socket = connection;
  connection.binaryType = 'arraybuffer';
  connection.onopen = () => {
    lastProblem = '';
    updateFt();
    setStatus('Backend verbinden …', 'connecting');
  };
  connection.onmessage = event => {
    if (connection !== socket) {
      return;
    }
    if (typeof event.data === 'string') {
      try {
        control(JSON.parse(event.data));
      } catch (error) {
        console.error(translate('Ungültige Backend-Nachricht'), error);
      }
    } else {
      binary(event.data);
    }
  };
  connection.onclose = () => {
    if (connection !== socket) {
      return;
    }
    bandSelect.disabled = true;
    modeSelect.disabled = true;
    setMessage(modeSelect, '', 'title');
    volume.disabled = true;
    activeBand = null;
    setupState = null;
    updateTx({phase: 'offline', ready: false});
    clearTimeout(filterTimer);
    rxFilter = null;
    detectedNotchHz = null;
    filterError.hidden = true;
    renderFilter();
    updateRfPower(null);
    for (const option of Object.values(radioOptions)) {
      updateRadioOption(option, null);
    }
    disableAudio();
    for (const address of addresses) {
      address.input.disabled = true;
      address.pending = null;
    }
    setMain(0);
    resetPlots();
    setStatus(lastProblem || 'Verbindung getrennt · neuer Versuch …', 'error');
    if (!reconnectTimer) {
      reconnectTimer = setTimeout(connect, reconnectDelay);
      reconnectDelay = Math.min(4000, reconnectDelay * 1.5);
    }
  };
}

bandSelect.addEventListener('change', () => selectBand(bandSelect.value));
modeSelect.addEventListener('change', () => {
  if (activeBand) {
    send({op: 'sideband', band: activeBand.id, mode: modeSelect.value});
    renderFilter();
  }
});
filterLower.addEventListener('input', () => changeFilter({low_hz: Number(filterLower.value)}));
filterUpper.addEventListener('input', () => changeFilter({high_hz: Number(filterUpper.value)}));
filterNoise.addEventListener('input', () => changeFilter({nr_percent: Number(filterNoise.value)}));
notchModes.addEventListener('change', event => changeFilter({notch_mode: event.target.value}, 0));
notchFrequency.addEventListener('input', () => changeFilter({notch_hz: Number(notchFrequency.value)}));
notchFrequency.addEventListener('change', () => changeFilter({notch_hz: Number(notchFrequency.value)}, 0));
for (const button of filterPresets) {
  button.addEventListener('click', () => {
    const bandwidth = Number(button.dataset.bandwidth);
    const low = Math.min(Number(filterLower.value), 4000 - bandwidth);
    changeFilter({low_hz: low, high_hz: low + bandwidth}, 0);
  });
}
volume.addEventListener('input', () => {
  const percent = Number(volume.value);
  setVolume(percent);
  send({op: 'volume', percent});
});
for (const [op, option] of Object.entries(radioOptions)) {
  option.select.addEventListener('change', () => {
    if (option.select.disabled || !send({op, value: Number(option.select.value)})) {
      renderRadioOptions();
    }
  });
}
rfPower.addEventListener('input', () => {
  editingPower = true;
  rfPowerValue.value = `${rfPower.value} %`;
});
rfPower.addEventListener('change', () => {
  editingPower = false;
  if (rfPower.disabled || !send({op: 'rf_power', percent: Number(rfPower.value)})) {
    renderRfPower();
  }
});
rfPower.addEventListener('pointercancel', () => {
  editingPower = false;
  renderRfPower();
});
lowerControl.addEventListener('input', () => {
  saveBandThresholds(Number(lowerControl.value), Number(upperControl.value));
});
upperControl.addEventListener('input', () => {
  const upper = Number(upperControl.value);
  saveBandThresholds(Math.min(Number(lowerControl.value), upper - 1), upper);
});
colorControl.value = color;
colorControl.addEventListener('change', () => {
  color = colorControl.value;
  preferences.color = color;
  updateThresholds(Number(lowerControl.value), Number(upperControl.value));
  savePreferences();
});
audioButton.addEventListener('click', toggleAudio);
pttModeControl.value = pttMode;
pttModeControl.addEventListener('change', () => {
  stopPtt();
  pttMode = pttModeControl.value;
  preferences.pttMode = pttMode;
  savePreferences();
  renderTx();
});
pttButton.addEventListener('pointerdown', event => {
  if (event.button === 0 && pttMode === 'hold') {
    event.preventDefault();
    pttButton.setPointerCapture(event.pointerId);
    pressPtt(event.pointerId);
  }
});
for (const eventName of ['pointerup', 'lostpointercapture']) {
  pttButton.addEventListener(eventName, event => releasePtt(event.pointerId));
}
pttButton.addEventListener('pointercancel', stopPtt);
pttButton.addEventListener('contextmenu', event => event.preventDefault());
pttButton.addEventListener('click', () => {
  if (pttMode === 'toggle') {
    pressPtt('click');
    releasePtt('click');
  }
});

function pttKey(event) {
  if (event.code === 'Space' || event.key === ' ') {
    return 'Space';
  }
  return event.key === 'Enter' ? 'Enter' : null;
}

window.addEventListener('keydown', event => {
  const key = pttKey(event);
  const target = event.target;
  if (!key || (key === 'Enter' && target !== pttButton) || pttButton.disabled ||
      event.defaultPrevented || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey ||
      target.isContentEditable || target.closest?.('input, textarea, select, summary') ||
      (!pttId && target.closest?.('.filter-presets button'))) {
    return;
  }
  event.preventDefault();
  if (!event.repeat) {
    pressPtt(key);
  }
});
window.addEventListener('keyup', event => {
  const key = pttKey(event);
  if (key && releasePtt(key)) {
    event.preventDefault();
  }
});
window.addEventListener('blur', stopPtt);
window.addEventListener('pagehide', disableAudio);
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    stopPtt();
  }
});
setInterval(() => {
  if (pttId) {
    send({op: 'ptt', action: 'keepalive', id: pttId});
  }
}, 250);
allowAudioDevices.addEventListener('click', () => grantBrowserDevices());
allowMicrophones.addEventListener('click', () => grantBrowserDevices(true));
browserSpeaker.addEventListener('change', () => selectBrowserSpeaker(browserSpeaker.value));
browserMicrophone.addEventListener('change', selectMicrophone);
mediaDevices?.addEventListener('devicechange', refreshBrowserDevices);
setup.addEventListener('toggle', refreshBrowserDevices);
for (const address of addresses) {
  address.input.addEventListener('input', () => showAddressError(address, ''));
  address.input.addEventListener('change', () => commitAddress(address));
  address.input.addEventListener('keydown', event => {
    if (event.key === 'Enter') {
      event.preventDefault();
      commitAddress(address);
    }
  });
}
const openDesktopMenus = !matchMedia('(max-width: 730px)').matches;
element('receive-menu').open = openDesktopMenus;
element('filter-menu').open = openDesktopMenus;

function applyLanguage(next) {
  setLanguage(next);
  languageSelect.value = language;
  txMeterFormat = new Intl.NumberFormat(locale, {minimumFractionDigits: 1, maximumFractionDigits: 1});
  frequencyFormat = new Intl.NumberFormat(locale, {
    minimumFractionDigits: 6, maximumFractionDigits: 6, useGrouping: false,
  });
  rangeFormat = new Intl.NumberFormat(locale, {maximumFractionDigits: 5, useGrouping: false});
  allowAudioDevices.textContent = translate(audioDevicePicker ? 'Wiedergabegerät freigeben …' : 'Soundkarten freigeben …');
  element('zoom-range').textContent = `MAIN ±${rangeFormat.format(halfSpan / 1000)} kHz`;
  for (const button of filterPresets) button.textContent = `${rangeFormat.format(button.dataset.bandwidth / 1000)} k`;
  renderBandRange();
  setMain(mainHz, mainConfirmed);
  renderFilter();
  renderTxMeters(txMeterReadings);
  renderBrowserDevices();
  renderTx();
  ft.languageChanged();
  overview.languageChanged();
  zoom.languageChanged();
}

languageSelect.addEventListener('change', () => {
  applyLanguage(languageSelect.value);
  preferences.language = language;
  savePreferences();
});
applyLanguage(preferences.language);
restoreBandThresholds(preferredBand);
connect();
