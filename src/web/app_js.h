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
  const toolbar = document.querySelector('.toolbar');
  const overlay = document.getElementById('overlay');
  const overlayMessage = document.getElementById('overlayMessage');
  const loader = document.getElementById('loader');
  const takeoverBtn = document.getElementById('takeoverBtn');
  const ledBaseUrl = `${location.protocol}//${location.hostname}:81/`;
  const streamBaseUrl = `${location.protocol}//${location.hostname}:82/`;
  const viewerId = (globalThis.crypto && crypto.randomUUID)
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
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

  if (takeoverBtn) {
    takeoverBtn.style.display = 'none';
  }

  function setActiveViewerStatus(lastSeenMs, ledState) {
    const lagMs = Number.isFinite(lastSeenMs) ? Math.max(0, Math.trunc(lastSeenMs)) : 0;
    const ledText = ledState === 'on' ? 'ON' : 'OFF';
    status.textContent = `Transmisja aktywna. Jesteś aktywnym oglądaczem. Aktywność: ${lagMs} ms. LED: ${ledText}.`;
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
    status.textContent = 'Kamera offline. Czekam na powrót połączenia...';
    setOverlay('Kamera offline. Próba ponownego połączenia...', true);
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
        setOverlay('Kamera wróciła online. Łączenie...', true);
        status.textContent = 'Kamera wróciła online. Trwa łączenie...';
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
        status.textContent = 'Sesja zostala przejeta przez innego uzytkownika.';
        showManualTakeoverPrompt('Twoja sesja została przejęta przez innego użytkownika. Kliknij przycisk, aby przejąć kamerę.');
        return;
      }

      if (data.busy === true && !streamHadFrame && streamRunning && reconnectTimer === null) {
        stopStream();
        status.textContent = 'Kamera jest aktualnie zajęta przez innego użytkownika.';
        showManualTakeoverPrompt('Kamera jest aktualnie używana przez innego klienta. Kliknij przycisk, aby przejąć kamerę.');
        return;
      }

      if (data.stale === true && streamRunning && streamHadFrame && reconnectTimer === null) {
        setOverlay('Brak nowych klatek. Przywracam transmisję...', true);
        scheduleStreamRetry();
        return;
      }

      if (data.active !== true && streamRunning && streamHadFrame && reconnectTimer === null) {
        scheduleStreamRetry(1200, 'Sesja nieaktywna. Przejmuje obraz ponownie...');
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

  function scheduleStreamRetry(delayMs = streamRetryDelayMs, message = 'Wykryto problem z transmisja. Trwa ponowne laczenie...') {
    if (streamRunning) {
      img.removeAttribute('src');
      streamRunning = false;
    }

    pendingTakeoverForNextStart = true;

    if (streamConnectAttempts >= maxStreamConnectAttempts) {
      setOverlay('Prawdopodobnie ktoś używa kamerki.', true);
      status.textContent = 'Nie udalo sie przywrocic transmisji.';
      return;
    }

    if (reconnectTimer !== null) {
      clearTimeout(reconnectTimer);
    }
    setOverlay('Przywracam transmisję...', true);
    status.textContent = message;
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
          showManualTakeoverPrompt('Kamera jest aktualnie używana przez innego klienta. Kliknij przycisk, aby przejąć kamerę.');
          status.textContent = 'Kamera zajęta. Przejęcie tylko po kliknięciu przycisku.';
          return;
        }
        if (awaitingManualTakeover) {
          const retryMs = (claim.retryMs && claim.retryMs > 0) ? claim.retryMs : streamRetryDelayMs;
          showManualTakeoverPrompt('Kamera jest aktualnie używana przez innego klienta. Kliknij, aby przejąć ponownie.');
          status.textContent = `Przejęcie nieudane. Spróbuj ponownie za około ${Math.trunc(retryMs)} ms.`;
          return;
        }
        const retryMs = (claim.retryMs && claim.retryMs > 0) ? claim.retryMs : streamRetryDelayMs;
        scheduleStreamRetry(retryMs, 'Stabilizuję sesję na serwerze. Ponawiam...');
        return;
      }
      pendingTakeoverForNextStart = false;
    }

    if (streamRunning) {
      return;
    }

    streamHadFrame = false;
    streamConnectAttempts += 1;
    setOverlay('Łączenie z kamerą...', true);
    status.textContent = 'Łączenie z kamerą...';
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
            scheduleStreamRetry(2200, 'Słaby sygnał. Ponawiam połączenie z kamerą...');
            return;
          }
          stopStream();
          showManualTakeoverPrompt('Nie udało się uruchomić obrazu. Kliknij przycisk, aby przejąć kamerę.');
          status.textContent = 'Urządzenie zajęte lub słaby sygnał. Kliknij przycisk.';
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
          hidePreviewWithMessage('Kamera jest aktualnie używana przez innego klienta.');
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
      setOverlay('Zmieniam ustawienia obrazu...', true);
      await new Promise((resolve) => setTimeout(resolve, 250));
    } else {
      setOverlay('Zmieniam ustawienia obrazu...', true);
    }

    try {
      const response = await fetch(
        `/?camcfg=1&set=1&viewer=${encodeURIComponent(viewerId)}&frame=${encodeURIComponent(frameSizeSel.value)}&t=${Date.now()}`,
        { cache: 'no-store' }
      );

      if (!response.ok) {
        if (response.status === 409) {
          status.textContent = 'Poczekaj chwile, strumien jeszcze sie zatrzymuje.';
        } else if (response.status === 423) {
          status.textContent = 'Kamera jest zajeta przez innego klienta.';
        } else {
          status.textContent = 'Nie udalo sie zmienic ustawien obrazu.';
        }
        if (wasRunning) {
          startStream();
        } else {
          setOverlay('', false);
        }
        return;
      }

      status.textContent = 'Ustawienia obrazu zapisane.';
      if (wasRunning) {
        unlockApplyOnNextFrame = true;
        setTimeout(startStream, 200);
      } else {
        setOverlay('', false);
        applyCamBtn.disabled = false;
      }
    } catch (e) {
      status.textContent = 'Blad sieci podczas zmiany ustawien obrazu.';
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
    fullscreenBtn.textContent = isActive ? 'Wyjdź z pełnego ekranu' : 'Pełny ekran';
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
      showManualTakeoverPrompt('Kamera jest zajęta lub niedostępna. Kliknij przycisk, aby przejąć kamerę.');
      status.textContent = 'Przejęcie działa tylko po kliknięciu przycisku.';
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
      setOverlay('Przejmuję kamerę...', true);
      status.textContent = 'Próba przejęcia kamery...';
      startStream();
    });
  }

  startStatusPoll();
  startStream();
  loadCamConfig();
  updateFullscreenButtonLabel();
})();
)JS";
