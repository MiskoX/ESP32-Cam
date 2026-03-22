#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <esp32-hal-cpu.h>
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
unsigned long ledAutoOffAtMs = 0;
const unsigned long ledAutoOffTimeoutMs = 15000;
String activeViewerToken;
const uint32_t cpuFreqActiveMHz = 240;
const uint32_t cpuFreqIdleMHz = 160;
std::atomic<bool> highPerformanceMode{false};
std::atomic<int> streamLoopCount{0};
std::atomic<uint32_t> streamOwnerEpoch{0};
const TickType_t serverTaskDelayActiveTicks = pdMS_TO_TICKS(4);
const TickType_t serverTaskDelayIdleTicks = pdMS_TO_TICKS(14);

#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
const BaseType_t uiLedCore = ARDUINO_RUNNING_CORE;
const BaseType_t streamCore = ARDUINO_RUNNING_CORE;
const BaseType_t housekeepingCore = ARDUINO_RUNNING_CORE;
#else
const BaseType_t uiLedCore = ARDUINO_RUNNING_CORE;
const BaseType_t streamCore = (ARDUINO_RUNNING_CORE == 0) ? 1 : 0;
const BaseType_t housekeepingCore = (ARDUINO_RUNNING_CORE == 0) ? 1 : 0;
#endif

void scheduleNoViewerPowerDownLocked();

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

void buildStatusJson(char *out, size_t outSize, bool busy, bool ledOn) {
  snprintf(out, outSize, "{\"busy\":%s,\"led\":\"%s\"}", busy ? "true" : "false", ledOn ? "on" : "off");
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
    scheduleNoViewerPowerDownLocked();
    return false;
  }

  return true;
}

void scheduleNoViewerPowerDownLocked() {
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

bool isViewerBusyForToken(const String &viewerToken) {
  if (!lockState()) {
    return true;
  }

  bool busy = false;
  if (validateViewerSessionLocked()) {
    busy = (activeViewerToken != viewerToken);
  }

  unlockState();
  return busy;
}

void handleRoot() {
  if (server.hasArg("takeover")) {
    String viewerToken = server.arg("viewer");
    if (viewerToken.length() == 0) {
      server.send(400, "text/plain", "Missing viewer token");
      return;
    }

    if (!lockState(pdMS_TO_TICKS(500))) {
      server.send(503, "text/plain", "State busy");
      return;
    }

    IPAddress remote = server.client().remoteIP();
    bool ownerChanged = (activeViewerToken != viewerToken);
    activeViewerIp = remote;
    activeViewerToken = viewerToken;
    activeViewerLastSeenMs = millis();
    cancelNoViewerPowerDownLocked();
    setPerformanceModeLocked(true);
    if (ownerChanged) {
      streamOwnerEpoch.fetch_add(1, std::memory_order_relaxed);
    }
    unlockState();

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"ok\":true}");
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
    bool busy = isViewerBusyForToken(viewerToken);
    char json[40];
    buildStatusJson(json, sizeof(json), busy, getLedEnabledSnapshot());
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

  bool needInit = !cameraActive.load(std::memory_order_relaxed);
  unlockState();

  if (needInit && !initCamera()) {
    unlockCameraOp();
    streamServer.send(503, "text/plain", "Camera wakeup failed");
    return;
  }

  if (!lockState(pdMS_TO_TICKS(1000))) {
    unlockCameraOp();
    streamServer.send(503, "text/plain", "Stream state busy");
    return;
  }

  bool ownerChanged = (activeViewerToken != viewerToken);
  int activeLoops = streamLoopCount.load(std::memory_order_relaxed);
  if (!ownerChanged && activeLoops > 0) {
    activeViewerLastSeenMs = millis();
    unlockState();
    unlockCameraOp();
    streamServer.send(409, "text/plain", "Stream already active");
    return;
  }

  cancelNoViewerPowerDownLocked();
  setPerformanceModeLocked(true);
  IPAddress remote = streamServer.client().remoteIP();
  activeViewerIp = remote;
  activeViewerToken = viewerToken;
  activeViewerLastSeenMs = millis();
  uint32_t myEpoch = ownerChanged
    ? (streamOwnerEpoch.fetch_add(1, std::memory_order_relaxed) + 1)
    : streamOwnerEpoch.load(std::memory_order_relaxed);
  unlockState();
  unlockCameraOp();

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
  const int maxNoFrameStreak = 30;
  streamLoopCount.fetch_add(1, std::memory_order_relaxed);

  unsigned long handoffStartMs = millis();
  while (streamLoopCount.load(std::memory_order_relaxed) > 1 && (unsigned long)(millis() - handoffStartMs) < 2500) {
    delay(2);
  }
  if (streamLoopCount.load(std::memory_order_relaxed) > 1) {
    streamLoopCount.fetch_sub(1, std::memory_order_relaxed);
    client.stop();
    return;
  }
  
  while (client.connected()) {
    if (streamOwnerEpoch.load(std::memory_order_relaxed) != myEpoch) {
      break;
    }

    if (skippedFrames > 0) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        esp_camera_fb_return(fb);
        skippedFrames--;
        noFrameStreak = 0;
      } else {
        noFrameStreak++;
        if (noFrameStreak >= maxNoFrameStreak) {
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
        break;
      }
      delay(10);
      continue;
    }
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

  if (lockState(pdMS_TO_TICKS(200))) {
    if (activeViewerToken == viewerToken && streamOwnerEpoch.load(std::memory_order_relaxed) == myEpoch) {
      releaseViewerLock();
      activeViewerToken = "";
      scheduleNoViewerPowerDownLocked();
      setPerformanceModeLocked(false);
    }
    unlockState();
  }
  streamLoopCount.fetch_sub(1, std::memory_order_relaxed);
}

void handleLedRoot() {
  ledServer.sendHeader("Cache-Control", "no-store");

  if (ledServer.hasArg("status")) {
    String viewerToken = ledServer.arg("viewer");
    bool busy = isViewerBusyForToken(viewerToken);
    char json[40];
    buildStatusJson(json, sizeof(json), busy, getLedEnabledSnapshot());
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

void housekeepingTask(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    bool shouldShutdown = false;
    bool shouldTurnOffLedByTimeout = false;

    if (lockState()) {
      bool hasViewer = hasActiveViewerSessionLocked();

      if (cameraActive.load(std::memory_order_relaxed) && cameraShutdownAtMs != 0 && activeViewerIp[0] == 0) {
        if ((long)(millis() - cameraShutdownAtMs) >= 0) {
          shouldShutdown = true;
          cameraShutdownAtMs = 0;
        }
      }

      if (ledEnabled.load(std::memory_order_relaxed) && ledAutoOffAtMs != 0) {
        if ((long)(millis() - ledAutoOffAtMs) >= 0) {
          shouldTurnOffLedByTimeout = true;
          ledAutoOffAtMs = 0;
        }
      }

      if (highPerformanceMode.load(std::memory_order_relaxed) && !hasViewer) {
        setPerformanceModeLocked(false);
      }
      unlockState();
    }

    if (shouldTurnOffLedByTimeout) {
      if (lockState(pdMS_TO_TICKS(200))) {
        setFlashLed(false);
        unlockState();
      }
    }

    if (shouldShutdown) {
      if (lockCameraOp(pdMS_TO_TICKS(1000))) {
        if (lockState(pdMS_TO_TICKS(1000))) {
          if (!hasActiveViewerSessionLocked() && cameraActive.load(std::memory_order_relaxed)) {
            shutdownCamera();
            setPerformanceModeLocked(false);
          }
          unlockState();
        }
        unlockCameraOp();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void failSetupAndRestart() {
  setFlashLed(true);
  delay(250);
  setFlashLed(false);
  delay(1500);
  ESP.restart();
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

  stateMutex = xSemaphoreCreateMutex();
  cameraOpMutex = xSemaphoreCreateMutex();
  if (stateMutex == nullptr || cameraOpMutex == nullptr) {
    failSetupAndRestart();
  }

  if (!initCamera()) {
    blinkCameraInitErrorPattern();
  } else {
    shutdownCamera();
  }

  if (lockState(pdMS_TO_TICKS(200))) {
    setPerformanceModeLocked(false);
    unlockState();
  }

  startWebServer();

  BaseType_t uiTaskOk = xTaskCreatePinnedToCore(uiServerTask, "uiServerTask", 4096, nullptr, 1, nullptr, uiLedCore);
  BaseType_t ledTaskOk = xTaskCreatePinnedToCore(ledServerTask, "ledServerTask", 4096, nullptr, 1, nullptr, uiLedCore);
  BaseType_t streamTaskOk = xTaskCreatePinnedToCore(streamServerTask, "streamServerTask", 6144, nullptr, 2, nullptr, streamCore);
  BaseType_t housekeepingTaskOk = xTaskCreatePinnedToCore(housekeepingTask, "housekeepingTask", 3072, nullptr, 1, nullptr, housekeepingCore);
  if (uiTaskOk != pdPASS || ledTaskOk != pdPASS || streamTaskOk != pdPASS || housekeepingTaskOk != pdPASS) {
    failSetupAndRestart();
  }
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
