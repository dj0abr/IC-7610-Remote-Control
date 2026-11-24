import {BrowserAudio} from '/audio.js';
import {PlotPair} from '/plots.js';

const element = id => document.getElementById(id);
const bandSelect = element('band');
const modeSelect = element('sideband');
const volume = element('volume');
const setup = element('setup');
const addresses = ['radio-ip', 'pi-ip'].map(id => ({
  key: id.replace('-', '_'), input: element(id), error: element(`${id}-error`), pending: null,
}));
const status = element('status');
const audioButton = element('audio-toggle');
const audioError = element('audio-error');
const browserSpeaker = element('browser-speaker');
const allowAudioDevices = element('allow-audio-devices');
const mediaDevices = navigator.mediaDevices;
const audioDevicePicker = typeof mediaDevices?.selectAudioOutput === 'function';
const lowerControl = element('lower');
const upperControl = element('upper');
const colorControl = element('color');
const supportedColors = ['blue', 'red', 'green'];
const frequencyFormat = new Intl.NumberFormat('de-DE', {
  minimumFractionDigits: 6, maximumFractionDigits: 6, useGrouping: false,
});
const rangeFormat = new Intl.NumberFormat('de-DE', {maximumFractionDigits: 5, useGrouping: false});
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
let halfSpan = 30000;
let spectrumLive = false;
let setupState = null;
let nextSetupId = 0;
let lastProblem = '';
let audioEnabled = false;
let audioStarting = false;
let audioRouting = Promise.resolve();
let browserDevices = [];
let deviceRefresh = 0;
let requestingDevices = false;

const audio = new BrowserAudio();
const overview = new PlotPair(element('overview'), false, tuneMain, stepMain);
const zoom = new PlotPair(element('zoom'), true, tuneMain, stepMain);

function savePreferences() {
  try {
    localStorage.setItem(preferenceKey, JSON.stringify(preferences));
  } catch {
    // A browser can block persistent storage.
  }
}

function setStatus(text, phase = '') {
  status.value = text;
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
  if (spectrumLive) {
    send({op: 'set_main', hz});
  }
}

function stepMain(steps) {
  if (spectrumLive) {
    send({op: 'step_main', steps});
  }
}

function setMain(hz) {
  mainHz = Number.isSafeInteger(hz) && hz > 0 ? hz : 0;
  element('main-frequency').value = mainHz ?
    frequencyFormat.format(mainHz / 1e6).replace(/(\d{3})(\d{3})$/, '$1\u202f$2') : '—';
  overview.setMain(mainHz, halfSpan);
  zoom.setMain(mainHz, halfSpan);
}

function resetPlots() {
  overview.clear();
  zoom.clear();
  spectrumLive = false;
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

function showAudioError(text) {
  audioError.textContent = text;
  audioError.hidden = !text;
}

function renderAudioButton() {
  audioButton.disabled = audioStarting || !setupState;
  audioButton.textContent = audioStarting ? 'Audio startet …' : audioEnabled ? 'Audio ausschalten' : 'Audio einschalten';
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
  audio.stop();
  send({op: 'audio_output', output: 'off'});
  renderAudioButton();
}

async function toggleAudio() {
  showAudioError('');
  if (audioEnabled) {
    disableAudio();
    return;
  }
  audioStarting = true;
  renderAudioButton();
  try {
    await audio.start();
    await routeBrowserAudio();
    if (!send({op: 'audio_output', output: 'browser'})) {
      throw new Error('Backend nicht verbunden.');
    }
    audioEnabled = true;
  } catch (error) {
    audio.stop();
    showAudioError(error.message || 'Audio konnte nicht gestartet werden.');
  } finally {
    audioStarting = false;
    renderAudioButton();
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
  overview.setBand(band);
  zoom.clear();
  restoreBandThresholds(id);
  spectrumLive = false;
  const low = rangeFormat.format(band.low_hz / 1e6);
  const high = rangeFormat.format(band.high_hz / 1e6);
  const center = rangeFormat.format(band.center_hz / 1e6);
  element('band-range').value = `${low}–${high} MHz`;
  element('sub-frequency').value = `${center} MHz`;
  element('fullband-range').textContent = `${(band.display_high_hz - band.display_low_hz) / 1000} kHz`;
  audio.reset();
  setStatus('Band wird eingestellt …', 'connecting');
  send({op: 'band', id});
}

function installBands(message) {
  if (message.version !== 1 || !Array.isArray(message.bands) || message.bands.length === 0) {
    setStatus('Unbekannte Protokollversion', 'error');
    return;
  }
  bands = message.bands;
  halfSpan = message.zoom_half_span_hz || 30000;
  element('zoom-range').textContent = `MAIN ±${halfSpan / 1000} kHz`;
  zoom.setMain(mainHz, halfSpan);
  bandSelect.replaceChildren(...bands.map(band => new Option(band.label, band.id)));
  bandSelect.disabled = false;
  reconnectDelay = 1000;
  selectBand(bands.some(band => band.id === preferredBand) ? preferredBand : bands[0].id);
  lowerControl.disabled = upperControl.disabled = false;
}

function renderBrowserDevices() {
  const selected = preferredBrowserSpeaker();
  browserSpeaker.replaceChildren(new Option('Systemstandard', ''));
  for (const device of browserDevices) {
    browserSpeaker.add(new Option(device.label, device.id));
  }
  if (selected && !browserDevices.some(device => device.id === selected)) {
    browserSpeaker.add(new Option('Gespeichertes Gerät (nicht verfügbar)', selected));
  }
  browserSpeaker.value = selected;
  browserSpeaker.disabled = !audio.canSelectOutput;
  element('browser-device-access').hidden = !audio.canSelectOutput ||
    (!audioDevicePicker && (browserDevices.length > 0 || typeof mediaDevices?.getUserMedia !== 'function'));
  element('audio-device-hint').hidden = audioDevicePicker;
}

function showBrowserDeviceError(text) {
  const error = element('browser-device-error');
  error.textContent = text;
  error.hidden = !text;
}

async function refreshBrowserDevices() {
  if (!setup.open || requestingDevices) {
    return;
  }
  const refresh = ++deviceRefresh;
  try {
    const devices = await audio.outputDevices();
    if (refresh === deviceRefresh) {
      browserDevices = devices;
      renderBrowserDevices();
      showBrowserDeviceError('');
    }
  } catch {
    if (refresh === deviceRefresh) {
      showBrowserDeviceError('Die Wiedergabegeräte konnten nicht geladen werden.');
    }
  }
}

async function grantBrowserDevices() {
  ++deviceRefresh;
  requestingDevices = allowAudioDevices.disabled = true;
  showBrowserDeviceError('');
  try {
    const {devices, selected} = await audio.requestOutputAccess();
    browserDevices = devices;
    if (selected !== undefined) {
      selectBrowserSpeaker(selected);
    }
    renderBrowserDevices();
  } catch {
    showBrowserDeviceError('Der Browser hat keine weiteren Wiedergabegeräte freigegeben.');
  } finally {
    requestingDevices = allowAudioDevices.disabled = false;
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
  if (audioEnabled) {
    send({op: 'audio_output', output: 'browser'});
  }
  renderAudioButton();
}

function showAddressError(address, text, invalid = false) {
  address.error.textContent = text;
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
  if (message.phase !== 'live' && message.phase !== 'idle') {
    resetPlots();
    audio.reset();
  }
}

function control(message) {
  switch (message.type) {
    case 'bands':
      installBands(message);
      break;
    case 'state':
      handleState(message);
      break;
    case 'setup':
      installSetup(message);
      break;
    case 'sideband':
      if (message.band === activeBand?.id && ['USB', 'LSB'].includes(message.mode)) {
        modeSelect.value = message.mode;
        modeSelect.disabled = false;
      }
      break;
    case 'setup-result':
      setupResult(message);
      break;
    case 'audio-reset':
      audio.reset();
      break;
    case 'main':
      setMain(message.hz);
      break;
    case 'main-status':
      element('main-frequency').title = message.text;
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
    spectrumLive = true;
  } else {
    zoom.addFrame(levels, center);
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
        console.error('Ungültige Backend-Nachricht', error);
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
    volume.disabled = true;
    activeBand = null;
    setupState = null;
    for (const address of addresses) {
      address.input.disabled = true;
      address.pending = null;
    }
    audio.reset();
    renderAudioButton();
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
  }
});
volume.addEventListener('input', () => {
  const percent = Number(volume.value);
  setVolume(percent);
  send({op: 'volume', percent});
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
allowAudioDevices.textContent = audioDevicePicker ? 'Wiedergabegerät freigeben …' : 'Soundkarten freigeben …';
allowAudioDevices.addEventListener('click', grantBrowserDevices);
browserSpeaker.addEventListener('change', () => selectBrowserSpeaker(browserSpeaker.value));
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
element('receive-menu').open = !matchMedia('(max-width: 730px)').matches;

renderBrowserDevices();
restoreBandThresholds(preferredBand);
connect();
