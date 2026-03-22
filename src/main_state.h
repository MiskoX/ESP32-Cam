#pragma once

#include <WebServer.h>
#include <WString.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <WiFi.h>

extern WebServer server;
extern WebServer ledServer;
extern WebServer streamServer;

extern SemaphoreHandle_t stateMutex;
extern SemaphoreHandle_t cameraOpMutex;

extern unsigned long cameraShutdownAtMs;
extern unsigned long ledAutoOffAtMs;
extern unsigned long lastOwnerSwitchMs;
extern unsigned long lastOwnerSwitchCooldownMs;
extern String activeViewerToken;
extern String reservedViewerToken;
extern unsigned long reservedViewerUntilMs;

extern std::atomic<bool> highPerformanceMode;
extern std::atomic<int> streamLoopCount;
extern std::atomic<uint32_t> streamOwnerEpoch;
extern std::atomic<unsigned long> streamActivityHeartbeatMs;
extern std::atomic<int> consecutiveNoFrameFailures;

constexpr unsigned long kCameraIdleShutdownMs = 10000;
constexpr bool kEnableCameraIdlePowerDown = true;
constexpr unsigned long kLedAutoOffTimeoutMs = 15000;
constexpr uint32_t kCpuFreqActiveMHz = 240;
constexpr uint32_t kCpuFreqIdleMHz = 160;
const TickType_t kServerTaskDelayActiveTicks = pdMS_TO_TICKS(4);
const TickType_t kServerTaskDelayIdleTicks = pdMS_TO_TICKS(14);

constexpr unsigned long kOwnerSwitchBaseCooldownMs = 700;
constexpr unsigned long kOwnerSwitchJitterMaxMs = 900;
constexpr unsigned long kOwnerHandoffWaitMs = 1400;
constexpr unsigned long kStreamStartReservationMs = 9000;
constexpr unsigned long kFirstFrameWarmupBudgetMs = 1800;
constexpr int kFirstFrameWarmupAttempts = 4;
constexpr int kQualityDropNoFrameThreshold = 2;

#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
constexpr BaseType_t kControlCore = ARDUINO_RUNNING_CORE;
constexpr BaseType_t kStreamCore = ARDUINO_RUNNING_CORE;
#else
constexpr BaseType_t kControlCore = 0;
constexpr BaseType_t kStreamCore = 1;
#endif
