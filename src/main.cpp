#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <esp32-hal-cpu.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <atomic>
#include "camera_runtime.h"
#include "web/index_html.h"
#include "web/style_css.h"
#include "web/app_js.h"

WebServer server(80);
WebServer ledServer(81);
WebServer streamServer(82);

SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t cameraOpMutex = nullptr;
unsigned long cameraShutdownAtMs = 0;
const unsigned long cameraIdleShutdownMs = 10000;
const bool enableCameraIdlePowerDown = false;
unsigned long ledAutoOffAtMs = 0;
const unsigned long ledAutoOffTimeoutMs = 15000;
unsigned long lastOwnerSwitchMs = 0;
unsigned long lastOwnerSwitchCooldownMs = 1200;
String activeViewerToken;
String reservedViewerToken;
unsigned long reservedViewerUntilMs = 0;
const uint32_t cpuFreqActiveMHz = 240;
const uint32_t cpuFreqIdleMHz = 160;
std::atomic<bool> highPerformanceMode{false};
std::atomic<int> streamLoopCount{0};
std::atomic<uint32_t> streamOwnerEpoch{0};
std::atomic<unsigned long> streamActivityHeartbeatMs{0};
const TickType_t serverTaskDelayActiveTicks = pdMS_TO_TICKS(4);
const TickType_t serverTaskDelayIdleTicks = pdMS_TO_TICKS(14);
const unsigned long ownerSwitchBaseCooldownMs = 700;
const unsigned long ownerSwitchJitterMaxMs = 900;
const unsigned long ownerHandoffWaitMs = 1400;
const unsigned long streamStartReservationMs = 9000;
const unsigned long firstFrameWarmupBudgetMs = 1800;
const int firstFrameWarmupAttempts = 4;
const int qualityDropNoFrameThreshold = 2;
std::atomic<int> consecutiveNoFrameFailures{0};

unsigned long nextOwnerSwitchCooldownMs() {
  uint32_t jitterMs = esp_random() % ownerSwitchJitterMaxMs;
  return ownerSwitchBaseCooldownMs + jitterMs;
}

#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
const BaseType_t controlCore = ARDUINO_RUNNING_CORE;
const BaseType_t streamCore = ARDUINO_RUNNING_CORE;
#else
const BaseType_t controlCore = 0;
const BaseType_t streamCore = 1;
#endif

void scheduleNoViewerPowerDownLocked();

void clearStreamReservationLocked() {
  reservedViewerToken = "";
  reservedViewerUntilMs = 0;
}

void reserveViewerForStartLocked(const String &viewerToken) {
  reservedViewerToken = viewerToken;
  reservedViewerUntilMs = millis() + streamStartReservationMs;
}

bool isReservationActiveLocked(const String &viewerToken) {
  if (reservedViewerToken.length() == 0 || reservedViewerToken != viewerToken) {
    return false;
  }
  if ((long)(millis() - reservedViewerUntilMs) > 0) {
    clearStreamReservationLocked();
    return false;
  }
  return true;
}

framesize_t lowerFrameSize(framesize_t current) {
  if (current == FRAMESIZE_SVGA) {
    return FRAMESIZE_VGA;
  }
  if (current == FRAMESIZE_VGA) {
    return FRAMESIZE_QVGA;
  }
  return FRAMESIZE_QVGA;
}

bool stepDownQualityIfNeeded() {
  framesize_t currentFrameSize = FRAMESIZE_VGA;
  getCameraStreamConfig(currentFrameSize);
  framesize_t nextFrameSize = lowerFrameSize(currentFrameSize);
  if (nextFrameSize == currentFrameSize) {
    return false;
  }

  bool wasActive = cameraActive.load(std::memory_order_relaxed);
  if (wasActive) {
    shutdownCamera();
  }
  setCameraStreamConfig(nextFrameSize);
  bool ok = initCamera();
  if (!ok) {
    setCameraStreamConfig(currentFrameSize);
    ok = initCamera();
  }
  if (ok && !wasActive) {
    shutdownCamera();
  }
  return ok;
}

bool warmupFirstFrameWithRecovery() {
  for (int attempt = 0; attempt < firstFrameWarmupAttempts; ++attempt) {
    unsigned long startMs = millis();
    while ((unsigned long)(millis() - startMs) < firstFrameWarmupBudgetMs) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb != nullptr) {
        esp_camera_fb_return(fb);
        consecutiveNoFrameFailures.store(0, std::memory_order_relaxed);
        return true;
      }
      delay(20);
    }

    shutdownCamera();
    if (!initCamera()) {
      continue;
    }
  }

  int failures = consecutiveNoFrameFailures.fetch_add(1, std::memory_order_relaxed) + 1;
  if (failures >= qualityDropNoFrameThreshold) {
    if (stepDownQualityIfNeeded()) {
      consecutiveNoFrameFailures.store(0, std::memory_order_relaxed);
    }
  }
  return false;
}

bool lockState(TickType_t timeoutTicks = pdMS_TO_TICKS(50)) {
  if (stateMutex == nullptr) {
    return true;
  }
  return xSemaphoreTake(stateMutex, timeoutTicks) == pdTRUE;
}

void unlockState() {
  if (stateMutex != nullptr) {
    xSemaphoreGive(stateMutex);
  }
}

bool lockCameraOp(TickType_t timeoutTicks = pdMS_TO_TICKS(50)) {
  if (cameraOpMutex == nullptr) {
    return true;
  }
  return xSemaphoreTake(cameraOpMutex, timeoutTicks) == pdTRUE;
}

void unlockCameraOp() {
  if (cameraOpMutex != nullptr) {
    xSemaphoreGive(cameraOpMutex);
  }
}

bool getLedEnabledSnapshot() {
  return ledEnabled.load(std::memory_order_relaxed);
}

void buildStatusJson(char *out, size_t outSize, bool busy, bool stale, bool active, unsigned long lastSeenMs, bool ledOn) {
  snprintf(out, outSize, "{\"busy\":%s,\"stale\":%s,\"active\":%s,\"lastSeenMs\":%lu,\"led\":\"%s\"}",
           busy ? "true" : "false", stale ? "true" : "false", active ? "true" : "false", lastSeenMs, ledOn ? "on" : "off");
}

void buildLedJson(char *out, size_t outSize, bool ledOn) {
  snprintf(out, outSize, "{\"led\":\"%s\"}", ledOn ? "on" : "off");
}

const char *frameSizeToText(framesize_t frameSize) {
  switch (frameSize) {
    case FRAMESIZE_QVGA:
      return "qvga";
    case FRAMESIZE_VGA:
      return "vga";
    case FRAMESIZE_SVGA:
      return "svga";
    default:
      return "vga";
  }
}

bool parseFrameSizeText(const String &value, framesize_t &frameSize) {
  if (value == "qvga") {
    frameSize = FRAMESIZE_QVGA;
    return true;
  }
  if (value == "vga") {
    frameSize = FRAMESIZE_VGA;
    return true;
  }
  if (value == "svga") {
    frameSize = FRAMESIZE_SVGA;
    return true;
  }
  return false;
}

void setPerformanceModeLocked(bool active) {
  if (highPerformanceMode.load(std::memory_order_relaxed) == active) {
    return;
  }

  setCpuFrequencyMhz(active ? cpuFreqActiveMHz : cpuFreqIdleMHz);
  highPerformanceMode.store(active, std::memory_order_relaxed);
}

bool isHighPerformanceModeSnapshot() {
  return highPerformanceMode.load(std::memory_order_relaxed);
}

bool hasActiveViewerSessionLocked() {
  if (activeViewerIp[0] == 0) {
    return false;
  }

  if ((unsigned long)(millis() - activeViewerLastSeenMs) > viewerHoldTimeoutMs) {
    releaseViewerLock();
    activeViewerToken = "";
    clearStreamReservationLocked();
    scheduleNoViewerPowerDownLocked();
    return false;
  }

  return true;
}

void scheduleNoViewerPowerDownLocked() {
  if (!enableCameraIdlePowerDown) {
    cameraShutdownAtMs = 0;
    return;
  }

  const unsigned long deadlineMs = millis() + cameraIdleShutdownMs;

  if (cameraActive.load(std::memory_order_relaxed) && cameraShutdownAtMs == 0) {
    cameraShutdownAtMs = deadlineMs;
  }

  if (ledEnabled.load(std::memory_order_relaxed)) {
    if (ledAutoOffAtMs == 0 || (long)(deadlineMs - ledAutoOffAtMs) < 0) {
      ledAutoOffAtMs = deadlineMs;
    }
  }
}

void cancelNoViewerPowerDownLocked() {
  cameraShutdownAtMs = 0;
}

bool validateViewerSessionLocked() {
  if (activeViewerIp[0] == 0) {
    return false;
  }

  if ((unsigned long)(millis() - activeViewerLastSeenMs) > viewerHoldTimeoutMs) {
    releaseViewerLock();
    activeViewerToken = "";
    clearStreamReservationLocked();
    scheduleNoViewerPowerDownLocked();
    return false;
  }

  return true;
}

bool isViewerTokenAllowed(const String &viewerToken) {
  if (viewerToken.length() == 0) {
    return false;
  }

  if (!lockState()) {
    return false;
  }

  bool allowed = true;
  if (validateViewerSessionLocked()) {
    allowed = (activeViewerToken == viewerToken);
  }

  unlockState();
  return allowed;
}

void getViewerStatusSnapshotForToken(const String &viewerToken, bool &busy, bool &stale, bool &active, unsigned long &lastSeenMs) {
  busy = true;
  stale = false;
  active = false;
  lastSeenMs = 0;

  if (!lockState()) {
    return;
  }

  busy = false;
  if (validateViewerSessionLocked()) {
    active = (activeViewerToken == viewerToken);
    unsigned long streamBeatMs = streamActivityHeartbeatMs.load(std::memory_order_relaxed);
    unsigned long latestSeenMs = (streamBeatMs > activeViewerLastSeenMs) ? streamBeatMs : activeViewerLastSeenMs;
    lastSeenMs = (latestSeenMs == 0) ? 0 : (unsigned long)(millis() - latestSeenMs);
    stale = (lastSeenMs > 2500);
    bool hasRunningStream = (streamLoopCount.load(std::memory_order_relaxed) > 0);
    busy = (!active && !stale && hasRunningStream);
  }

  unlockState();
}

void handleRoot() {
  if (server.hasArg("claim") || server.hasArg("takeover")) {
    String viewerToken = server.arg("viewer");
    if (viewerToken.length() == 0) {
      server.send(400, "text/plain", "Missing viewer token");
      return;
    }

    bool forceTakeover = server.hasArg("force") || server.hasArg("takeover");

    if (!lockState(pdMS_TO_TICKS(500))) {
      server.send(503, "text/plain", "State busy");
      return;
    }

    IPAddress remote = server.client().remoteIP();
    unsigned long nowMs = millis();

    bool hasActiveSession = validateViewerSessionLocked();
    bool tokenOwnsSession = hasActiveSession && (activeViewerToken == viewerToken);
    bool ownerChanged = false;

    if (!hasActiveSession || tokenOwnsSession) {
      ownerChanged = (!hasActiveSession || activeViewerToken != viewerToken);
      activeViewerIp = remote;
      activeViewerToken = viewerToken;
      activeViewerLastSeenMs = nowMs;
      reserveViewerForStartLocked(viewerToken);
      cancelNoViewerPowerDownLocked();
      setPerformanceModeLocked(true);
      if (ownerChanged) {
        lastOwnerSwitchMs = nowMs;
        lastOwnerSwitchCooldownMs = nextOwnerSwitchCooldownMs();
        streamOwnerEpoch.fetch_add(1, std::memory_order_relaxed);
      }
      unlockState();
      server.sendHeader("Cache-Control", "no-store");
      server.send(200, "application/json", "{\"ok\":true,\"decision\":\"join\"}");
      return;
    }

    unsigned long streamBeatMs = streamActivityHeartbeatMs.load(std::memory_order_relaxed);
    unsigned long latestSeenMs = (streamBeatMs > activeViewerLastSeenMs) ? streamBeatMs : activeViewerLastSeenMs;
    unsigned long inactivityMs = (latestSeenMs == 0) ? 0 : (unsigned long)(nowMs - latestSeenMs);
    bool staleSession = (inactivityMs > 2500);
    bool hasRunningStream = (streamLoopCount.load(std::memory_order_relaxed) > 0);
    bool allowTakeover = forceTakeover || staleSession || !hasRunningStream;
    if (!allowTakeover) {
      unlockState();
      server.sendHeader("Cache-Control", "no-store");
      server.send(423, "application/json", "{\"ok\":false,\"busy\":true}");
      return;
    }

    unsigned long cooldownMs = (lastOwnerSwitchCooldownMs == 0) ? ownerSwitchBaseCooldownMs : lastOwnerSwitchCooldownMs;
    if (lastOwnerSwitchMs != 0) {
      unsigned long sinceLastSwitchMs = (unsigned long)(nowMs - lastOwnerSwitchMs);
      if (sinceLastSwitchMs < cooldownMs) {
        unsigned long retryMs = cooldownMs - sinceLastSwitchMs;
        unlockState();
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
    reserveViewerForStartLocked(viewerToken);
    cancelNoViewerPowerDownLocked();
    setPerformanceModeLocked(true);
    lastOwnerSwitchMs = nowMs;
    lastOwnerSwitchCooldownMs = nextOwnerSwitchCooldownMs();
    streamOwnerEpoch.fetch_add(1, std::memory_order_relaxed);
    unlockState();

    unsigned long handoffStartMs = millis();
    while (streamLoopCount.load(std::memory_order_relaxed) > 0 && (unsigned long)(millis() - handoffStartMs) < ownerHandoffWaitMs) {
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
    if (!isViewerTokenAllowed(viewerToken)) {
      server.send(423, "text/plain", "Ktos aktualnie korzysta z podgladu");
      return;
    }

    if (server.hasArg("set")) {
      if (!server.hasArg("frame")) {
        server.send(400, "text/plain", "Missing frame parameter");
        return;
      }

      framesize_t requestedFrameSize = FRAMESIZE_VGA;
      if (!parseFrameSizeText(server.arg("frame"), requestedFrameSize)) {
        server.send(400, "text/plain", "Invalid frame size");
        return;
      }

      if (!lockCameraOp(pdMS_TO_TICKS(1500))) {
        server.send(503, "text/plain", "Camera operation busy");
        return;
      }

      unsigned long waitStartMs = millis();
      while (streamLoopCount.load(std::memory_order_relaxed) > 0 && (unsigned long)(millis() - waitStartMs) < 2000) {
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      if (streamLoopCount.load(std::memory_order_relaxed) > 0) {
        unlockCameraOp();
        server.send(409, "text/plain", "Stream still shutting down");
        return;
      }

      if (!lockState(pdMS_TO_TICKS(1000))) {
        unlockCameraOp();
        server.send(503, "text/plain", "State busy");
        return;
      }

      if (hasActiveViewerSessionLocked()) {
        unlockState();
        unlockCameraOp();
        server.send(409, "text/plain", "Stop stream before changing camera profile");
        return;
      }

      framesize_t previousFrameSize = FRAMESIZE_VGA;
      getCameraStreamConfig(previousFrameSize);
      bool wasActive = cameraActive.load(std::memory_order_relaxed);
      unlockState();

      if (requestedFrameSize == previousFrameSize) {
        unlockCameraOp();
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
          unlockCameraOp();
          server.send(503, "text/plain", "Invalid camera profile, restored previous config");
          return;
        }

        if (!wasActive) {
          shutdownCamera();
        }

        unlockCameraOp();
      }

    }

    framesize_t currentFrameSize = FRAMESIZE_VGA;
    getCameraStreamConfig(currentFrameSize);

    char json[64];
    snprintf(json, sizeof(json), "{\"frame\":\"%s\"}", frameSizeToText(currentFrameSize));
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
    getViewerStatusSnapshotForToken(viewerToken, busy, stale, active, lastSeenMs);
    char json[112];
    buildStatusJson(json, sizeof(json), busy, stale, active, lastSeenMs, getLedEnabledSnapshot());
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

void handleStreamRoot() {
  if (!streamServer.hasArg("stream")) {
    streamServer.send(400, "text/plain", "Missing stream parameter");
    return;
  }

  String viewerToken = streamServer.arg("viewer");
  if (viewerToken.length() == 0) {
    streamServer.send(423, "text/plain", "Ktos aktualnie korzysta z podgladu");
    return;
  }

  if (!lockCameraOp(pdMS_TO_TICKS(1500))) {
    streamServer.send(503, "text/plain", "Camera operation busy");
    return;
  }

  if (!lockState(pdMS_TO_TICKS(1000))) {
    unlockCameraOp();
    streamServer.send(503, "text/plain", "Stream state busy");
    return;
  }

  bool tokenActive = false;
  if (validateViewerSessionLocked()) {
    if (activeViewerToken == viewerToken) {
      tokenActive = true;
    } else if (isReservationActiveLocked(viewerToken)) {
      activeViewerToken = viewerToken;
      activeViewerIp = streamServer.client().remoteIP();
      activeViewerLastSeenMs = millis();
      tokenActive = true;
    }
  }
  if (!tokenActive) {
    if (isReservationActiveLocked(viewerToken)) {
      clearStreamReservationLocked();
    }
    unlockState();
    unlockCameraOp();
    streamServer.send(423, "text/plain", "Ktos aktualnie korzysta z podgladu");
    return;
  }

  bool needInit = !cameraActive.load(std::memory_order_relaxed);
  unlockState();

  if (needInit && !initCamera()) {
    if (lockState(pdMS_TO_TICKS(50))) {
      clearStreamReservationLocked();
      unlockState();
    }
    unlockCameraOp();
    streamServer.send(503, "text/plain", "Camera wakeup failed");
    return;
  }

  if (!warmupFirstFrameWithRecovery()) {
    // Non-fatal: continue and let stream loop try to recover frames.
    if (lockState(pdMS_TO_TICKS(50))) {
      clearStreamReservationLocked();
      unlockState();
    }
  }

  if (!lockState(pdMS_TO_TICKS(1000))) {
    unlockCameraOp();
    streamServer.send(503, "text/plain", "Stream state busy");
    return;
  }

  tokenActive = false;
  if (validateViewerSessionLocked()) {
    if (activeViewerToken == viewerToken) {
      tokenActive = true;
    } else if (isReservationActiveLocked(viewerToken)) {
      activeViewerToken = viewerToken;
      activeViewerIp = streamServer.client().remoteIP();
      activeViewerLastSeenMs = millis();
      tokenActive = true;
    }
  }
  if (!tokenActive) {
    if (isReservationActiveLocked(viewerToken)) {
      clearStreamReservationLocked();
    }
    unlockState();
    unlockCameraOp();
    streamServer.send(423, "text/plain", "Ktos aktualnie korzysta z podgladu");
    return;
  }

  unsigned long nowMs = millis();
  int activeLoops = streamLoopCount.load(std::memory_order_relaxed);
  if (activeLoops > 0) {
    unsigned long waitStartMs = millis();
    while (streamLoopCount.load(std::memory_order_relaxed) > 0 && (unsigned long)(millis() - waitStartMs) < 5000) {
      unlockState();
      unlockCameraOp();
      delay(10);
      if (!lockCameraOp(pdMS_TO_TICKS(200))) {
        streamServer.send(503, "text/plain", "Camera operation busy");
        return;
      }
      if (!lockState(pdMS_TO_TICKS(200))) {
        unlockCameraOp();
        streamServer.send(503, "text/plain", "Stream state busy");
        return;
      }
      if (!validateViewerSessionLocked() || activeViewerToken != viewerToken) {
        unlockState();
        unlockCameraOp();
        streamServer.send(423, "text/plain", "Ktos aktualnie korzysta z podgladu");
        return;
      }
    }

    activeLoops = streamLoopCount.load(std::memory_order_relaxed);
    if (activeLoops > 0) {
      activeViewerLastSeenMs = nowMs;
      unlockState();
      unlockCameraOp();
      streamServer.send(409, "text/plain", "Stream already active");
      return;
    }
  }

  cancelNoViewerPowerDownLocked();
  setPerformanceModeLocked(true);
  activeViewerLastSeenMs = nowMs;
  clearStreamReservationLocked();
  uint32_t myEpoch = streamOwnerEpoch.load(std::memory_order_relaxed);
  unlockState();
  unlockCameraOp();

  camera_fb_t *firstFrame = nullptr;
  auto tryGetFirstFrame = [&](unsigned long budgetMs) -> camera_fb_t * {
    camera_fb_t *fb = nullptr;
    unsigned long startMs = millis();
    while (fb == nullptr && (unsigned long)(millis() - startMs) < budgetMs) {
      fb = esp_camera_fb_get();
      if (fb == nullptr) {
        delay(16);
      }
    }
    return fb;
  };

  firstFrame = tryGetFirstFrame(6000);

  if (firstFrame == nullptr) {
    shutdownCamera();
    if (initCamera()) {
      firstFrame = tryGetFirstFrame(4000);
    }
  }

  if (firstFrame == nullptr) {
    framesize_t currentFrameSize = FRAMESIZE_VGA;
    getCameraStreamConfig(currentFrameSize);
    if (currentFrameSize != FRAMESIZE_QVGA) {
      shutdownCamera();
      setCameraStreamConfig(FRAMESIZE_QVGA);
      if (initCamera()) {
        firstFrame = tryGetFirstFrame(4000);
      }
    }
  }

  if (firstFrame == nullptr) {
    if (lockState(pdMS_TO_TICKS(80))) {
      if (activeViewerToken == viewerToken) {
        releaseViewerLock();
        activeViewerToken = "";
        clearStreamReservationLocked();
        scheduleNoViewerPowerDownLocked();
      }
      unlockState();
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
    delay(2);
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
      delay(0);
      continue;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      noFrameStreak++;
      if (noFrameStreak >= maxNoFrameStreak) {
        noFrameFailure = true;
        break;
      }
      delay(10);
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
      if (lockState(pdMS_TO_TICKS(1))) {
        activeViewerLastSeenMs = nowMs;
        unlockState();
      }
      lastKeepAliveMs = nowMs;
    }
    delay(0);
  }

  client.stop();

  if (lockState(pdMS_TO_TICKS(200))) {
    if (activeViewerToken == viewerToken && streamOwnerEpoch.load(std::memory_order_relaxed) == myEpoch) {
      releaseViewerLock();
      activeViewerToken = "";
      clearStreamReservationLocked();
      scheduleNoViewerPowerDownLocked();
      setPerformanceModeLocked(false);
    }
    unlockState();
  }
  streamLoopCount.fetch_sub(1, std::memory_order_relaxed);

  if (noFrameFailure) {
    int failures = consecutiveNoFrameFailures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (failures >= qualityDropNoFrameThreshold) {
      if (lockCameraOp(pdMS_TO_TICKS(400))) {
        if (streamLoopCount.load(std::memory_order_relaxed) == 0) {
          if (stepDownQualityIfNeeded()) {
            consecutiveNoFrameFailures.store(0, std::memory_order_relaxed);
          }
        }
        unlockCameraOp();
      }
    }
  }
}

void handleLedRoot() {
  ledServer.sendHeader("Cache-Control", "no-store");

  if (ledServer.hasArg("status")) {
    String viewerToken = ledServer.arg("viewer");
    bool busy = false;
    bool stale = false;
    bool active = false;
    unsigned long lastSeenMs = 0;
    getViewerStatusSnapshotForToken(viewerToken, busy, stale, active, lastSeenMs);
    char json[112];
    buildStatusJson(json, sizeof(json), busy, stale, active, lastSeenMs, getLedEnabledSnapshot());
    ledServer.send(200, "application/json", json);
    return;
  }

  if (!ledServer.hasArg("led")) {
    char json[20];
    buildLedJson(json, sizeof(json), getLedEnabledSnapshot());
    ledServer.send(200, "application/json", json);
    return;
  }

  String viewerToken = ledServer.arg("viewer");
  
  if (!lockState(pdMS_TO_TICKS(200))) {
    ledServer.send(503, "text/plain", "LED state busy");
    return;
  }

  bool allowed = (activeViewerIp[0] == 0);
  if (!allowed) {
    if ((unsigned long)(millis() - activeViewerLastSeenMs) > viewerHoldTimeoutMs) {
      releaseViewerLock();
      activeViewerToken = "";
      scheduleNoViewerPowerDownLocked();
      allowed = true;
    } else {
      allowed = (activeViewerToken == viewerToken);
    }
  }

  if (!allowed) {
    unlockState();
    ledServer.send(423, "text/plain", "Ktos aktualnie korzysta z podgladu");
    return;
  }

  String cmd = ledServer.arg("led");
  if (cmd == "on") {
    if (!ledEnabled.load(std::memory_order_relaxed)) {
      setFlashLed(true);
      ledAutoOffAtMs = millis() + ledAutoOffTimeoutMs;
    }
  } else if (cmd == "off") {
    setFlashLed(false);
    ledAutoOffAtMs = 0;
  } else if (cmd == "toggle") {
    bool nextState = !ledEnabled.load(std::memory_order_relaxed);
    setFlashLed(nextState);
    ledAutoOffAtMs = nextState ? (millis() + ledAutoOffTimeoutMs) : 0;
  }

  unlockState();

  char json[20];
  buildLedJson(json, sizeof(json), getLedEnabledSnapshot());
  ledServer.send(200, "application/json", json);
}

void startWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/assets/style.css", HTTP_GET, handleRoot);
  server.on("/assets/app.js", HTTP_GET, handleRoot);
  server.begin();

  ledServer.on("/", HTTP_GET, handleLedRoot);
  ledServer.begin();

  streamServer.on("/", HTTP_GET, handleStreamRoot);
  streamServer.begin();
}

void uiServerTask(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    server.handleClient();
    vTaskDelay(isHighPerformanceModeSnapshot() ? serverTaskDelayActiveTicks : serverTaskDelayIdleTicks);
  }
}

void ledServerTask(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    ledServer.handleClient();
    vTaskDelay(isHighPerformanceModeSnapshot() ? serverTaskDelayActiveTicks : serverTaskDelayIdleTicks);
  }
}

void streamServerTask(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    streamServer.handleClient();
    vTaskDelay(isHighPerformanceModeSnapshot() ? serverTaskDelayActiveTicks : serverTaskDelayIdleTicks);
  }
}

void failSetupAndRestart() {
  setFlashLed(true);
  delay(250);
  setFlashLed(false);
  delay(1500);
  ESP.restart();
}

void runHousekeepingTick() {
  if (!lockState(pdMS_TO_TICKS(20))) {
    return;
  }

  if (ledEnabled.load(std::memory_order_relaxed) && ledAutoOffAtMs != 0) {
    if ((long)(millis() - ledAutoOffAtMs) >= 0) {
      setFlashLed(false);
      ledAutoOffAtMs = 0;
    }
  }

  unlockState();
}

void setup() {
  delay(1000);

  pinMode(FLASH_LED_GPIO_NUM, OUTPUT);
  setFlashLed(false);

  WiFi.mode(WIFI_STA);

  WiFiManager wm;
  wm.setConfigPortalBlocking(true);
  wm.setConfigPortalTimeout(180);

  bool ok = wm.autoConnect("ESP32CAM-Setup");
  if (!ok) {
    delay(3000);
    ESP.restart();
  }

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  stateMutex = xSemaphoreCreateMutex();
  cameraOpMutex = xSemaphoreCreateMutex();
  if (stateMutex == nullptr || cameraOpMutex == nullptr) {
    failSetupAndRestart();
  }

  if (!initCamera()) {
    blinkCameraInitErrorPattern();
  } else if (enableCameraIdlePowerDown) {
    shutdownCamera();
  }

  if (lockState(pdMS_TO_TICKS(200))) {
    setPerformanceModeLocked(false);
    unlockState();
  }

  startWebServer();

  streamActivityHeartbeatMs.store(millis(), std::memory_order_relaxed);

  BaseType_t uiTaskOk = xTaskCreatePinnedToCore(uiServerTask, "uiServerTask", 4096, nullptr, 1, nullptr, controlCore);
  BaseType_t ledTaskOk = xTaskCreatePinnedToCore(ledServerTask, "ledServerTask", 4096, nullptr, 1, nullptr, controlCore);
  BaseType_t streamTaskOk = xTaskCreatePinnedToCore(streamServerTask, "streamServerTask", 6144, nullptr, 2, nullptr, streamCore);
  if (uiTaskOk != pdPASS || ledTaskOk != pdPASS || streamTaskOk != pdPASS) {
    failSetupAndRestart();
  }
}

void loop() {
  runHousekeepingTick();
  vTaskDelay(pdMS_TO_TICKS(250));
}
