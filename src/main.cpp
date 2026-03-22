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
  if (!sessionLockState(pdMS_TO_TICKS(20))) {
    return;
  }

  if (ledEnabled.load(std::memory_order_relaxed) && ledAutoOffAtMs != 0) {
    if ((long)(millis() - ledAutoOffAtMs) >= 0) {
      setFlashLed(false);
      ledAutoOffAtMs = 0;
    }
  }

  sessionUnlockState();
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
  } else if (kEnableCameraIdlePowerDown) {
    shutdownCamera();
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
