#pragma once

#include <Arduino.h>

static const char kAppJs[] PROGMEM = R"JS((() => {
  const img = document.getElementById('cam');
  const preview = document.querySelector('.preview');
  const status = document.getElementById('status');
  const ledBtn = document.getElementById('ledBtn');
  const fullscreenBtn = document.getElementById('fullscreenBtn');
  const exitFullscreenBtn = document.getElementById('exitFullscreenBtn');
  const frameSizeSel = document.getElementById('frameSizeSel');
  const applyCamBtn = document.getElementById('applyCamBtn');
  const langBtn = document.getElementById('langBtn');
  const toolbar = document.querySelector('.toolbar');
  const overlay = document.getElementById('overlay');
  const overlayMessage = document.getElementById('overlayMessage');
  const loader = document.getElementById('loader');
  const takeoverBtn = document.getElementById('takeoverBtn');
  const frameLabel = document.querySelector('.frameLabel');
  const frameQvgaOption = frameSizeSel ? frameSizeSel.querySelector('option[value="qvga"]') : null;
  const frameVgaOption = frameSizeSel ? frameSizeSel.querySelector('option[value="vga"]') : null;
  const frameSvgaOption = frameSizeSel ? frameSizeSel.querySelector('option[value="svga"]') : null;
  const ledBaseUrl = `${location.protocol}//${location.hostname}:81/`;
  const streamBaseUrl = `${location.protocol}//${location.hostname}:82/`;
  const viewerId = (globalThis.crypto && crypto.randomUUID)
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`;

  const defaultStrings = {
    pageTitle: 'ESP32-CAM',
    cameraAlt: 'Podglad z ESP32-CAM',
    buttonLedToggle: 'Wlacz/wylacz LED',
    buttonFullscreen: 'Pelny ekran',
    buttonExitFullscreen: 'Wyjdz z pelnego ekranu',
    buttonApplyImage: 'Zastosuj obraz',
    buttonTakeover: 'Przejmij kamere',
    buttonSwitchLanguage: 'English',
    buttonSwitchLanguageAria: 'Zmien jezyk',
    labelResolution: 'Rozdzielczosc',
    optionLow: 'Niska',
    optionMedium: 'Srednia',
    optionHigh: 'Wysoka',
    ledStateOn: 'ON',
    ledStateOff: 'OFF',
    statusActiveTemplate: 'Transmisja aktywna. Jestes aktywnym ogladajacym. Aktywnosc: {lagMs} ms. LED: {led}.',
    overlayCameraOfflineRetry: 'Kamera offline. Proba ponownego polaczenia...',
    statusCameraOfflineWait: 'Kamera offline. Czekam na powrot polaczenia...',
    overlayCameraBackOnline: 'Kamera wrocila online. Laczenie...',
    statusCameraBackOnline: 'Kamera wrocila online. Trwa laczenie...',
    statusSessionTaken: 'Sesja zostala przejeta przez innego uzytkownika.',
    overlaySessionTakenTakeover: 'Twoja sesja zostala przejeta przez innego uzytkownika. Kliknij przycisk, aby przejac kamere.',
    statusCameraBusy: 'Kamera jest aktualnie zajeta przez innego uzytkownika.',
    overlayCameraBusyTakeover: 'Kamera jest aktualnie uzywana przez innego klienta. Kliknij przycisk, aby przejac kamere.',
    overlayNoFramesRecover: 'Brak nowych klatek. Przywracam transmisje...',
    statusSessionInactiveRetake: 'Sesja nieaktywna. Przejmuje obraz ponownie...',
    overlayLikelyBusy: 'Prawdopodobnie ktos uzywa kamerki.',
    statusStreamRestoreFailed: 'Nie udalo sie przywrocic transmisji.',
    overlayRestoringStream: 'Przywracam transmisje...',
    statusStreamProblemReconnect: 'Wykryto problem z transmisja. Trwa ponowne laczenie...',
    statusStabilizingSession: 'Stabilizuje sesje na serwerze. Ponawiam...',
    statusTakeoverButtonOnly: 'Kamera zajeta. Przejecie tylko po kliknieciu przycisku.',
    overlayTakeoverRetryPrompt: 'Kamera jest aktualnie uzywana przez innego klienta. Kliknij, aby przejac ponownie.',
    statusTakeoverRetryTemplate: 'Przejecie nieudane. Sprobuj ponownie za okolo {retryMs} ms.',
    overlayConnectingCamera: 'Laczenie z kamera...',
    statusConnectingCamera: 'Laczenie z kamera...',
    statusWeakSignalReconnect: 'Slaby sygnal. Ponawiam polaczenie z kamera...',
    overlayStartImageFailed: 'Nie udalo sie uruchomic obrazu. Kliknij przycisk, aby przejac kamere.',
    statusDeviceBusyOrWeakSignal: 'Urzadzenie zajete lub slaby sygnal. Kliknij przycisk.',
    overlayUsedByAnotherClient: 'Kamera jest aktualnie uzywana przez innego klienta.',
    overlayChangingImageSettings: 'Zmieniam ustawienia obrazu...',
    statusStreamStillStopping: 'Poczekaj chwile, strumien jeszcze sie zatrzymuje.',
    statusCameraBusyByOther: 'Kamera jest zajeta przez innego klienta.',
    statusImageSettingsChangeFailed: 'Nie udalo sie zmienic ustawien obrazu.',
    statusImageSettingsSaved: 'Ustawienia obrazu zapisane.',
    statusNetworkErrorChangingImage: 'Blad sieci podczas zmiany ustawien obrazu.',
    overlayCameraUnavailableTakeover: 'Kamera jest zajeta lub niedostepna. Kliknij przycisk, aby przejac kamere.',
    statusTakeoverOnlyByButton: 'Przejecie dziala tylko po kliknieciu przycisku.',
    overlayTakingOverCamera: 'Przejmuje kamere...',
    statusTakingOverCamera: 'Proba przejecia kamery...'
  };
  let strings = { ...defaultStrings };

  let streamRunning = false;
  let streamHadFrame = false;
  let connectHintTimer = null;
  let reconnectTimer = null;
  let statusPollTimer = null;
  let statusPollFailCount = 0;
  let deviceOffline = false;
  let streamConnectAttempts = 0;
  let pendingTakeoverForNextStart = true;
  let awaitingManualTakeover = false;
  let manualTakeoverInFlight = false;
  let ledRequestInFlight = false;
  let camCfgRequestInFlight = false;
  let unlockApplyOnNextFrame = false;
  let lastLedClickAt = 0;
  const ledDebounceMs = 300;
  const maxStreamConnectAttempts = 3;
  const streamRetryDelayMs = 900;
  const statusPollMs = 1200;
  const maxStatusPollFailuresForOffline = 4;
  const retryJitterMaxMs = 900;
  const firstFrameWaitMs = 7000;
  const languageStorageKey = 'esp32cam.ui.lang';
  const supportedLanguages = ['pl', 'en'];
  let currentLanguage = 'pl';

  function getInitialLanguage() {
    try {
      const saved = localStorage.getItem(languageStorageKey);
      if (saved && supportedLanguages.includes(saved)) {
        return saved;
      }
    } catch (e) {
    }
    return 'pl';
  }

  function t(key) {
    return strings[key] || defaultStrings[key] || key;
  }

  function tf(key, values) {
    let text = t(key);
    if (!values) {
      return text;
    }
    Object.keys(values).forEach((name) => {
      text = text.replaceAll(`{${name}}`, String(values[name]));
    });
    return text;
  }

  async function loadStrings(languageCode) {
    const language = supportedLanguages.includes(languageCode) ? languageCode : 'pl';
    try {
      const response = await fetch(`/assets/strings.${language}.json?v=1`, { cache: 'no-store' });
      if (!response.ok) {
        return;
      }
      const data = await response.json();
      if (data && typeof data === 'object') {
        strings = { ...defaultStrings, ...data };
        currentLanguage = language;
      }
    } catch (e) {
    }
  }

  function applyStaticTexts() {
    document.documentElement.lang = currentLanguage;
    document.title = t('pageTitle');
    img.alt = t('cameraAlt');

    if (ledBtn) {
      ledBtn.textContent = t('buttonLedToggle');
    }
    if (fullscreenBtn) {
      fullscreenBtn.textContent = t('buttonFullscreen');
    }
    if (exitFullscreenBtn) {
      exitFullscreenBtn.textContent = t('buttonExitFullscreen');
      exitFullscreenBtn.setAttribute('aria-label', t('buttonExitFullscreen'));
    }
    if (applyCamBtn) {
      applyCamBtn.textContent = t('buttonApplyImage');
    }
    if (langBtn) {
      langBtn.textContent = t('buttonSwitchLanguage');
      langBtn.setAttribute('aria-label', t('buttonSwitchLanguageAria'));
    }
    if (takeoverBtn) {
      takeoverBtn.textContent = t('buttonTakeover');
    }
    if (frameLabel) {
      frameLabel.textContent = t('labelResolution');
    }
    if (frameQvgaOption) {
      frameQvgaOption.textContent = t('optionLow');
    }
    if (frameVgaOption) {
      frameVgaOption.textContent = t('optionMedium');
    }
    if (frameSvgaOption) {
      frameSvgaOption.textContent = t('optionHigh');
    }
  }

  async function switchLanguage() {
    const nextLanguage = currentLanguage === 'pl' ? 'en' : 'pl';
    await loadStrings(nextLanguage);
    try {
      localStorage.setItem(languageStorageKey, currentLanguage);
    } catch (e) {
    }
    applyStaticTexts();
    updateFullscreenButtonLabel();

    if (streamRunning && streamHadFrame) {
      setActiveViewerStatus(0, 'off');
    }
  }

  if (takeoverBtn) {
    takeoverBtn.style.display = 'none';
  }

  function setActiveViewerStatus(lastSeenMs, ledState) {
    const lagMs = Number.isFinite(lastSeenMs) ? Math.max(0, Math.trunc(lastSeenMs)) : 0;
    const ledText = ledState === 'on' ? t('ledStateOn') : t('ledStateOff');
    status.textContent = tf('statusActiveTemplate', { lagMs, led: ledText });
  }

  function setOverlay(message, showSpinner) {
    overlay.style.display = (message || showSpinner) ? 'flex' : 'none';
    overlayMessage.textContent = message || '';
    loader.style.display = showSpinner ? 'block' : 'none';
    if (showSpinner && takeoverBtn) {
      takeoverBtn.style.display = 'none';
      takeoverBtn.disabled = false;
    }
    if (!message && !showSpinner) {
      if (takeoverBtn) {
        takeoverBtn.style.display = 'none';
        takeoverBtn.disabled = false;
      }
    }
  }

  function showManualTakeoverPrompt(message) {
    awaitingManualTakeover = true;
    pendingTakeoverForNextStart = false;
    manualTakeoverInFlight = false;
    if (reconnectTimer !== null) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
    setOverlay(message, false);
    if (takeoverBtn) {
      takeoverBtn.style.display = 'inline-flex';
      takeoverBtn.disabled = false;
    }
  }

  function hidePreviewWithMessage(message) {
    img.style.display = 'none';
    img.removeAttribute('src');
    status.textContent = '';
    setOverlay(message, false);
  }

  function stopStream() {
    if (streamRunning) {
      img.removeAttribute('src');
      streamRunning = false;
    }
    streamConnectAttempts = 0;
    if (connectHintTimer !== null) {
      clearTimeout(connectHintTimer);
      connectHintTimer = null;
    }
    if (reconnectTimer !== null) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
    img.style.display = 'none';
    setOverlay('', false);
  }

  function setOfflineUi() {
    status.textContent = t('statusCameraOfflineWait');
    setOverlay(t('overlayCameraOfflineRetry'), true);
  }

  async function pollStatus() {
    if (camCfgRequestInFlight) {
      return;
    }

    try {
      const response = await fetch(
        `/?status=1&viewer=${encodeURIComponent(viewerId)}&t=${Date.now()}`,
        { cache: 'no-store' }
      );
      if (!response.ok) {
        statusPollFailCount += 1;
        if (statusPollFailCount >= maxStatusPollFailuresForOffline && !deviceOffline) {
          deviceOffline = true;
          stopStream();
          setOfflineUi();
        }
        return;
      }

      statusPollFailCount = 0;
      if (deviceOffline) {
        deviceOffline = false;
        streamConnectAttempts = 0;
        setOverlay(t('overlayCameraBackOnline'), true);
        status.textContent = t('statusCameraBackOnline');
        if (!streamRunning && reconnectTimer === null) {
          startStream();
        }
      }

      const data = await response.json();
      if (!data) {
        return;
      }

      if (data.busy === true && streamHadFrame) {
        stopStream();
        status.textContent = t('statusSessionTaken');
        showManualTakeoverPrompt(t('overlaySessionTakenTakeover'));
        return;
      }

      if (data.busy === true && !streamHadFrame && streamRunning && reconnectTimer === null) {
        stopStream();
        status.textContent = t('statusCameraBusy');
        showManualTakeoverPrompt(t('overlayCameraBusyTakeover'));
        return;
      }

      if (data.stale === true && streamRunning && streamHadFrame && reconnectTimer === null) {
        setOverlay(t('overlayNoFramesRecover'), true);
        scheduleStreamRetry();
        return;
      }

      if (data.active !== true && streamRunning && streamHadFrame && reconnectTimer === null) {
        scheduleStreamRetry(1200, t('statusSessionInactiveRetake'));
        return;
      }

      if (data.active === true && streamRunning) {
        setActiveViewerStatus(data.lastSeenMs, data.led);
      }
    } catch (e) {
      statusPollFailCount += 1;
      if (statusPollFailCount >= maxStatusPollFailuresForOffline && !deviceOffline) {
        deviceOffline = true;
        stopStream();
        setOfflineUi();
      }
    }
  }

  function startStatusPoll() {
    if (statusPollTimer !== null) {
      clearInterval(statusPollTimer);
    }
    statusPollTimer = setInterval(pollStatus, statusPollMs);
  }

  async function claimSession(forceTakeover) {
    const forceFlag = forceTakeover ? '&force=1' : '';
    try {
      const response = await fetch(`/?claim=1&viewer=${encodeURIComponent(viewerId)}${forceFlag}&t=${Date.now()}`, { cache: 'no-store' });
      if (response.ok) {
        return { ok: true, retryMs: 0, busy: false };
      }

      if (response.status === 423) {
        return { ok: false, retryMs: 0, busy: true };
      }

      if (response.status === 429) {
        try {
          const data = await response.json();
          const retryMs = (data && Number.isFinite(data.retryMs)) ? Math.max(200, Math.trunc(data.retryMs)) : streamRetryDelayMs;
          return { ok: false, retryMs, busy: false };
        } catch (e) {
          return { ok: false, retryMs: streamRetryDelayMs, busy: false };
        }
      }

      return { ok: false, retryMs: streamRetryDelayMs, busy: false };
    } catch (e) {
      return { ok: false, retryMs: streamRetryDelayMs, busy: false };
    }
  }

  function scheduleStreamRetry(delayMs = streamRetryDelayMs, message = null) {
    const retryMessage = message || t('statusStreamProblemReconnect');
    if (streamRunning) {
      img.removeAttribute('src');
      streamRunning = false;
    }

    pendingTakeoverForNextStart = true;

    if (streamConnectAttempts >= maxStreamConnectAttempts) {
      setOverlay(t('overlayLikelyBusy'), true);
      status.textContent = t('statusStreamRestoreFailed');
      return;
    }

    if (reconnectTimer !== null) {
      clearTimeout(reconnectTimer);
    }
    setOverlay(t('overlayRestoringStream'), true);
    status.textContent = retryMessage;
    const jitterMs = Math.floor(Math.random() * retryJitterMaxMs);
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      startStream();
    }, delayMs + jitterMs);
  }

  async function startStream() {
    if (streamRunning) {
      return;
    }

    if (deviceOffline) {
      return;
    }

    if (pendingTakeoverForNextStart) {
      const claim = await claimSession(awaitingManualTakeover);
      if (!claim.ok) {
        if (claim.busy) {
          showManualTakeoverPrompt(t('overlayCameraBusyTakeover'));
          status.textContent = t('statusTakeoverButtonOnly');
          return;
        }
        if (awaitingManualTakeover) {
          const retryMs = (claim.retryMs && claim.retryMs > 0) ? claim.retryMs : streamRetryDelayMs;
          showManualTakeoverPrompt(t('overlayTakeoverRetryPrompt'));
          status.textContent = tf('statusTakeoverRetryTemplate', { retryMs: Math.trunc(retryMs) });
          return;
        }
        const retryMs = (claim.retryMs && claim.retryMs > 0) ? claim.retryMs : streamRetryDelayMs;
        scheduleStreamRetry(retryMs, t('statusStabilizingSession'));
        return;
      }
      pendingTakeoverForNextStart = false;
    }

    if (streamRunning) {
      return;
    }

    streamHadFrame = false;
    streamConnectAttempts += 1;
    setOverlay(t('overlayConnectingCamera'), true);
    status.textContent = t('statusConnectingCamera');
    img.style.display = 'none';
    img.src = streamBaseUrl + '?stream=1&viewer=' + encodeURIComponent(viewerId) + '&t=' + Date.now();
    streamRunning = true;

    if (connectHintTimer !== null) {
      clearTimeout(connectHintTimer);
    }
    connectHintTimer = setTimeout(() => {
      if (streamRunning) {
        if (!streamHadFrame) {
          if (!awaitingManualTakeover && streamConnectAttempts < 2) {
            scheduleStreamRetry(2200, t('statusWeakSignalReconnect'));
            return;
          }
          stopStream();
          showManualTakeoverPrompt(t('overlayStartImageFailed'));
          status.textContent = t('statusDeviceBusyOrWeakSignal');
        } else {
          scheduleStreamRetry();
        }
      }
    }, firstFrameWaitMs);
  }

  async function toggleLed() {
    const now = Date.now();
    if (ledRequestInFlight || (now - lastLedClickAt) < ledDebounceMs) {
      return;
    }
    lastLedClickAt = now;
    ledRequestInFlight = true;

    try {
      const response = await fetch(
        ledBaseUrl + '?led=toggle&viewer=' + encodeURIComponent(viewerId) + '&t=' + Date.now(),
        { cache: 'no-store' }
      );
      if (!response.ok) {
        if (response.status === 423) {
          toolbar.style.display = 'none';
          stopStream();
          hidePreviewWithMessage(t('overlayUsedByAnotherClient'));
        }
        return;
      }
    } catch (e) {
    } finally {
      ledRequestInFlight = false;
    }
  }

  ledBtn.addEventListener('click', toggleLed);

  async function loadCamConfig() {
    try {
      const response = await fetch(
        `/?camcfg=1&viewer=${encodeURIComponent(viewerId)}&t=${Date.now()}`,
        { cache: 'no-store' }
      );
      if (!response.ok) {
        return;
      }
      const cfg = await response.json();
      if (cfg.frame) {
        frameSizeSel.value = cfg.frame;
      }
    } catch (e) {
    }
  }

  async function applyCamConfig() {
    if (camCfgRequestInFlight) {
      return;
    }

    camCfgRequestInFlight = true;
    unlockApplyOnNextFrame = false;
    applyCamBtn.disabled = true;
    status.textContent = '';

    const wasRunning = streamRunning;
    if (wasRunning) {
      stopStream();
      setOverlay(t('overlayChangingImageSettings'), true);
      await new Promise((resolve) => setTimeout(resolve, 250));
    } else {
      setOverlay(t('overlayChangingImageSettings'), true);
    }

    try {
      const response = await fetch(
        `/?camcfg=1&set=1&viewer=${encodeURIComponent(viewerId)}&frame=${encodeURIComponent(frameSizeSel.value)}&t=${Date.now()}`,
        { cache: 'no-store' }
      );

      if (!response.ok) {
        if (response.status === 409) {
          status.textContent = t('statusStreamStillStopping');
        } else if (response.status === 423) {
          status.textContent = t('statusCameraBusyByOther');
        } else {
          status.textContent = t('statusImageSettingsChangeFailed');
        }
        if (wasRunning) {
          startStream();
        } else {
          setOverlay('', false);
        }
        return;
      }

      status.textContent = t('statusImageSettingsSaved');
      if (wasRunning) {
        unlockApplyOnNextFrame = true;
        setTimeout(startStream, 200);
      } else {
        setOverlay('', false);
        applyCamBtn.disabled = false;
      }
    } catch (e) {
      status.textContent = t('statusNetworkErrorChangingImage');
      if (wasRunning) {
        startStream();
      } else {
        setOverlay('', false);
      }
    } finally {
      camCfgRequestInFlight = false;
      if (!unlockApplyOnNextFrame) {
        applyCamBtn.disabled = false;
      }
    }
  }

  applyCamBtn.addEventListener('click', applyCamConfig);

  function isFullscreenActive() {
    return !!(document.fullscreenElement || document.webkitFullscreenElement);
  }

  function updateFullscreenButtonLabel() {
    const isActive = isFullscreenActive();
    fullscreenBtn.textContent = isActive ? t('buttonExitFullscreen') : t('buttonFullscreen');
    if (exitFullscreenBtn) {
      exitFullscreenBtn.textContent = t('buttonExitFullscreen');
      exitFullscreenBtn.setAttribute('aria-label', t('buttonExitFullscreen'));
    }
    exitFullscreenBtn.disabled = !isActive;
  }

  async function toggleFullscreen() {
    try {
      if (!isFullscreenActive()) {
        if (preview.requestFullscreen) {
          await preview.requestFullscreen();
        } else if (preview.webkitRequestFullscreen) {
          preview.webkitRequestFullscreen();
        }
      } else {
        if (document.exitFullscreen) {
          await document.exitFullscreen();
        } else if (document.webkitExitFullscreen) {
          document.webkitExitFullscreen();
        }
      }
    } catch (e) {
    }
    updateFullscreenButtonLabel();
  }

  fullscreenBtn.addEventListener('click', toggleFullscreen);
  exitFullscreenBtn.addEventListener('click', async () => {
    if (!isFullscreenActive()) {
      return;
    }
    try {
      if (document.exitFullscreen) {
        await document.exitFullscreen();
      } else if (document.webkitExitFullscreen) {
        document.webkitExitFullscreen();
      }
    } catch (e) {
    }
    updateFullscreenButtonLabel();
  });
  document.addEventListener('fullscreenchange', updateFullscreenButtonLabel);
  document.addEventListener('webkitfullscreenchange', updateFullscreenButtonLabel);

  img.onload = () => {
    if (connectHintTimer !== null) {
      clearTimeout(connectHintTimer);
      connectHintTimer = null;
    }
    if (reconnectTimer !== null) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
    streamConnectAttempts = 0;
    streamHadFrame = true;
    pendingTakeoverForNextStart = false;
    awaitingManualTakeover = false;
    manualTakeoverInFlight = false;
    setOverlay('', false);
    img.style.display = 'block';
    if (unlockApplyOnNextFrame) {
      unlockApplyOnNextFrame = false;
      applyCamBtn.disabled = false;
    }
    setActiveViewerStatus(0, 'off');
    toolbar.style.display = 'flex';
  };

  img.onerror = () => {
    if (unlockApplyOnNextFrame) {
      unlockApplyOnNextFrame = false;
      applyCamBtn.disabled = false;
    }

    if (awaitingManualTakeover || !streamHadFrame) {
      manualTakeoverInFlight = false;
      showManualTakeoverPrompt(t('overlayCameraUnavailableTakeover'));
      status.textContent = t('statusTakeoverOnlyByButton');
      return;
    }
    scheduleStreamRetry();
  };

  if (takeoverBtn) {
    takeoverBtn.addEventListener('click', () => {
      if (manualTakeoverInFlight) {
        return;
      }
      manualTakeoverInFlight = true;
      awaitingManualTakeover = true;
      pendingTakeoverForNextStart = true;
      streamConnectAttempts = 0;
      takeoverBtn.disabled = true;
      setOverlay(t('overlayTakingOverCamera'), true);
      status.textContent = t('statusTakingOverCamera');
      startStream();
    });
  }

  if (langBtn) {
    langBtn.addEventListener('click', () => {
      switchLanguage();
    });
  }

  async function bootstrap() {
    currentLanguage = getInitialLanguage();
    await loadStrings(currentLanguage);
    applyStaticTexts();
    startStatusPoll();
    startStream();
    loadCamConfig();
    updateFullscreenButtonLabel();
  }

  bootstrap();
})();
)JS";
