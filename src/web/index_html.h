#pragma once

#include <Arduino.h>

static const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="pl">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>ESP32-CAM</title>
  <link rel="stylesheet" href="/assets/style.css?v=13" />
</head>
<body>
  <div class="wrap">
    <div class="preview">
      <img id="cam" alt="Podgląd z ESP32-CAM" />
      <div id="overlay" class="overlay" aria-live="polite">
        <div id="loader" class="loader" aria-hidden="true"></div>
        <p id="overlayMessage" class="overlayMessage"></p>
      </div>
      <button id="exitFullscreenBtn" class="exitFullscreenBtn" type="button" aria-label="Wyjdź z pełnego ekranu">Wyjdź z pełnego ekranu</button>
    </div>
    <p id="status"></p>
    <div class="toolbar">
      <button id="ledBtn" type="button">Włącz/wyłącz LED</button>
      <button id="fullscreenBtn" type="button">Pełny ekran</button>
      <label for="frameSizeSel" class="frameLabel">Rozdzielczość</label>
      <select id="frameSizeSel">
        <option value="qvga">Niska</option>
        <option value="vga" selected>Średnia</option>
        <option value="svga">Wysoka</option>
      </select>
      <button id="applyCamBtn" type="button">Zastosuj obraz</button>
    </div>
  </div>
  <script src="/assets/app.js?v=17"></script>
</body>
</html>
)HTML";
