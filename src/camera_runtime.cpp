#include "camera_runtime.h"

#include "server_utils.h"

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

IPAddress activeViewerIp(0, 0, 0, 0);
unsigned long activeViewerLastSeenMs = 0;
const unsigned long viewerHoldTimeoutMs = 5000;
std::atomic<bool> cameraActive{false};
std::atomic<bool> ledEnabled{false};

namespace {
constexpr uint8_t kFlashLedPwmChannel = 15;
constexpr uint32_t kFlashLedPwmFreqHz = 5000;
constexpr uint8_t kFlashLedPwmResolutionBits = 8;
constexpr uint32_t kFlashLedDutyOn = 128;
constexpr uint32_t kFlashLedDutyShutdownBlink = (255 * 15) / 100;
constexpr int kCameraStartBlinkCount = 3;
constexpr int kCameraStopBlinkCount = 2;
constexpr unsigned long kCameraBlinkOnMs = 90;
constexpr unsigned long kCameraBlinkOffMs = 90;

bool flashPwmInitialized = false;
framesize_t configuredFrameSize = FRAMESIZE_VGA;
constexpr int kFixedJpegQuality = 15;
constexpr int kFixedFbCount = 3;

void ensureFlashPwmInit() {
  if (flashPwmInitialized) {
    return;
  }

  ledcSetup(kFlashLedPwmChannel, kFlashLedPwmFreqHz, kFlashLedPwmResolutionBits);
  ledcAttachPin(FLASH_LED_GPIO_NUM, kFlashLedPwmChannel);
  ledcWrite(kFlashLedPwmChannel, 0);
  flashPwmInitialized = true;
}

void blinkFlashPattern(int count, uint32_t onDuty) {
  const bool previousLedState = ledEnabled.load(std::memory_order_relaxed);
  for (int i = 0; i < count; ++i) {
    ledEnabled.store(true, std::memory_order_relaxed);
    ledcWrite(kFlashLedPwmChannel, onDuty);
    delay(kCameraBlinkOnMs);
    ledEnabled.store(false, std::memory_order_relaxed);
    ledcWrite(kFlashLedPwmChannel, 0);
    delay(kCameraBlinkOffMs);
  }
  setFlashLed(previousLedState);
}
}

void setFlashLed(bool on) {
  ensureFlashPwmInit();
  const bool targetState = on;
  ledEnabled.store(targetState, std::memory_order_relaxed);
  ledcWrite(kFlashLedPwmChannel, targetState ? kFlashLedDutyOn : 0);
}

void blinkCameraInitErrorPattern() {
  for (int i = 0; i < 2; ++i) {
    setFlashLed(true);
    delay(250);
    setFlashLed(false);
    delay(250);
  }
  delay(5000);
}

void releaseViewerLock() {
  activeViewerIp = IPAddress(0, 0, 0, 0);
  activeViewerLastSeenMs = 0;
}

bool initCamera() {
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(20);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;

  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.frame_size = configuredFrameSize;
  config.jpeg_quality = kFixedJpegQuality;
  config.fb_count = (configuredFrameSize == FRAMESIZE_SVGA) ? 2 : kFixedFbCount;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    cameraActive.store(false, std::memory_order_relaxed);
    return false;
  }

  cameraActive.store(true, std::memory_order_relaxed);
  serverSetPerformanceModeLocked(true);

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 0);
    s->set_saturation(s, -1);
  }

  blinkFlashPattern(kCameraStartBlinkCount, kFlashLedDutyOn);

  return true;
}

void shutdownCamera() {
  if (!cameraActive) {
    return;
  }

  blinkFlashPattern(kCameraStopBlinkCount, kFlashLedDutyShutdownBlink);

  esp_camera_deinit();
  cameraActive.store(false, std::memory_order_relaxed);
  serverSetPerformanceModeLocked(false);
  delay(10);

  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  delay(10);
}

void setCameraStreamConfig(framesize_t frameSize) {
  configuredFrameSize = frameSize;
}

void getCameraStreamConfig(framesize_t &frameSize) {
  frameSize = configuredFrameSize;
}
