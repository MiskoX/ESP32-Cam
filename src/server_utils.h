#pragma once

#include <Arduino.h>
#include "esp_camera.h"

unsigned long sessionNextOwnerSwitchCooldownMs();

void sessionClearStreamReservationLocked();
void sessionReserveViewerForStartLocked(const String &viewerToken);
bool sessionIsReservationActiveLocked(const String &viewerToken);

framesize_t streamLowerFrameSize(framesize_t current);
bool streamStepDownQualityIfNeeded();
bool streamWarmupFirstFrameWithRecovery();

bool sessionLockState(TickType_t timeoutTicks = pdMS_TO_TICKS(50));
void sessionUnlockState();
bool streamLockCameraOp(TickType_t timeoutTicks = pdMS_TO_TICKS(50));
void streamUnlockCameraOp();

bool ledGetEnabledSnapshot();
void serverBuildStatusJson(char *out, size_t outSize, bool busy, bool stale, bool active, unsigned long lastSeenMs, bool ledOn);
void ledBuildJson(char *out, size_t outSize, bool ledOn);

const char *streamFrameSizeToText(framesize_t frameSize);
bool streamParseFrameSizeText(const String &value, framesize_t &frameSize);

void serverSetPerformanceModeLocked(bool active);
bool serverIsHighPerformanceModeSnapshot();

bool sessionHasActiveViewerLocked();
void sessionScheduleNoViewerPowerDownLocked();
void sessionCancelNoViewerPowerDownLocked();
bool sessionValidateViewerLocked();
bool sessionIsViewerTokenAllowed(const String &viewerToken);
void sessionGetViewerStatusForToken(const String &viewerToken, bool &busy, bool &stale, bool &active, unsigned long &lastSeenMs);
