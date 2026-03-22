#include "server_utils.h"

#include <esp32-hal-cpu.h>
#include <esp_system.h>
#include <stdio.h>

#include "camera_runtime.h"
#include "main_state.h"

unsigned long sessionNextOwnerSwitchCooldownMs() {
  uint32_t jitterMs = esp_random() % kOwnerSwitchJitterMaxMs;
  return kOwnerSwitchBaseCooldownMs + jitterMs;
}

void sessionClearStreamReservationLocked() {
  reservedViewerToken = "";
  reservedViewerUntilMs = 0;
}

void sessionReserveViewerForStartLocked(const String &viewerToken) {
  reservedViewerToken = viewerToken;
  reservedViewerUntilMs = millis() + kStreamStartReservationMs;
}

bool sessionIsReservationActiveLocked(const String &viewerToken) {
  if (reservedViewerToken.length() == 0 || reservedViewerToken != viewerToken) {
    return false;
  }
  if ((long)(millis() - reservedViewerUntilMs) > 0) {
    sessionClearStreamReservationLocked();
    return false;
  }
  return true;
}

framesize_t streamLowerFrameSize(framesize_t current) {
  if (current == FRAMESIZE_SVGA) {
    return FRAMESIZE_VGA;
  }
  if (current == FRAMESIZE_VGA) {
    return FRAMESIZE_QVGA;
  }
  return FRAMESIZE_QVGA;
}

bool streamStepDownQualityIfNeeded() {
  framesize_t currentFrameSize = FRAMESIZE_VGA;
  getCameraStreamConfig(currentFrameSize);
  framesize_t nextFrameSize = streamLowerFrameSize(currentFrameSize);
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

bool streamWarmupFirstFrameWithRecovery() {
  for (int attempt = 0; attempt < kFirstFrameWarmupAttempts; ++attempt) {
    unsigned long startMs = millis();
    while ((unsigned long)(millis() - startMs) < kFirstFrameWarmupBudgetMs) {
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
  if (failures >= kQualityDropNoFrameThreshold) {
    if (streamStepDownQualityIfNeeded()) {
      consecutiveNoFrameFailures.store(0, std::memory_order_relaxed);
    }
  }
  return false;
}

bool sessionLockState(TickType_t timeoutTicks) {
  if (stateMutex == nullptr) {
    return true;
  }
  return xSemaphoreTake(stateMutex, timeoutTicks) == pdTRUE;
}

void sessionUnlockState() {
  if (stateMutex != nullptr) {
    xSemaphoreGive(stateMutex);
  }
}

bool streamLockCameraOp(TickType_t timeoutTicks) {
  if (cameraOpMutex == nullptr) {
    return true;
  }
  return xSemaphoreTake(cameraOpMutex, timeoutTicks) == pdTRUE;
}

void streamUnlockCameraOp() {
  if (cameraOpMutex != nullptr) {
    xSemaphoreGive(cameraOpMutex);
  }
}

bool ledGetEnabledSnapshot() {
  return ledEnabled.load(std::memory_order_relaxed);
}

void serverBuildStatusJson(char *out, size_t outSize, bool busy, bool stale, bool active, unsigned long lastSeenMs, bool ledOn) {
  snprintf(out, outSize, "{\"busy\":%s,\"stale\":%s,\"active\":%s,\"lastSeenMs\":%lu,\"led\":\"%s\"}",
           busy ? "true" : "false", stale ? "true" : "false", active ? "true" : "false", lastSeenMs, ledOn ? "on" : "off");
}

void ledBuildJson(char *out, size_t outSize, bool ledOn) {
  snprintf(out, outSize, "{\"led\":\"%s\"}", ledOn ? "on" : "off");
}

const char *streamFrameSizeToText(framesize_t frameSize) {
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

bool streamParseFrameSizeText(const String &value, framesize_t &frameSize) {
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

void serverSetPerformanceModeLocked(bool active) {
  if (highPerformanceMode.load(std::memory_order_relaxed) == active) {
    return;
  }

  setCpuFrequencyMhz(active ? kCpuFreqActiveMHz : kCpuFreqIdleMHz);
  highPerformanceMode.store(active, std::memory_order_relaxed);
}

bool serverIsHighPerformanceModeSnapshot() {
  return highPerformanceMode.load(std::memory_order_relaxed);
}

bool sessionHasActiveViewerLocked() {
  if (activeViewerIp[0] == 0) {
    return false;
  }

  if ((unsigned long)(millis() - activeViewerLastSeenMs) > viewerHoldTimeoutMs) {
    releaseViewerLock();
    activeViewerToken = "";
    sessionClearStreamReservationLocked();
    sessionScheduleNoViewerPowerDownLocked();
    return false;
  }

  return true;
}

void sessionScheduleNoViewerPowerDownLocked() {
  if (!kEnableCameraIdlePowerDown) {
    cameraShutdownAtMs = 0;
    return;
  }

  const unsigned long deadlineMs = millis() + kCameraIdleShutdownMs;

  if (cameraActive.load(std::memory_order_relaxed) && cameraShutdownAtMs == 0) {
    cameraShutdownAtMs = deadlineMs;
  }

  if (ledEnabled.load(std::memory_order_relaxed)) {
    if (ledAutoOffAtMs == 0 || (long)(deadlineMs - ledAutoOffAtMs) < 0) {
      ledAutoOffAtMs = deadlineMs;
    }
  }
}

void sessionCancelNoViewerPowerDownLocked() {
  cameraShutdownAtMs = 0;
}

bool sessionValidateViewerLocked() {
  if (activeViewerIp[0] == 0) {
    return false;
  }

  if ((unsigned long)(millis() - activeViewerLastSeenMs) > viewerHoldTimeoutMs) {
    releaseViewerLock();
    activeViewerToken = "";
    sessionClearStreamReservationLocked();
    sessionScheduleNoViewerPowerDownLocked();
    return false;
  }

  return true;
}

bool sessionIsViewerTokenAllowed(const String &viewerToken) {
  if (viewerToken.length() == 0) {
    return false;
  }

  if (!sessionLockState()) {
    return false;
  }

  bool allowed = true;
  if (sessionValidateViewerLocked()) {
    allowed = (activeViewerToken == viewerToken);
  }

  sessionUnlockState();
  return allowed;
}

void sessionGetViewerStatusForToken(const String &viewerToken, bool &busy, bool &stale, bool &active, unsigned long &lastSeenMs) {
  busy = true;
  stale = false;
  active = false;
  lastSeenMs = 0;

  if (!sessionLockState()) {
    return;
  }

  busy = false;
  if (sessionValidateViewerLocked()) {
    active = (activeViewerToken == viewerToken);
    unsigned long streamBeatMs = streamActivityHeartbeatMs.load(std::memory_order_relaxed);
    unsigned long latestSeenMs = (streamBeatMs > activeViewerLastSeenMs) ? streamBeatMs : activeViewerLastSeenMs;
    lastSeenMs = (latestSeenMs == 0) ? 0 : (unsigned long)(millis() - latestSeenMs);
    stale = (lastSeenMs > 2500);
    bool hasRunningStream = (streamLoopCount.load(std::memory_order_relaxed) > 0);
    busy = (!active && !stale && hasRunningStream);
  }

  sessionUnlockState();
}
