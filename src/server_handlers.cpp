#include "server_handlers.h"

#include <Arduino.h>

#include "camera_runtime.h"
#include "main_state.h"
#include "server_utils.h"
#include "web/app_js.h"
#include "web/index_html.h"
#include "web/style_css.h"

void serverHandleRoot() {
  if (server.hasArg("claim") || server.hasArg("takeover")) {
    auto wakeCameraAfterClaimIfNeeded = [&]() {
      if (cameraActive.load(std::memory_order_relaxed)) {
        return;
      }

      if (!streamLockCameraOp(pdMS_TO_TICKS(120))) {
        return;
      }

      if (!cameraActive.load(std::memory_order_relaxed)) {
        initCamera();
      }

      streamUnlockCameraOp();
    };

    String viewerToken = server.arg("viewer");
    if (viewerToken.length() == 0) {
      server.send(400, "text/plain", "Missing viewer token");
      return;
    }

    bool forceTakeover = server.hasArg("force") || server.hasArg("takeover");

    if (!sessionLockState(pdMS_TO_TICKS(500))) {
      server.send(503, "text/plain", "State busy");
      return;
    }

    IPAddress remote = server.client().remoteIP();
    unsigned long nowMs = millis();

    bool hasActiveSession = sessionValidateViewerLocked();
    bool tokenOwnsSession = hasActiveSession && (activeViewerToken == viewerToken);
    bool ownerChanged = false;

    if (!hasActiveSession || tokenOwnsSession) {
      ownerChanged = (!hasActiveSession || activeViewerToken != viewerToken);
      activeViewerIp = remote;
      activeViewerToken = viewerToken;
      activeViewerLastSeenMs = nowMs;
      sessionReserveViewerForStartLocked(viewerToken);
      sessionCancelNoViewerPowerDownLocked();
      if (ownerChanged) {
        lastOwnerSwitchMs = nowMs;
        lastOwnerSwitchCooldownMs = sessionNextOwnerSwitchCooldownMs();
        streamOwnerEpoch.fetch_add(1, std::memory_order_relaxed);
      }
      sessionUnlockState();
      wakeCameraAfterClaimIfNeeded();
      server.sendHeader("Cache-Control", "no-store");
      server.send(200, "application/json", "{\"ok\":true,\"decision\":\"join\"}");
      return;
    }

    unsigned long streamBeatMs = streamActivityHeartbeatMs.load(std::memory_order_relaxed);
    unsigned long latestSeenMs = (streamBeatMs > activeViewerLastSeenMs) ? streamBeatMs : activeViewerLastSeenMs;
    unsigned long inactivityMs = (latestSeenMs == 0) ? 0 : (unsigned long)(nowMs - latestSeenMs);
    bool staleSession = (inactivityMs > 2500);
    bool allowTakeover = forceTakeover || staleSession;
    if (!allowTakeover) {
      sessionUnlockState();
      server.sendHeader("Cache-Control", "no-store");
      server.send(423, "application/json", "{\"ok\":false,\"busy\":true}");
      return;
    }

    unsigned long cooldownMs = (lastOwnerSwitchCooldownMs == 0) ? kOwnerSwitchBaseCooldownMs : lastOwnerSwitchCooldownMs;
    if (lastOwnerSwitchMs != 0) {
      unsigned long sinceLastSwitchMs = (unsigned long)(nowMs - lastOwnerSwitchMs);
      if (sinceLastSwitchMs < cooldownMs) {
        unsigned long retryMs = cooldownMs - sinceLastSwitchMs;
        sessionUnlockState();
        char json[64];
        snprintf(json, sizeof(json), "{\"ok\":false,\"retryMs\":%lu}", retryMs);
        server.sendHeader("Cache-Control", "no-store");
        server.send(429, "application/json", json);
        return;
      }
    }

    activeViewerIp = remote;
    activeViewerToken = viewerToken;
    activeViewerLastSeenMs = nowMs;
    sessionReserveViewerForStartLocked(viewerToken);
    sessionCancelNoViewerPowerDownLocked();
    lastOwnerSwitchMs = nowMs;
    lastOwnerSwitchCooldownMs = sessionNextOwnerSwitchCooldownMs();
    streamOwnerEpoch.fetch_add(1, std::memory_order_relaxed);
    sessionUnlockState();
    wakeCameraAfterClaimIfNeeded();

    unsigned long handoffStartMs = millis();
    while (streamLoopCount.load(std::memory_order_relaxed) > 0 && (unsigned long)(millis() - handoffStartMs) < kOwnerHandoffWaitMs) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    int activeLoopsAfterWait = streamLoopCount.load(std::memory_order_relaxed);
    if (activeLoopsAfterWait > 0 && !forceTakeover) {
      unsigned long retryMs = 250 + (esp_random() % 550);
      char json[64];
      snprintf(json, sizeof(json), "{\"ok\":false,\"retryMs\":%lu}", retryMs);
      server.sendHeader("Cache-Control", "no-store");
      server.send(429, "application/json", json);
      return;
    }

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"ok\":true,\"decision\":\"takeover\"}");
    return;
  }

  if (server.hasArg("camcfg")) {
    String viewerToken = server.arg("viewer");
    if (!sessionIsViewerTokenAllowed(viewerToken)) {
      server.send(423, "text/plain", "Ktos aktualnie korzysta z podgladu");
      return;
    }

    if (server.hasArg("set")) {
      if (!server.hasArg("frame")) {
        server.send(400, "text/plain", "Missing frame parameter");
        return;
      }

      framesize_t requestedFrameSize = FRAMESIZE_VGA;
      if (!streamParseFrameSizeText(server.arg("frame"), requestedFrameSize)) {
        server.send(400, "text/plain", "Invalid frame size");
        return;
      }

      if (!streamLockCameraOp(pdMS_TO_TICKS(1500))) {
        server.send(503, "text/plain", "Camera operation busy");
        return;
      }

      unsigned long waitStartMs = millis();
      while (streamLoopCount.load(std::memory_order_relaxed) > 0 && (unsigned long)(millis() - waitStartMs) < 2000) {
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      if (streamLoopCount.load(std::memory_order_relaxed) > 0) {
        streamUnlockCameraOp();
        server.send(409, "text/plain", "Stream still shutting down");
        return;
      }

      if (!sessionLockState(pdMS_TO_TICKS(1000))) {
        streamUnlockCameraOp();
        server.send(503, "text/plain", "State busy");
        return;
      }

      if (sessionHasActiveViewerLocked()) {
        sessionUnlockState();
        streamUnlockCameraOp();
        server.send(409, "text/plain", "Stop stream before changing camera profile");
        return;
      }

      framesize_t previousFrameSize = FRAMESIZE_VGA;
      getCameraStreamConfig(previousFrameSize);
      bool wasActive = cameraActive.load(std::memory_order_relaxed);
      sessionUnlockState();

      if (requestedFrameSize == previousFrameSize) {
        streamUnlockCameraOp();
      } else {
        if (wasActive) {
          shutdownCamera();
        }

        setCameraStreamConfig(requestedFrameSize);

        if (!initCamera()) {
          setCameraStreamConfig(previousFrameSize);
          bool restoreOk = initCamera();
          if (!restoreOk) {
            setCameraStreamConfig(FRAMESIZE_VGA);
            restoreOk = initCamera();
          }
          if (restoreOk && !wasActive) {
            shutdownCamera();
          }
          streamUnlockCameraOp();
          server.send(503, "text/plain", "Invalid camera profile, restored previous config");
          return;
        }

        if (!wasActive) {
          shutdownCamera();
        }

        streamUnlockCameraOp();
      }
    }

    framesize_t currentFrameSize = FRAMESIZE_VGA;
    getCameraStreamConfig(currentFrameSize);

    char json[64];
    snprintf(json, sizeof(json), "{\"frame\":\"%s\"}", streamFrameSizeToText(currentFrameSize));
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
    return;
  }

  if (server.hasArg("status")) {
    String viewerToken = server.arg("viewer");
    bool busy = false;
    bool stale = false;
    bool active = false;
    unsigned long lastSeenMs = 0;
    sessionGetViewerStatusForToken(viewerToken, busy, stale, active, lastSeenMs);
    char json[112];
    serverBuildStatusJson(json, sizeof(json), busy, stale, active, lastSeenMs, ledGetEnabledSnapshot());
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
    return;
  }

  if (server.uri() == "/assets/style.css") {
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    server.send(200, "text/css", kStyleCss);
    return;
  }

  if (server.uri() == "/assets/app.js") {
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    server.send(200, "application/javascript", kAppJs);
    return;
  }

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send(200, "text/html", kIndexHtml);
}

void streamHandleRoot() {
  if (!streamServer.hasArg("stream")) {
    streamServer.send(400, "text/plain", "Missing stream parameter");
    return;
  }

  String viewerToken = streamServer.arg("viewer");
  if (viewerToken.length() == 0) {
    streamServer.send(423, "text/plain", "Ktos aktualnie korzysta z podgladu");
    return;
  }

  if (!streamLockCameraOp(pdMS_TO_TICKS(1500))) {
    streamServer.send(503, "text/plain", "Camera operation busy");
    return;
  }

  if (!sessionLockState(pdMS_TO_TICKS(1000))) {
    streamUnlockCameraOp();
    streamServer.send(503, "text/plain", "Stream state busy");
    return;
  }

  bool tokenActive = false;
  if (sessionValidateViewerLocked()) {
    if (activeViewerToken == viewerToken) {
      tokenActive = true;
    } else if (sessionIsReservationActiveLocked(viewerToken)) {
      activeViewerToken = viewerToken;
      activeViewerIp = streamServer.client().remoteIP();
      activeViewerLastSeenMs = millis();
      tokenActive = true;
    }
  }
  if (!tokenActive) {
    if (sessionIsReservationActiveLocked(viewerToken)) {
      sessionClearStreamReservationLocked();
    }
    sessionUnlockState();
    streamUnlockCameraOp();
    streamServer.send(423, "text/plain", "Ktos aktualnie korzysta z podgladu");
    return;
  }

  bool needInit = !cameraActive.load(std::memory_order_relaxed);
  sessionUnlockState();

  if (needInit && !initCamera()) {
    if (sessionLockState(pdMS_TO_TICKS(50))) {
      sessionClearStreamReservationLocked();
      sessionUnlockState();
    }
    streamUnlockCameraOp();
    streamServer.send(503, "text/plain", "Camera wakeup failed");
    return;
  }

  if (needInit) {
    // Fast wake path: keep startup responsive after idle power-down.
    unsigned long warmupStartMs = millis();
    while ((unsigned long)(millis() - warmupStartMs) < 900) {
      camera_fb_t *warmupFrame = esp_camera_fb_get();
      if (warmupFrame != nullptr) {
        esp_camera_fb_return(warmupFrame);
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  } else {
    if (!streamWarmupFirstFrameWithRecovery()) {
      // Non-fatal: continue and let stream loop try to recover frames.
      if (sessionLockState(pdMS_TO_TICKS(50))) {
        sessionClearStreamReservationLocked();
        sessionUnlockState();
      }
    }
  }

  if (!sessionLockState(pdMS_TO_TICKS(1000))) {
    streamUnlockCameraOp();
    streamServer.send(503, "text/plain", "Stream state busy");
    return;
  }

  tokenActive = false;
  if (sessionValidateViewerLocked()) {
    if (activeViewerToken == viewerToken) {
      tokenActive = true;
    } else if (sessionIsReservationActiveLocked(viewerToken)) {
      activeViewerToken = viewerToken;
      activeViewerIp = streamServer.client().remoteIP();
      activeViewerLastSeenMs = millis();
      tokenActive = true;
    }
  }
  if (!tokenActive) {
    if (sessionIsReservationActiveLocked(viewerToken)) {
      sessionClearStreamReservationLocked();
    }
    sessionUnlockState();
    streamUnlockCameraOp();
    streamServer.send(423, "text/plain", "Ktos aktualnie korzysta z podgladu");
    return;
  }

  unsigned long nowMs = millis();
  int activeLoops = streamLoopCount.load(std::memory_order_relaxed);
  if (activeLoops > 0) {
    unsigned long waitStartMs = millis();
    while (streamLoopCount.load(std::memory_order_relaxed) > 0 && (unsigned long)(millis() - waitStartMs) < 5000) {
      sessionUnlockState();
      streamUnlockCameraOp();
      vTaskDelay(pdMS_TO_TICKS(10));
      if (!streamLockCameraOp(pdMS_TO_TICKS(200))) {
        streamServer.send(503, "text/plain", "Camera operation busy");
        return;
      }
      if (!sessionLockState(pdMS_TO_TICKS(200))) {
        streamUnlockCameraOp();
        streamServer.send(503, "text/plain", "Stream state busy");
        return;
      }
      if (!sessionValidateViewerLocked() || activeViewerToken != viewerToken) {
        sessionUnlockState();
        streamUnlockCameraOp();
        streamServer.send(423, "text/plain", "Ktos aktualnie korzysta z podgladu");
        return;
      }
    }

    activeLoops = streamLoopCount.load(std::memory_order_relaxed);
    if (activeLoops > 0) {
      activeViewerLastSeenMs = nowMs;
      sessionUnlockState();
      streamUnlockCameraOp();
      streamServer.send(409, "text/plain", "Stream already active");
      return;
    }
  }

  sessionCancelNoViewerPowerDownLocked();
  activeViewerLastSeenMs = nowMs;
  sessionClearStreamReservationLocked();
  uint32_t myEpoch = streamOwnerEpoch.load(std::memory_order_relaxed);
  sessionUnlockState();
  streamUnlockCameraOp();

  camera_fb_t *firstFrame = nullptr;
  auto tryGetFirstFrame = [&](unsigned long budgetMs) -> camera_fb_t * {
    camera_fb_t *fb = nullptr;
    unsigned long startMs = millis();
    while (fb == nullptr && (unsigned long)(millis() - startMs) < budgetMs) {
      fb = esp_camera_fb_get();
      if (fb == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(16));
      }
    }
    return fb;
  };

  const unsigned long firstFrameBudgetMs = needInit ? 2200 : 6000;
  const unsigned long recoveryFrameBudgetMs = needInit ? 1200 : 4000;

  firstFrame = tryGetFirstFrame(firstFrameBudgetMs);

  if (firstFrame == nullptr) {
    shutdownCamera();
    if (initCamera()) {
      firstFrame = tryGetFirstFrame(recoveryFrameBudgetMs);
    }
  }

  if (firstFrame == nullptr && !needInit) {
    framesize_t currentFrameSize = FRAMESIZE_VGA;
    getCameraStreamConfig(currentFrameSize);
    if (currentFrameSize != FRAMESIZE_QVGA) {
      shutdownCamera();
      setCameraStreamConfig(FRAMESIZE_QVGA);
      if (initCamera()) {
        firstFrame = tryGetFirstFrame(recoveryFrameBudgetMs);
      }
    }
  }

  if (firstFrame == nullptr) {
    if (sessionLockState(pdMS_TO_TICKS(80))) {
      if (activeViewerToken == viewerToken) {
        releaseViewerLock();
        activeViewerToken = "";
        sessionClearStreamReservationLocked();
        sessionScheduleNoViewerPowerDownLocked();
      }
      sessionUnlockState();
    }
    streamServer.send(503, "text/plain", "Camera no frame after recovery");
    return;
  }

  WiFiClient client = streamServer.client();
  client.setNoDelay(true);
  client.setTimeout(350);
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println("Cache-Control: no-store, no-cache, must-revalidate, max-age=0");
  client.println("Pragma: no-cache");
  client.println("Expires: 0");
  client.println("Connection: close");
  client.println();

  unsigned long lastKeepAliveMs = 0;
  int skippedFrames = 0;
  int noFrameStreak = 0;
  const int maxSkipThreshold = 3;
  const unsigned long maxFrameSendMs = 90;
  const int maxNoFrameStreak = 300;
  bool noFrameFailure = false;
  streamLoopCount.fetch_add(1, std::memory_order_relaxed);

  unsigned long handoffStartMs = millis();
  while (streamLoopCount.load(std::memory_order_relaxed) > 1 && (unsigned long)(millis() - handoffStartMs) < 2500) {
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  if (streamLoopCount.load(std::memory_order_relaxed) > 1) {
    streamLoopCount.fetch_sub(1, std::memory_order_relaxed);
    if (firstFrame != nullptr) {
      esp_camera_fb_return(firstFrame);
    }
    client.stop();
    return;
  }

  if (firstFrame != nullptr) {
    size_t firstFrameLen = firstFrame->len;
    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned int)firstFrameLen);
    size_t firstWritten = client.write(firstFrame->buf, firstFrameLen);
    client.print("\r\n");
    esp_camera_fb_return(firstFrame);
    if (firstWritten != firstFrameLen) {
      streamLoopCount.fetch_sub(1, std::memory_order_relaxed);
      client.stop();
      return;
    }
    streamActivityHeartbeatMs.store(millis(), std::memory_order_relaxed);
  }

  while (client.connected()) {
    if (streamOwnerEpoch.load(std::memory_order_relaxed) != myEpoch) {
      break;
    }

    streamActivityHeartbeatMs.store(millis(), std::memory_order_relaxed);

    if (skippedFrames > 0) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        esp_camera_fb_return(fb);
        skippedFrames--;
        noFrameStreak = 0;
      } else {
        noFrameStreak++;
        if (noFrameStreak >= maxNoFrameStreak) {
          noFrameFailure = true;
          break;
        }
      }
      taskYIELD();
      continue;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      noFrameStreak++;
      if (noFrameStreak >= maxNoFrameStreak) {
        noFrameFailure = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    consecutiveNoFrameFailures.store(0, std::memory_order_relaxed);
    noFrameStreak = 0;

    size_t frameLen = fb->len;
    unsigned long sendStartMs = millis();
    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned int)frameLen);
    size_t written = client.write(fb->buf, frameLen);
    client.print("\r\n");
    unsigned long sendTimeMs = millis() - sendStartMs;

    esp_camera_fb_return(fb);
    if (written != frameLen) {
      skippedFrames = maxSkipThreshold;
      break;
    }

    if (sendTimeMs > maxFrameSendMs) {
      skippedFrames = maxSkipThreshold;
    }

    unsigned long nowMs = millis();
    if ((unsigned long)(nowMs - lastKeepAliveMs) >= 250) {
      if (sessionLockState(pdMS_TO_TICKS(1))) {
        activeViewerLastSeenMs = nowMs;
        sessionUnlockState();
      }
      lastKeepAliveMs = nowMs;
    }
    taskYIELD();
  }

  client.stop();

  if (sessionLockState(pdMS_TO_TICKS(200))) {
    if (activeViewerToken == viewerToken && streamOwnerEpoch.load(std::memory_order_relaxed) == myEpoch) {
      releaseViewerLock();
      activeViewerToken = "";
      sessionClearStreamReservationLocked();
      sessionScheduleNoViewerPowerDownLocked();
    }
    sessionUnlockState();
  }
  streamLoopCount.fetch_sub(1, std::memory_order_relaxed);

  if (noFrameFailure) {
    int failures = consecutiveNoFrameFailures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (failures >= kQualityDropNoFrameThreshold) {
      if (streamLockCameraOp(pdMS_TO_TICKS(400))) {
        if (streamLoopCount.load(std::memory_order_relaxed) == 0) {
          if (streamStepDownQualityIfNeeded()) {
            consecutiveNoFrameFailures.store(0, std::memory_order_relaxed);
          }
        }
        streamUnlockCameraOp();
      }
    }
  }
}

void ledHandleRoot() {
  ledServer.sendHeader("Cache-Control", "no-store");

  if (ledServer.hasArg("status")) {
    String viewerToken = ledServer.arg("viewer");
    bool busy = false;
    bool stale = false;
    bool active = false;
    unsigned long lastSeenMs = 0;
    sessionGetViewerStatusForToken(viewerToken, busy, stale, active, lastSeenMs);
    char json[112];
    serverBuildStatusJson(json, sizeof(json), busy, stale, active, lastSeenMs, ledGetEnabledSnapshot());
    ledServer.send(200, "application/json", json);
    return;
  }

  if (!ledServer.hasArg("led")) {
    char json[20];
    ledBuildJson(json, sizeof(json), ledGetEnabledSnapshot());
    ledServer.send(200, "application/json", json);
    return;
  }

  String viewerToken = ledServer.arg("viewer");

  if (!sessionLockState(pdMS_TO_TICKS(200))) {
    ledServer.send(503, "text/plain", "LED state busy");
    return;
  }

  bool allowed = (activeViewerIp[0] == 0);
  if (!allowed) {
    if ((unsigned long)(millis() - activeViewerLastSeenMs) > viewerHoldTimeoutMs) {
      releaseViewerLock();
      activeViewerToken = "";
      sessionScheduleNoViewerPowerDownLocked();
      allowed = true;
    } else {
      allowed = (activeViewerToken == viewerToken);
    }
  }

  if (!allowed) {
    sessionUnlockState();
    ledServer.send(423, "text/plain", "Ktos aktualnie korzysta z podgladu");
    return;
  }

  String cmd = ledServer.arg("led");
  if (cmd == "on") {
    if (!ledEnabled.load(std::memory_order_relaxed)) {
      setFlashLed(true);
      ledAutoOffAtMs = millis() + kLedAutoOffTimeoutMs;
    }
  } else if (cmd == "off") {
    setFlashLed(false);
    ledAutoOffAtMs = 0;
  } else if (cmd == "toggle") {
    bool nextState = !ledEnabled.load(std::memory_order_relaxed);
    setFlashLed(nextState);
    ledAutoOffAtMs = nextState ? (millis() + kLedAutoOffTimeoutMs) : 0;
  }

  sessionUnlockState();

  char json[20];
  ledBuildJson(json, sizeof(json), ledGetEnabledSnapshot());
  ledServer.send(200, "application/json", json);
}

void serverStartWeb() {
  server.on("/", HTTP_GET, serverHandleRoot);
  server.on("/assets/style.css", HTTP_GET, serverHandleRoot);
  server.on("/assets/app.js", HTTP_GET, serverHandleRoot);
  server.begin();

  ledServer.on("/", HTTP_GET, ledHandleRoot);
  ledServer.begin();

  streamServer.on("/", HTTP_GET, streamHandleRoot);
  streamServer.begin();
}
