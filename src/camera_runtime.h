#pragma once

#include <Arduino.h>
#include <atomic>
#include <WiFi.h>
#include "esp_camera.h"

constexpr uint8_t FLASH_LED_GPIO_NUM = 4;

extern IPAddress activeViewerIp;
extern unsigned long activeViewerLastSeenMs;
extern const unsigned long viewerHoldTimeoutMs;
extern std::atomic<bool> cameraActive;
extern std::atomic<bool> ledEnabled;

void setFlashLed(bool on);
void blinkCameraInitErrorPattern();
void releaseViewerLock();
bool initCamera();
void shutdownCamera();
void setCameraStreamConfig(framesize_t frameSize);
void getCameraStreamConfig(framesize_t &frameSize);
