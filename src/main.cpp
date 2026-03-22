#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>

#include "camera_runtime.h"
#include "main_state.h"
#include "server_handlers.h"
#include "server_utils.h"

WebServer server(80);
WebServer ledServer(81);
WebServer streamServer(82);

SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t cameraOpMutex = nullptr;
unsigned long cameraShutdownAtMs = 0;
unsigned long ledAutoOffAtMs = 0;
unsigned long lastOwnerSwitchMs = 0;
unsigned long lastOwnerSwitchCooldownMs = 1200;
String activeViewerToken;
String reservedViewerToken;
unsigned long reservedViewerUntilMs = 0;
std::atomic<bool> highPerformanceMode{false};
std::atomic<int> streamLoopCount{0};
std::atomic<uint32_t> streamOwnerEpoch{0};
std::atomic<unsigned long> streamActivityHeartbeatMs{0};
std::atomic<int> consecutiveNoFrameFailures{0};

constexpr unsigned long kWifiHealthCheckPeriodMs = 1000;
constexpr unsigned long kWifiReconnectIntervalMs = 5000;
unsigned long wifiLastHealthCheckMs = 0;
unsigned long wifiLastReconnectAttemptMs = 0;

void runWifiWatchdogTick() {
  unsigned long nowMs = millis();
  if ((unsigned long)(nowMs - wifiLastHealthCheckMs) < kWifiHealthCheckPeriodMs) {
    return;
  }
  wifiLastHealthCheckMs = nowMs;

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (wifiLastReconnectAttemptMs == 0 || (unsigned long)(nowMs - wifiLastReconnectAttemptMs) >= kWifiReconnectIntervalMs) {
    WiFi.reconnect();
    wifiLastReconnectAttemptMs = nowMs;
  }
}

void uiServerTask(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    server.handleClient();
    vTaskDelay(serverIsHighPerformanceModeSnapshot() ? kServerTaskDelayActiveTicks : kServerTaskDelayIdleTicks);
  }
}

void ledServerTask(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    ledServer.handleClient();
    vTaskDelay(serverIsHighPerformanceModeSnapshot() ? kServerTaskDelayActiveTicks : kServerTaskDelayIdleTicks);
  }
}

void streamServerTask(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    streamServer.handleClient();
    vTaskDelay(serverIsHighPerformanceModeSnapshot() ? kServerTaskDelayActiveTicks : kServerTaskDelayIdleTicks);
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
  runWifiWatchdogTick();

  bool shouldAttemptCameraShutdown = false;

  if (!sessionLockState(pdMS_TO_TICKS(20))) {
    return;
  }

  if (ledEnabled.load(std::memory_order_relaxed) && ledAutoOffAtMs != 0) {
    if ((long)(millis() - ledAutoOffAtMs) >= 0) {
      setFlashLed(false);
      ledAutoOffAtMs = 0;
    }
  }

  if (kEnableCameraIdlePowerDown && cameraShutdownAtMs != 0) {
    if ((long)(millis() - cameraShutdownAtMs) >= 0) {
      shouldAttemptCameraShutdown = true;
    }
  }

  sessionUnlockState();

  if (!shouldAttemptCameraShutdown) {
    return;
  }

  if (!streamLockCameraOp(pdMS_TO_TICKS(20))) {
    return;
  }

  bool shouldShutdownNow = false;
  if (sessionLockState(pdMS_TO_TICKS(20))) {
    // Keep lock order cameraOp -> state to avoid deadlocks with stream startup path.
    bool hasViewer = sessionValidateViewerLocked();
    bool hasRunningStream = (streamLoopCount.load(std::memory_order_relaxed) > 0);
    if (!hasViewer && !hasRunningStream && cameraShutdownAtMs != 0 && (long)(millis() - cameraShutdownAtMs) >= 0) {
      cameraShutdownAtMs = 0;
      ledAutoOffAtMs = 0;
      shouldShutdownNow = true;
    }
    sessionUnlockState();
  }

  if (shouldShutdownNow) {
    if (ledEnabled.load(std::memory_order_relaxed)) {
      setFlashLed(false);
    }
    if (cameraActive.load(std::memory_order_relaxed)) {
      shutdownCamera();
    }
  }

  streamUnlockCameraOp();
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
  wifiLastHealthCheckMs = millis();
  wifiLastReconnectAttemptMs = 0;

  stateMutex = xSemaphoreCreateMutex();
  cameraOpMutex = xSemaphoreCreateMutex();
  if (stateMutex == nullptr || cameraOpMutex == nullptr) {
    failSetupAndRestart();
  }

  if (!initCamera()) {
    blinkCameraInitErrorPattern();
  } else if (kEnableCameraIdlePowerDown) {
    if (sessionLockState(pdMS_TO_TICKS(200))) {
      // Start with camera active, then power it down after idle timeout if nobody watches.
      sessionScheduleNoViewerPowerDownLocked();
      sessionUnlockState();
    }
  }

  if (sessionLockState(pdMS_TO_TICKS(200))) {
    serverSetPerformanceModeLocked(false);
    sessionUnlockState();
  }

  serverStartWeb();

  streamActivityHeartbeatMs.store(millis(), std::memory_order_relaxed);

  BaseType_t uiTaskOk = xTaskCreatePinnedToCore(uiServerTask, "uiServerTask", 4096, nullptr, 1, nullptr, kControlCore);
  BaseType_t ledTaskOk = xTaskCreatePinnedToCore(ledServerTask, "ledServerTask", 4096, nullptr, 1, nullptr, kControlCore);
  BaseType_t streamTaskOk = xTaskCreatePinnedToCore(streamServerTask, "streamServerTask", 6144, nullptr, 2, nullptr, kStreamCore);
  if (uiTaskOk != pdPASS || ledTaskOk != pdPASS || streamTaskOk != pdPASS) {
    failSetupAndRestart();
  }
}

void loop() {
  runHousekeepingTick();
  vTaskDelay(pdMS_TO_TICKS(250));
}
