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
  const ledBaseUrl = `${location.protocol}//${location.hostname}:81/`;
  const streamBaseUrl = `${location.protocol}//${location.hostname}:82/`;
  const viewerId = (globalThis.crypto && crypto.randomUUID)
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  let streamRunning = false;
  let streamHadFrame = false;
  let connectHintTimer = null;
  let reconnectTimer = null;
  let streamConnectAttempts = 0;
  let ledRequestInFlight = false;
  let camCfgRequestInFlight = false;
  let unlockApplyOnNextFrame = false;
  let lastLedClickAt = 0;
  const ledDebounceMs = 300;
  const maxStreamConnectAttempts = 3;
  const streamRetryDelayMs = 900;

  function setOverlay(message, showSpinner) {
    overlay.style.display = (message || showSpinner) ? 'flex' : 'none';
    overlayMessage.textContent = message || '';
    loader.style.display = showSpinner ? 'block' : 'none';
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

  async function claimTakeover() {
    try {
      await fetch(`/?takeover=1&viewer=${encodeURIComponent(viewerId)}&t=${Date.now()}`, { cache: 'no-store' });
    } catch (e) {
    }
  }

  function scheduleStreamRetry() {
    if (streamRunning) {
      img.removeAttribute('src');
      streamRunning = false;
    }

    if (streamConnectAttempts >= maxStreamConnectAttempts) {
      setOverlay('Prawdopodobnie ktoś używa kamerki.', true);
      return;
    }

    if (reconnectTimer !== null) {
      clearTimeout(reconnectTimer);
    }
    setOverlay('Przejmuję sesję kamery...', true);
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      startStream();
    }, streamRetryDelayMs);
  }

  async function startStream() {
    if (streamRunning) {
      return;
    }

    await claimTakeover();

    if (streamRunning) {
      return;
    }

    streamConnectAttempts += 1;
    setOverlay('Łączenie z kamerą...', true);
    img.style.display = 'block';
    img.src = streamBaseUrl + '?stream=1&viewer=' + encodeURIComponent(viewerId) + '&t=' + Date.now();
    streamRunning = true;

    if (connectHintTimer !== null) {
      clearTimeout(connectHintTimer);
    }
    connectHintTimer = setTimeout(() => {
      if (streamRunning) {
        scheduleStreamRetry();
      }
    }, 3500);
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
    setOverlay('', false);
    if (unlockApplyOnNextFrame) {
      unlockApplyOnNextFrame = false;
      applyCamBtn.disabled = false;
    }
    status.textContent = '';
    toolbar.style.display = 'flex';
  };

  img.onerror = () => {
    if (unlockApplyOnNextFrame) {
      unlockApplyOnNextFrame = false;
      applyCamBtn.disabled = false;
    }

    if (streamHadFrame) {
      stopStream();
      hidePreviewWithMessage('Twoja sesja została przejęta przez innego użytkownika.');
      return;
    }

    scheduleStreamRetry();
  };

  startStream();
  loadCamConfig();
  updateFullscreenButtonLabel();
})();
)JS";
