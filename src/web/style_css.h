#pragma once

#include <Arduino.h>

static const char kStyleCss[] PROGMEM = R"CSS(:root { color-scheme: dark; }
body {
  margin: 0;
  background: #111;
  color: #eee;
  font-family: system-ui, sans-serif;
  display: grid;
  place-items: center;
  min-height: 100vh;
}
.wrap {
  width: min(100vw, 960px);
  padding: 12px;
  box-sizing: border-box;
}
.preview {
  width: 100%;
  aspect-ratio: 4 / 3;
  border-radius: 10px;
  background: #000;
  position: relative;
  overflow: hidden;
}
#cam {
  width: 100%;
  height: 100%;
  object-fit: contain;
  display: none;
}
.overlay {
  position: absolute;
  inset: 0;
  display: none;
  align-items: center;
  justify-content: center;
  flex-direction: column;
  gap: 12px;
  text-align: center;
  padding: 16px;
  box-sizing: border-box;
}
.overlayMessage {
  margin: 0;
  opacity: 0.9;
  font-size: 20px;
  line-height: 1.4;
  max-width: 36ch;
}
p { opacity: 0.8; font-size: 14px; }
#status { min-height: 1.5em; }
.toolbar {
  margin-top: 10px;
  display: flex;
  gap: 10px;
  align-items: center;
  flex-wrap: nowrap;
}
.frameLabel {
  opacity: 0.85;
  font-size: 12px;
  white-space: nowrap;
}
.toolbar select {
  border: 1px solid #333;
  border-radius: 8px;
  padding: 8px 10px;
  background: #1b1b1b;
  color: #eee;
  min-width: 220px;
}
button {
  border: 0;
  border-radius: 8px;
  padding: 10px 14px;
  font-weight: 600;
  cursor: pointer;
  background: #ffd24a;
  color: #111;
}
.loader {
  width: 30px;
  height: 30px;
  border: 3px solid rgba(255,255,255,0.2);
  border-top-color: #ffd24a;
  border-radius: 50%;
  animation: spin 0.9s linear infinite;
  display: none;
}

.exitFullscreenBtn {
  position: absolute;
  right: 14px;
  bottom: 14px;
  z-index: 5;
  display: none;
  background: rgba(0, 0, 0, 0.72);
  color: #fff;
  border: 1px solid rgba(255, 255, 255, 0.35);
  border-radius: 999px;
  padding: 10px 14px;
  font-size: 13px;
  font-weight: 700;
}

.exitFullscreenBtn:hover {
  background: rgba(0, 0, 0, 0.86);
}

.preview:fullscreen {
  width: 100vw;
  height: 100vh;
  aspect-ratio: auto;
  border-radius: 0;
}

.preview:fullscreen #cam {
  width: 100%;
  height: 100%;
  object-fit: contain;
}

.preview:fullscreen .exitFullscreenBtn {
  display: block;
}

.preview:-webkit-full-screen {
  width: 100vw;
  height: 100vh;
  aspect-ratio: auto;
  border-radius: 0;
}

.preview:-webkit-full-screen #cam {
  width: 100%;
  height: 100%;
  object-fit: contain;
}

.preview:-webkit-full-screen .exitFullscreenBtn {
  display: block;
}
@keyframes spin {
  to { transform: rotate(360deg); }
}

@media (max-width: 700px) {
  .toolbar {
    flex-wrap: wrap;
  }
}
)CSS";
