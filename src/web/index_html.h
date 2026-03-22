#pragma once

#include <Arduino.h>

static const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="pl">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title></title>
  <link rel="stylesheet" href="/assets/style.css?v=13" />
</head>
<body>
  <div class="wrap">
    <div class="preview">
      <img id="cam" alt="" />
      <div id="overlay" class="overlay" aria-live="polite">
        <div id="loader" class="loader" aria-hidden="true"></div>
        <p id="overlayMessage" class="overlayMessage"></p>
        <button id="takeoverBtn" class="takeoverBtn" type="button"></button>
      </div>
      <button id="exitFullscreenBtn" class="exitFullscreenBtn" type="button" aria-label=""></button>
    </div>
    <p id="status"></p>
    <div class="toolbar">
      <button id="langBtn" type="button"></button>
      <button id="ledBtn" type="button"></button>
      <button id="fullscreenBtn" type="button"></button>
      <label for="frameSizeSel" class="frameLabel"></label>
      <select id="frameSizeSel">
        <option value="qvga"></option>
        <option value="vga" selected></option>
        <option value="svga"></option>
      </select>
      <button id="applyCamBtn" type="button"></button>
    </div>
  </div>
  <script src="/assets/app.js?v=36"></script>
</body>
</html>
)HTML";
