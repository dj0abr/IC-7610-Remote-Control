import {canStartFtAuto, ftCall, ftGrid, ftPeriod, ftReply, nextFtAutoStep, parseFtContact, splitFtMessage} from '/ft-qso.js';
import {countryFlagPosition, dxccForCall} from '/dxcc.js';
import {waterfallColors, minimumLevel} from '/plots.js';

const $ = id => document.getElementById(id);
// Contact progression and layout adapted from the FT4/FT8 view in QO100_TRX2.
export function createFtPanel({send, preferences, savePreferences}) {
  let radio = {connected: false, dial: 0};
  let available = false, active = false, logAvailable = false, clockOffset = 0;
  let mode = preferences.ftMode === 'FT4' ? 'FT4' : 'FT8';
  let transmission = {phase:'idle'};
  let ft8Pending = false, ftModePending = false, ft8Error = '';
  let ftQso = null, ftQsoTransmission = null, ftAutoQso = null, ftQsoLog = null, ftLogError = false;
  const ftAutoMaxAttempts = 3, ftAutoReplySlots = 2;
  const requests = new Map(), decoded = new Set();
  let revision = 0;
  const ft8Log = {output:$('ft8-log'), lines:[], follow:true};
  const ftOwnLog = {output:$('ft8-own-log'), lines:[], follow:true};
  const ownTraffic = [];
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
  const isOpen = () => radio.connected;
  const ft8Visible = () => active;
  const ftBusy = () => ft8Pending || transmission.phase !== 'idle';
  const canTune = () => isOpen() && radio.live && !radio.paused && !radio.voiceBusy;
  const canTransmit = () => active && available && canTune() && radio.txReady && radio.confirmed &&
    radio.dial >= radio.band?.low_hz && radio.dial < radio.band?.high_hz && radio.band?.id !== 'cb';
  const readPreference = key => preferences[key];
  function writePreference(key, value) {
    preferences[key] = value;
    savePreferences();
  }

  function checked(op, values) {
    return new Promise((resolve, reject) => {
      const id = ++revision;
      const timeout = setTimeout(() => {
        requests.delete(id);
        reject(Object.assign(new Error('Keine Antwort vom Backend.'), {code:'timeout'}));
      }, 3000);
      requests.set(id, {resolve, reject, timeout});
      if (!send({op, ...values, request_id:id})) {
        clearTimeout(timeout);
        requests.delete(id);
        reject(new Error('Backend nicht verbunden.'));
      }
    });
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
  function clearFt8() {
    decoded.clear();
    ownTraffic.length = 0;
    clearFtLog(ft8Log);
    clearFtLog(ftOwnLog);
  }
  function appendFtLine(log, entry) {
    const {output, lines} = log;
    lines.push(entry);
    output.append(entry.line);
    if (lines.length > 50) lines.shift().line.remove();
    if (log.follow) scheduleFtLogScroll(log);
  }
  function appendFtOwnLine(entry, own) {
    if (!entry.sent && (!own || !entry.text.toUpperCase().split(/[^A-Z0-9]+/).includes(own))) return;
    const line = entry.line.cloneNode(true);
    line.onclick = entry.line.onclick;
    line.removeAttribute('data-new-cq');
    line.removeAttribute('data-cq-call');
    appendFtLine(ftOwnLog, {...entry, line});
  }
  function appendOwnTraffic(entry, own) {
    ownTraffic.push(entry);
    if (ownTraffic.length > 100) ownTraffic.shift();
    appendFtOwnLine(entry, own);
  }
  function appendFtSent({result, message, slot_utc: slot, audio_hz: audioHz, dial_hz: dial}) {
    if (!active || result !== 'complete' || dial !== radio.dial || !message ||
        !Number.isFinite(slot) || !Number.isFinite(audioHz)) return;
    const date = new Date(slot * 1000);
    if (!Number.isFinite(date.getTime())) return;
    const utc = date.toISOString().slice(11, 19).replace(/:/g, '') + (slot % 1 ? '.5' : '');
    const line = document.createElement('div');
    line.className = 'ft-decode-line ft-sent-line';
    line.textContent = `${utc} TX  ${String(audioHz).padStart(4)} Hz  ${message}`;
    appendOwnTraffic({line, text: message, sent: true});
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
  function appendFt8(message) {
    const {utc, snr_db, dt_s, frequency_hz, message: text} = message;
    if (typeof utc !== 'string' || typeof text !== 'string' ||
        ![snr_db, dt_s, frequency_hz].every(Number.isFinite)) return;
    const signature = `${message.slot_utc}/${text}`;
    if (decoded.has(signature)) return;
    decoded.add(signature);
    if (decoded.size > 240) decoded.delete(decoded.values().next().value);
    const contact = parseFtContact(message);
    const own = ftCall($('ft-own-call').value);
    const line = document.createElement(contact ? 'button' : 'div');
    line.className = 'ft-decode-line';
    const metadata = `${utc} ${String(snr_db).padStart(3)} dB ${dt_s.toFixed(2).padStart(5)} s ${frequency_hz.toFixed(0).padStart(4)} Hz  `;
    ftDecodeFields(line, metadata, text, splitFtMessage(text, message.message_type));
    if (contact) {
      line.type = 'button';
      line.disabled = $('ft-cq').disabled || !canChooseFtContact(contact, own);
      line.onclick = () => {
        const own = ftCall($('ft-own-call').value);
        // For a third-party exchange, call the station that transmitted the selected line.
        const target = contact.recipient && contact.recipient !== own ? {...contact, kind:'cq'} : contact;
        if ($('ft-auto').checked && (target.kind === 'cq' || canStartFtAuto(target, own))) startFtAutoQso(target, true);
        else prepareFtReply(target, true);
      };
    }
    const entry = {line, contact, text};
    appendFtLine(ft8Log, entry);
    appendOwnTraffic(entry, own);
    if (typeof message.cq_call === 'string') {
      line.dataset.cqCall = message.cq_call;
      line.toggleAttribute('data-new-cq', message.worked_recently === false);
    }
    if (contact) {
      if (ftAutoQso) advanceFtAutoQso(contact);
      else if (isFtAutoReply(contact, own) || isFtAutoCqReply(contact, own, message)) startFtAutoQso(contact);
      else prepareFtReply(contact, false);
    }
  }
  function isFtAutoReply(contact, own) {
    if (!ftAutoReply.checked || contact.recipient !== own || !canStartFtAuto(contact, own)) return false;
    if (ftGrid(contact.kind)) return true;
    return ftQso?.step === 'cq' && ftQso.waiting &&
      contact.parity === ftQso.parity && contact.slot === ftQso.sentSlot + ftPeriod(mode);
  }
  function isFtAutoCqReply(contact, own, message) {
    return ftAutoCqReply.checked && !ftAutoCq.checked && contact.kind === 'cq' &&
      (!ftQso?.peer || ftQso.step === 'complete') && canStartFtAuto(contact, own) &&
      message.cq_call === contact.peer && message.worked_recently === false;
  }
  function canChooseFtContact(contact, own) {
    return contact.peer !== own && !(contact.recipient === own && contact.kind === '73');
  }
  function ftDraftInForm(draft) {
    return !!draft?.text && $('ft8-message').value === draft.text && Number($('ft8-parity').value) === draft.parity;
  }
  function applyFtDraft(draft) {
    $('ft8-message').value = draft.text;
    if (draft.text) $('ft8-parity').value = String(draft.parity);
  }
  function clearFtQso() {
    if (ftAutoQso) endFtAutoQso(true);
    if (ftDraftInForm(ftQso)) $('ft8-message').value = '';
    ftQso = null;
    ftQsoTransmission = null;
    ftQsoLog = null;
  }
  function ftIdentity(showError) {
    const own = ftCall($('ft-own-call').value);
    const locator = ftGrid($('ft-own-grid').value);
    if (own && locator) return {own, locator};
    if (showError) {
      ft8Error = own ? 'QTH-Locator mit vier oder sechs Zeichen eingeben.' :
        'Eigenes Standardrufzeichen eingeben.';
      renderFt8Controls();
      $(own ? 'ft-own-grid' : 'ft-own-call').focus();
    }
    return null;
  }
  function setFtDraft(draft, selected) {
    if (ftQsoLog && (draft.peer !== ftQsoLog.call || (selected && draft.step === 'locator')))
      ftQsoLog = null;
    const use = selected || ftDraftInForm(ftQso) || !$('ft8-message').value;
    ftQso = draft;
    if (use) applyFtDraft(draft);
    ft8Error = '';
    renderFt8Controls();
  }
  function prepareFtReply(contact, selected) {
    if (!selected && (!isOpen() || !ftQso?.peer || ftQso.step === 'complete' || contact.peer !== ftQso.peer || contact.kind === 'cq' ||
        contact.slot <= Math.max(ftQso.slot, ftQso.sentSlot ?? 0, ftQsoTransmission?.slot ?? 0) ||
        contact.parity !== ftQso.parity)) return;
    const identity = ftIdentity(selected);
    if (!identity) return;
    const draft = ftReply(contact, identity.own, identity.locator);
    if (!draft) return;
    if (!selected && draft.step === 'complete' && ftQsoLog?.acknowledged)
      saveFtQsoLog(contact.slot);
    setFtDraft(draft, selected);
  }
  function startFtAutoQso(contact, selected = false) {
    if ($('ft8-send').disabled || ftAutoQso) return;
    const identity = ftIdentity(true);
    if (!identity) return;
    const draft = ftReply(contact, identity.own, identity.locator);
    const period = ftPeriod(mode);
    if (!draft || !period || (!selected && (Date.now() + clockOffset) / 1000 - contact.slot > 3 * period)) return;
    ftAutoQso = {own: identity.own, period, attempts: 1, timer: null};
    setFtDraft(draft, true);
    transmitFtDraft(ftAutoQso);
  }
  function endFtAutoQso(cancelTx) {
    clearTimeout(ftAutoQso.timer);
    ftAutoQso = null;
    if (cancelTx && (ft8Pending || transmission?.phase === 'scheduled' || transmission?.phase === 'sending'))
      send({op:'ft8_tx_stop'});
  }
  function advanceFtAutoQso(contact) {
    if (!ftQso?.waiting || contact.peer !== ftQso.peer || contact.recipient !== ftAutoQso.own ||
        contact.slot <= Math.max(ftQso.slot, ftQso.sentSlot) || contact.parity !== ftQso.parity) return;
    const draft = ftReply(contact, ftAutoQso.own, '');
    if (!draft || !nextFtAutoStep(ftQso.step, draft.step)) return;
    if (draft.step === 'complete') {
      if (ftQsoLog?.acknowledged) saveFtQsoLog(contact.slot);
      endFtAutoQso(false);
      setFtDraft(draft, true);
      return;
    }
    const sameStep = draft.step === ftQso.step;
    if (sameStep && ftAutoQso.attempts >= ftAutoMaxAttempts) return;
    clearTimeout(ftAutoQso.timer);
    sendFtAutoStep(draft, sameStep ? ftAutoQso.attempts + 1 : 1);
  }
  function sendFtAutoStep(draft, attempts) {
    if (!canSendFtDraft()) {
      stopFtAutoQso('TX ist belegt oder nicht bereit.');
      return;
    }
    ftAutoQso.attempts = attempts;
    draft.waiting = false;
    if (draft !== ftQso) setFtDraft(draft, true);
    transmitFtDraft(ftAutoQso);
  }
  function stopFtAutoQso(message, cancelTx = false) {
    endFtAutoQso(cancelTx);
    clearFtQso();
    ft8Error = message;
    renderFt8Controls();
  }
  function trackFtQsoTransmission({draft, slot, values, identity, rxHz}) {
    if (!draft.peer || !identity) return;
    if (!ftQsoLog) {
      ftQsoLog = {
        call: draft.peer, own_call: identity.own, own_grid: identity.locator, mode: values.mode,
        start: Math.floor(slot), tx_hz: values.frequency_hz + values.audio_hz,
        rx_hz: rxHz + Math.round(draft.audioHz),
      };
    }
    const {received} = draft;
    if (received.locator || (/^[A-R]{2}\d{2}$/.test(received.kind) && received.kind !== 'RR73'))
      ftQsoLog.grid = received.locator ?? received.kind;
    if (/^R?[+-]\d{2}$/.test(received.kind))
      ftQsoLog.received_report = received.kind.replace(/^R/, '');
    if (draft.step === 'report' || draft.step === 'ackReport')
      ftQsoLog.sent_report = values.message.split(/\s+/).pop().replace(/^R/, '');
    if (draft.step === 'ack' || draft.step === 'signoff') saveFtQsoLog(slot);
    if (draft.step === 'ackReport') ftQsoLog.acknowledged = true;
  }
  async function saveFtQsoLog(slot) {
    const record = ftQsoLog;
    if (!record || record.saving || record.saved || !record.sent_report || !record.received_report) return;
    record.saving = true;
    try {
      const {call, own_call, grid, own_grid, mode, start, tx_hz, rx_hz, sent_report, received_report} = record;
      await checked('ft_qso_log', {call, own_call, grid, own_grid, mode, start, tx_hz, rx_hz, sent_report, received_report,
        end: Math.floor(slot)});
      record.saved = true;
      ftLogError = false;
    } catch (error) {
      ftLogError = error.code !== 'ft_log_disabled';
    } finally {
      record.saving = false;
      renderFt8Controls();
    }
  }
  function updateFtQsoTransmission(tx) {
    const sending = ftQsoTransmission;
    if (!sending || !tx) return;
    if (tx.phase !== 'idle') {
      if (tx.slot_utc === sending.slot) sending.observed = true;
      return;
    }
    if (!sending.observed) return;
    ftQsoTransmission = null;
    if (sending.draft !== ftQso) return;
    if (tx.result !== 'complete') {
      if (ftAutoQso) {
        endFtAutoQso(false);
        clearFtQso();
      }
      return;
    }
    ftQso.sentSlot = sending.slot;
    trackFtQsoTransmission(sending);
    if (ftQso.step === 'signoff') {
      ftQso.step = 'complete';
      if (ftDraftInForm(ftQso)) $('ft8-message').value = '';
      ftQso.text = '';
      if (ftAutoQso) endFtAutoQso(false);
    } else {
      ftQso.waiting = true;
      if (ftAutoQso) {
        const automatic = ftAutoQso;
        automatic.timer = setTimeout(() => {
          if (ftAutoQso !== automatic) return;
          if (automatic.attempts >= ftAutoMaxAttempts) {
            stopFtAutoQso('Auto-QSO beendet: keine passende Antwort nach drei Sendeversuchen.');
            return;
          }
          sendFtAutoStep(ftQso, automatic.attempts + 1);
        }, ftAutoReplySlots * automatic.period * 1000);
      }
    }
  }
  function renderFtQsoControls(enabled) {
    const manual = enabled && !ftAutoQso;
    for (const id of ['ft-cq', 'ft-apply-reply', 'ft-end-qso', 'ft-auto-reply']) $(id).disabled = !manual;
    for (const id of ['ft-own-call', 'ft-own-grid', 'ft-auto']) $(id).disabled = ftBusy() || !!ftAutoQso;
    const own = ftCall($('ft-own-call').value);
    for (const {lines} of [ft8Log, ftOwnLog]) {
      for (const {line, contact} of lines) {
        if (!contact) continue;
        const disabled = !manual || !canChooseFtContact(contact, own);
        if (line.disabled !== disabled) line.disabled = disabled;
      }
    }
    $('ft-contact').hidden = !ftQso;
    if (!ftQso) return;
    const {peer, audioHz, step, text, waiting} = ftQso;
    const ready = !!text && !ftDraftInForm(ftQso);
    $('ft-apply-reply').hidden = !ready;
    $('ft-end-qso').hidden = !!ftAutoQso;
    const name = peer ? `${peer} · RX ${Math.round(audioHz)} Hz` : 'CQ';
    const status = step === 'complete' ? 'QSO abgeschlossen' :
      waiting ? (peer || ftAutoReply.checked ? 'Warte auf Antwort' : 'Antwort anklicken') :
      ready ? text : 'Sendung vorbereitet';
    const output = $('ft-qso-status');
    const label = `${ftAutoQso ? 'Auto · ' : ''}${name} · ${status}`;
    if (output.textContent !== label) output.textContent = label;
  }
  for (const [id, preference] of [['ft-own-call', 'ftOwnCall'], ['ft-own-grid', 'ftOwnGrid']]) {
    const input = $(id);
    input.value = readPreference(preference) ?? '';
    input.oninput = () => {
      clearFtQso();
      ft8Error = '';
      writePreference(preference, input.value.trim().toUpperCase());
      if (id === 'ft-own-call') {
        clearFtLog(ftOwnLog);
        const own = ftCall(input.value);
        for (const entry of ownTraffic) appendFtOwnLine(entry, own);
      }
      renderFt8Controls();
    };
  }
  const ftAutoReply = $('ft-auto-reply');
  ftAutoReply.checked = false;
  ftAutoReply.onchange = () => {
    renderFt8Controls();
  };
  const ftAutoCq = $('ft-auto-cq');
  const ftAutoCqReply = $('ft-auto-cq-reply');
  let ftAutoCqRetryAt = 0;
  ftAutoCq.checked = ftAutoCqReply.checked = false;
  ftAutoCq.onchange = () => {
    if (ftAutoCq.checked) {
      if (ftIdentity(true)) {
        ftAutoCqReply.checked = false;
        ftAutoCqRetryAt = 0;
        sendIdleFtCq();
      } else ftAutoCq.checked = false;
    } else stopScheduledFtCq();
    renderFt8Controls();
  };
  ftAutoCqReply.onchange = () => {
    if (ftAutoCqReply.checked) {
      if (!ftIdentity(true)) ftAutoCqReply.checked = false;
      else if (ftAutoCq.checked) {
        ftAutoCq.checked = false;
        stopScheduledFtCq();
      }
    }
    renderFt8Controls();
  };
  function stopScheduledFtCq() {
    if (ftQso?.autoCq && (ft8Pending || transmission?.phase === 'scheduled')) {
      send({op:'ft8_tx_stop'});
      clearFtQso();
    }
  }
  function prepareFtCq(identity, autoCq = false) {
    setFtDraft({step:'cq', autoCq, text:`CQ ${identity.own} ${identity.locator}`,
      parity:Number($('ft8-parity').value)}, true);
  }
  $('ft-cq').onclick = () => {
    const identity = ftIdentity(true);
    if (!identity) return;
    prepareFtCq(identity);
    $('ft8-tx-form').requestSubmit($('ft8-send'));
  };
  function sendIdleFtCq() {
    if (!ftAutoCq.checked || !active || ftAutoQso || !canSendFtDraft()) return;
    if (ftQso?.peer && ftQso.step !== 'complete') return;
    const period = ftPeriod(mode);
    const now = (Date.now() + clockOffset) / 1000;
    if (now < ftAutoCqRetryAt ||
        (ftQso?.step === 'cq' && ftQso.waiting && now < ftQso.sentSlot + 2 * period + 1)) return;
    const identity = ftIdentity(false);
    if (!identity || !$('ft8-audio-hz').validity.valid || !$('ft8-level').validity.valid) return;
    prepareFtCq(identity, true);
    transmitFtDraft();
  }
  $('ft-apply-reply').onclick = () => {
    if (ftQso) applyFtDraft(ftQso);
    renderFt8Controls();
  };
  $('ft-end-qso').onclick = () => {
    clearFtQso();
    ft8Error = '';
    renderFt8Controls();
  };
  function canSendFtDraft() {
    return canTransmit() && !ft8Pending && !ftModePending && transmission.phase === 'idle';
  }
  function renderFtSlotClock() {
    if (!ft8Visible()) return;
    const now = new Date(Date.now() + clockOffset);
    const clock = $('ft-utc-clock');
    const utc = now.toISOString();
    const label = utc.slice(11, 19) + ' UTC';
    if (clock.textContent !== label) {
      clock.textContent = label;
      clock.dateTime = utc.slice(11, 19) + 'Z';
    }
    const period = ftPeriod(mode ?? 'FT8');
    const progress = $('ft-slot-progress');
    progress.max = period;
    progress.value = (now.getTime() % (period * 1000)) / 1000;
  }
  function renderFt8Controls() {
    renderFtSlotClock();
    const tx = transmission;
    const active = !!tx && tx.phase !== 'idle';
    const writable = canTransmit();
    const busy = ft8Pending || ftModePending || active;
    const modeSelect = $('ft-mode');
    if (!ftModePending) modeSelect.value = mode;
    modeSelect.disabled = !isOpen() || radio.paused || active || ft8Pending || ftModePending || !!ftAutoQso;
    const ft4 = mode === 'FT4';
    const parity = $('ft8-parity');
    parity.options[0].textContent = ft4 ? ':00 / :15 / :30 / :45' : ':00 / :30';
    parity.options[1].textContent = ft4 ? ':07.5 / :22.5 / :37.5 / :52.5' : ':15 / :45';
    $('ft8-audio-hz').max = ft4 ? 3930 : 3950;
    $('ft8-send').disabled = !canSendFtDraft() || !!ftAutoQso;
    ftAutoCq.disabled = !writable && !ftAutoCq.checked;
    ftAutoCqReply.disabled = !logAvailable || (!writable && !ftAutoCqReply.checked);
    $('ft8-stop').disabled = !isOpen() ||
      (!ftAutoCq.checked && !ftAutoCqReply.checked && !ftAutoQso && (!active || ft8Pending));
    renderFtQsoControls(!$('ft8-send').disabled);
    for (const id of ['ft8-message', 'ft8-audio-hz', 'ft8-level', 'ft8-parity']) $(id).disabled = !writable || busy || !!ftAutoQso;
    renderFt8Status(tx);
  }
  function renderFt8Status(tx) {
    const time = tx?.slot_utc ?
      new Date(tx.slot_utc * 1000).toISOString().slice(11, Number.isInteger(tx.slot_utc) ? 19 : 21) + ' UTC' : '';
    const result = {
      complete: 'Sendung abgeschlossen',
      stopped: 'Sendung abgebrochen',
      missed_slot: 'Sendeslot verpasst',
      clock_changed: 'Systemzeit hat sich geändert',
      retuned: 'Frequenz geändert',
      unavailable: 'TX nicht verfügbar',
      underrun: 'TX-Audiopuffer unterbrochen',
    };
    const status = $('ft8-status');
    const logError = ftLogError && 'QSO konnte nicht im ADIF-Logbuch gespeichert werden.';
    status.classList.toggle('error', !!(ft8Error || logError));
    status.classList.toggle('sending', tx?.phase === 'sending' && !ft8Error && !logError);
    const seconds = Math.max(0, Math.ceil(tx?.slot_utc - (Date.now() + clockOffset) / 1000));
    const text = ft8Error || logError || (tx?.phase === 'scheduled' && time ?
      `${'Geplant für'} ${time} · ${`noch ${seconds} s`}` :
      tx?.phase === 'sending' ? `${'Sendet'} · ${time}` :
      tx?.text || result[tx?.result] || '');
    if (status.textContent !== text) status.textContent = text;
  }
  $('ft-mode').onchange = event => setMode(event.target.value);
  $('ft8-tx-form').onsubmit = event => {
    event.preventDefault();
    if (!$('ft8-send').disabled) transmitFtDraft();
  };
  async function transmitFtDraft(automatic = null) {
    const values = {
      frequency_hz: radio.dial,
      message: $('ft8-message').value,
      mode: mode ?? 'FT8',
      audio_hz: Number($('ft8-audio-hz').value),
      level: Number($('ft8-level').value),
      parity: Number($('ft8-parity').value),
    };
    if (!Number.isInteger(values.audio_hz) || values.audio_hz < 100 ||
        values.audio_hz > Number($('ft8-audio-hz').max) ||
        !Number.isInteger(values.level) || values.level < 1 || values.level > 100) {
      const message = 'Audiofrequenz und TX-Pegel prüfen.';
      if (automatic) stopFtAutoQso(message);
      else { ft8Error = message; renderFt8Controls(); }
      return;
    }
    ft8Error = '';
    ft8Pending = true;
    renderFt8Controls();
    const draft = ftDraftInForm(ftQso) ? ftQso : null;
    const identity = draft?.peer ? ftIdentity(false) : null;
    if (identity) identity.locator = $('ft-own-grid').value.trim().toUpperCase();
    const rxHz = radio.dial;
    try {
      const answer = await checked('ft8_tx_start', values);
      if (draft && draft === ftQso && Number.isFinite(answer.slot_utc))
        ftQsoTransmission = {draft, slot:answer.slot_utc, values, identity, rxHz,
          observed:transmission?.slot_utc === answer.slot_utc};
    }
    catch (error) {
      if (automatic && automatic !== ftAutoQso) return;
      if (draft?.autoCq && draft === ftQso) {
        if (error.code === 'tx2_busy') ftAutoCqRetryAt = (Date.now() + clockOffset) / 1000 + ftPeriod(values.mode);
        else ftAutoCq.checked = false;
      }
      if (automatic) {
        endFtAutoQso(error.code === 'timeout');
        clearFtQso();
      }
      ft8Error = ({
        ft8_free_text_length: 'Freitext ist auf 13 Zeichen inklusive Leerzeichen begrenzt. Für längere Nachrichten ein Standardformat verwenden.',
        ft8_invalid_message: 'Diese Nachricht lässt sich so nicht kodieren.',
      })[error.code] ?? error.message;
    }
    finally { ft8Pending = false; renderFt8Controls(); }
  }
  for (const [id, preference] of [['ft8-audio-hz', 'ftAudioHz'], ['ft8-level', 'ftTxLevel']]) {
    const input = $(id);
    const saved = Number(readPreference(preference));
    if (Number.isInteger(saved) && saved >= Number(input.min) && saved <= Number(input.max)) {
      input.value = saved;
    }
    input.addEventListener('input', () => {
      if (input.validity.valid) writePreference(preference, input.value);
    });
  }
  $('ft8-tx-form').addEventListener('input', () => {
    ft8Error = '';
    renderFt8Controls();
  });
  $('ft8-stop').onclick = () => {
    ftAutoCq.checked = ftAutoCqReply.checked = ftAutoReply.checked = false;
    if (ftAutoQso) stopFtAutoQso('Auto-QSO abgebrochen.', true);
    else {
      if (ft8Pending || (transmission?.phase ?? 'idle') !== 'idle') send({op:'ft8_tx_stop'});
      renderFt8Controls();
    }
  };


  function clearWaterfall() {
    context.clearRect(0, 0, canvas.width, canvas.height);
    waterfallAt = 0;
  }

  function resetAutomation() {
    ftAutoCq.checked = ftAutoCqReply.checked = ftAutoReply.checked = false;
    clearFtQso();
  }

  async function setMode(next) {
    if (ftModePending) return;
    ftModePending = true;
    ft8Error = '';
    renderView();
    try {
      await checked('ft_mode', {mode: next});
    } catch (error) {
      ft8Error = error.message;
    } finally {
      ftModePending = false;
      renderView();
    }
  }

  function renderView() {
    zoomChoice.checked = !active;
    ftChoice.checked = active;
    ftChoice.disabled = !available || !isOpen() || ftModePending || (!active && (radio.paused || radio.voiceBusy));
    zoomChoice.disabled = ftModePending;
    panel.hidden = !active;
    $('zoom').hidden = active;
    $('ft-clear').hidden = !active;
    $('ft8-frequency').textContent = radio.dial ?
      `MAIN ${(radio.dial / 1e6).toLocaleString('de-DE', {minimumFractionDigits:6})} MHz · USB` : 'MAIN';
    renderFt8Controls();
  }

  function update(next) {
    if (!next.connected && radio.connected) {
      available = active = false;
      transmission = {phase:'idle'};
      resetAutomation();
      for (const request of requests.values()) {
        clearTimeout(request.timeout);
        request.reject(new Error('Backend nicht verbunden.'));
      }
      requests.clear();
    }
    if (radio.dial !== next.dial || radio.band?.id !== next.band?.id || (radio.live && !next.live)) {
      resetAutomation();
      clearFt8();
      clearWaterfall();
    }
    radio = next;
    renderView();
  }

  function handle(message) {
    if (message.type === 'ft-reply') {
      const request = requests.get(message.request_id);
      if (request) {
        clearTimeout(request.timeout);
        requests.delete(message.request_id);
        if (message.ok) request.resolve(message);
        else request.reject(Object.assign(new Error(message.text || 'FT-Befehl fehlgeschlagen.'), {code:message.code}));
      }
    } else if (message.type === 'ft-state') {
      available = message.available;
      logAvailable = message.log_available;
      if (Number.isFinite(message.utc_ms)) clockOffset = message.utc_ms - Date.now();
      const next = ftPeriod(message.mode) > 0;
      if (active !== next || (next && mode !== message.mode)) {
        resetAutomation();
        clearFt8();
        clearWaterfall();
      }
      active = next;
      if (active) {
        mode = message.mode;
        writePreference('ftMode', mode);
      }
    } else if (message.type === 'ft-tx') {
      transmission = message;
      appendFtSent(message);
      updateFtQsoTransmission(message);
    } else if (message.type === 'ft-decode' && active && message.mode === mode && message.dial_hz === radio.dial) {
      appendFt8(message);
    } else if (message.type === 'ft-qso-logged') {
      for (const {line} of ft8Log.lines) {
        if (line.dataset.cqCall === message.call) line.removeAttribute('data-new-cq');
      }
    } else if (message.type === 'ft-qrz-status') {
      const status = $('ft-qrz-status');
      status.textContent = `${message.call} · ${message.text}`;
      status.classList.toggle('setting-error', message.state === 'error');
    } else if (message.type === 'ft-error' && active) {
      ft8Error = message.text;
    }
    renderView();
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
    if (target.name !== 'receiver-view') return;
    void setMode(target.value === 'ft' ? mode : 'off');
  });
  $('ft-clear').onclick = clearFt8;
  for (const section of [view, panel]) {
    section.addEventListener('keydown', event => {
      if (event.code === 'Space' || event.key === ' ') event.stopPropagation();
    });
  }
  setInterval(() => {
    if (!active) return;
    renderFtSlotClock();
    renderFt8Status(transmission);
    sendIdleFtCq();
  }, 200);

  return {
    update, handle, addFrame, clearWaterfall, active: ft8Visible, busy: ftBusy,
    setStyle(lower, upper, color) { palette = waterfallColors(color, lower, upper); }
  };
  }
