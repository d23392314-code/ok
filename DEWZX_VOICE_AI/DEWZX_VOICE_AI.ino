/*
  ==========================================================================
  DEWZX VOICE AI  -  firmware SATU FILE (.ino) untuk ESP32-S3-CAM
  ==========================================================================
  Fitur : dengar (INMP441) -> AI (GenZi AI / Qwen3.8-Omni-Flash) -> bicara
          (Cicada-TTS + MAX98357A), kamera (foto soal), memory percakapan,
          wajah animasi di LCD ST7789 240x240, portal setup Wi-Fi + API key.

  Wajib: Arduino-ESP32 core 3.x (IDF 5.x), board "ESP32S3 Dev Module",
         PSRAM = OPI PSRAM (atau "Enabled" kalau PSRAM quad), Partition = Huge APP.
  Library: Adafruit GFX Library, Adafruit BusIO,
           Adafruit ST7735 and ST7789 Library, ArduinoJson (v7).

  KONTROL (tombol BOOT = GPIO0):
    1x ketuk   : ngobrol suara
    2x ketuk   : foto + pertanyaan suara (untuk soal sekolah)
    3x ketuk   : hapus memory percakapan
    ketuk saat AI bicara/mikir : batalkan
    tahan 3 detik : buka portal setup (Wi-Fi + API key)

  PINOUT (ubah di bagian KONFIGURASI kalau board kamu beda):
    Kamera : pinout ESP32-S3-EYE / Freenove ESP32-S3-WROOM-CAM
    LCD    : SCL=39 SDA=40 DC=38 RES=48 BLK=47 (CS tidak dipakai)
    INMP441: SCK=41 WS=42 SD=2  (L/R -> GND)
    MAX98357A: BCLK=1 LRC=14 DIN=21
  ==========================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <vector>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "mbedtls/base64.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <ArduinoJson.h>

// ==========================================================================
//  KONFIGURASI
// ==========================================================================
static const char* BASE_URL  = "https://ai.genzicorp.com/v1";   // tertanam di kode
static const char* CHAT_PATH = "/chat/completions";             // gaya OpenAI-compatible

static const char* AP_SSID = "DEWZX-Setup";   // nama Wi-Fi portal setup
static const char* AP_PASS = "dewzx1234";     // password Wi-Fi portal (min 8 karakter)

// Nilai awal (bisa diubah dari portal > Pengaturan lanjutan)
static const char* DEF_CHAT_MODEL = "qwen3.8-omni-flash";
static const char* DEF_TTS_PATH   = "/audio/speech";   // TEBAKAN gaya OpenAI - cek docs GenZi
static const char* DEF_TTS_MODEL  = "cicada-tts";      // TEBAKAN - cek docs GenZi
static const char* DEF_TTS_VOICE  = "";                // kosong = tidak dikirim
static const char* DEF_TTS_FORMAT = "wav";             // firmware hanya bisa memutar WAV PCM16
static const int   DEF_TTS_RATE   = 24000;             // dipakai kalau respons audio tanpa header WAV
static const int   DEF_VOLUME     = 80;                // 0-100

// ---- Tombol
#define BTN_PIN 0

// ---- Kamera (ESP32-S3-EYE / Freenove WROOM-CAM)
#define CAM_PWDN  -1
#define CAM_RESET -1
#define CAM_XCLK  15
#define CAM_SIOD   4
#define CAM_SIOC   5
#define CAM_Y9    16
#define CAM_Y8    17
#define CAM_Y7    18
#define CAM_Y6    12
#define CAM_Y5    10
#define CAM_Y4     8
#define CAM_Y3     9
#define CAM_Y2    11
#define CAM_VSYNC  6
#define CAM_HREF   7
#define CAM_PCLK  13
#define CAM_FRAMESIZE     FRAMESIZE_SXGA   // 1280x1024 (UXGA = lebih tajam tapi lebih berat)
#define CAM_JPEG_QUALITY  10               // 10 = bagus, makin besar angka makin kecil file
#define CAM_VFLIP   0
#define CAM_HMIRROR 0

// ---- LCD ST7789 240x240 (SPI)
#define PIN_LCD_SCK   39
#define PIN_LCD_MOSI  40
#define PIN_LCD_DC    38
#define PIN_LCD_RST   48
#define PIN_LCD_CS    -1      // modul 7 pin umumnya tanpa CS
#define PIN_LCD_BL    47
#define LCD_SPI_MODE  SPI_MODE3   // kalau layar kacau/bergeser coba SPI_MODE2 atau SPI_MODE0
#define LCD_SPI_HZ    40000000

// ---- Mikrofon INMP441 (I2S port 0)
#define PIN_MIC_BCLK 41
#define PIN_MIC_WS   42
#define PIN_MIC_DIN   2
#define MIC_RATE      16000
#define REC_MAX_SEC   15
#define NOSPEECH_SEC  6
#define SILENCE_END_MS 1200

// ---- Speaker MAX98357A (I2S port 1)
#define PIN_SPK_BCLK  1
#define PIN_SPK_WS   14
#define PIN_SPK_DOUT 21

// ---- AI
#define MAX_TOKENS 700
#define HIST_MAX_TURNS 8
#define HIST_MAX_BYTES 3000

static const char SYSTEM_PROMPT[] =
  "Kamu adalah DEWZX, asisten AI portabel yang ramah dan pintar. Selalu jawab dalam Bahasa Indonesia yang santai tapi sopan. "
  "Jawabanmu akan DIBACAKAN lewat speaker, jadi: singkat dan jelas (sekitar 6 kalimat kecuali soal butuh langkah), "
  "tanpa markdown, tanpa tabel, tanpa emoji, tanpa simbol aneh; tulis rumus dengan kata-kata (contoh: x kuadrat, dua per tiga, akar dua). "
  "Jika pengguna mengirim foto soal sekolah: baca soalnya dengan teliti, selesaikan langkah demi langkah secara ringkas, lalu sebutkan jawaban akhir dengan jelas. "
  "Jika foto buram, gelap, atau terpotong, katakan apa yang kurang dan minta pengguna memotret ulang. "
  "Jika pengguna bicara lewat audio, pahami ucapannya. "
  "FORMAT BALASAN WAJIB, persis dua bagian: baris pertama diawali 'DIDENGAR: ' berisi ringkasan sangat singkat (maksimal 12 kata) "
  "dari apa yang diucapkan atau ditanyakan pengguna; lalu baris berikutnya diawali 'JAWAB: ' berisi jawabanmu.";

// ==========================================================================
//  UTILITAS MEMORI (PSRAM)
// ==========================================================================
static void* pmalloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(n);
  return p;
}
static void* prealloc(void* p, size_t n) {
  void* q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!q) q = realloc(p, n);
  return q;
}

struct PBuf {
  char* d = nullptr;
  size_t n = 0, cap = 0;
  bool reserve(size_t c) {
    if (c <= cap) return true;
    size_t nc = cap ? cap : 1024;
    while (nc < c) nc *= 2;
    char* p = (char*)(d ? prealloc(d, nc) : pmalloc(nc));
    if (!p) return false;
    d = p; cap = nc;
    return true;
  }
  bool add(const char* s, size_t l) {
    if (!reserve(n + l + 1)) return false;
    memcpy(d + n, s, l); n += l; d[n] = 0;
    return true;
  }
  bool add(const char* s) { return add(s, strlen(s)); }
  bool add(const String& s) { return add(s.c_str(), s.length()); }
  bool addEsc(const String& s) {  // escape JSON (tanpa tanda kutip)
    size_t L = s.length();
    for (size_t i = 0; i < L; i++) {
      char c = s[i];
      bool ok = true;
      switch (c) {
        case '"':  ok = add("\\\"", 2); break;
        case '\\': ok = add("\\\\", 2); break;
        case '\n': ok = add("\\n", 2); break;
        case '\r': ok = add("\\r", 2); break;
        case '\t': ok = add("\\t", 2); break;
        default:
          if ((uint8_t)c < 0x20) { char tmp[8]; snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)(uint8_t)c); ok = add(tmp, 6); }
          else ok = add(&c, 1);
      }
      if (!ok) return false;
    }
    return true;
  }
  bool addB64(const uint8_t* src, size_t len) {
    size_t need = 4 * ((len + 2) / 3);
    if (!reserve(n + need + 4)) return false;
    size_t o = 0;
    if (mbedtls_base64_encode((unsigned char*)d + n, cap - n, &o, src, len) != 0) return false;
    n += o; d[n] = 0;
    return true;
  }
  void free_() { if (d) free(d); d = nullptr; n = cap = 0; }
};

class PsramSink : public Stream {
 public:
  uint8_t* d = nullptr;
  size_t n = 0, cap = 0;
  size_t limit = 6UL * 1024UL * 1024UL;
  using Print::write;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* b, size_t l) override {
    if (n + l > limit) return 0;
    if (n + l > cap) {
      size_t nc = cap ? cap * 2 : 65536;
      while (nc < n + l) nc *= 2;
      uint8_t* p = (uint8_t*)(d ? prealloc(d, nc) : pmalloc(nc));
      if (!p) return 0;
      d = p; cap = nc;
    }
    memcpy(d + n, b, l); n += l;
    return l;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void release() { if (d) free(d); d = nullptr; n = cap = 0; }
};

struct Photo { uint8_t* d = nullptr; size_t n = 0; void free_() { if (d) free(d); d = nullptr; n = 0; } };
struct Rec   { uint8_t* wav = nullptr; size_t wavLen = 0; bool speech = false; void free_() { if (wav) free(wav); wav = nullptr; wavLen = 0; } };

// ==========================================================================
//  PENGATURAN (NVS)
// ==========================================================================
static Preferences prefs;
static String g_ssid, g_pass, g_key1, g_key2;
static String g_chatModel, g_ttsPath, g_ttsModel, g_ttsVoice, g_ttsFmt;
static int  g_ttsRate = DEF_TTS_RATE, g_vol = DEF_VOLUME;
static bool g_audPrefix = true;   // base64 audio diawali "data:;base64," (gaya DashScope); auto-fallback
static bool g_stream = true;      // minta respons streaming (SSE); parser mendukung keduanya

static void cfgLoad() {
  prefs.begin("dewzx", false);
  g_ssid      = prefs.getString("ssid", "");
  g_pass      = prefs.getString("pass", "");
  g_key1      = prefs.getString("k1", "");
  g_key2      = prefs.getString("k2", "");
  g_chatModel = prefs.getString("cm", DEF_CHAT_MODEL);
  g_ttsPath   = prefs.getString("tp", DEF_TTS_PATH);
  g_ttsModel  = prefs.getString("tm", DEF_TTS_MODEL);
  g_ttsVoice  = prefs.getString("tv", DEF_TTS_VOICE);
  g_ttsFmt    = prefs.getString("tf", DEF_TTS_FORMAT);
  g_ttsRate   = prefs.getInt("tr", DEF_TTS_RATE);
  g_vol       = prefs.getInt("vol", DEF_VOLUME);
  g_audPrefix = prefs.getBool("ap", true);
  g_stream    = prefs.getBool("st", true);
  prefs.end();
}
static void cfgSave() {
  prefs.begin("dewzx", false);
  prefs.putString("ssid", g_ssid);
  prefs.putString("pass", g_pass);
  prefs.putString("k1", g_key1);
  prefs.putString("k2", g_key2);
  prefs.putString("cm", g_chatModel);
  prefs.putString("tp", g_ttsPath);
  prefs.putString("tm", g_ttsModel);
  prefs.putString("tv", g_ttsVoice);
  prefs.putString("tf", g_ttsFmt);
  prefs.putInt("tr", g_ttsRate);
  prefs.putInt("vol", g_vol);
  prefs.putBool("ap", g_audPrefix);
  prefs.putBool("st", g_stream);
  prefs.end();
}

// ==========================================================================
//  MEMORY PERCAKAPAN
// ==========================================================================
struct Turn { String u; String a; };
static std::vector<Turn> g_hist;

static String cutUtf8(const String& s, size_t len) {
  if (s.length() <= len) return s;
  while (len > 0 && (((uint8_t)s[len]) & 0xC0) == 0x80) len--;
  return s.substring(0, len);
}
static size_t histBytes() {
  size_t b = 0;
  for (size_t i = 0; i < g_hist.size(); i++) b += g_hist[i].u.length() + g_hist[i].a.length() + 2;
  return b;
}
static void histSave() {
  String s;
  for (size_t i = 0; i < g_hist.size(); i++) { s += g_hist[i].u; s += '\x1f'; s += g_hist[i].a; s += '\x1e'; }
  prefs.begin("dewzx", false);
  prefs.putString("hist", s);
  prefs.end();
}
static void histLoad() {
  prefs.begin("dewzx", false);
  String s = prefs.getString("hist", "");
  prefs.end();
  g_hist.clear();
  int pos = 0;
  while (pos < (int)s.length()) {
    int e = s.indexOf('\x1e', pos);
    if (e < 0) break;
    String rec = s.substring(pos, e);
    pos = e + 1;
    int m = rec.indexOf('\x1f');
    if (m < 0) continue;
    Turn t; t.u = rec.substring(0, m); t.a = rec.substring(m + 1);
    g_hist.push_back(t);
  }
}
static void histClear() {
  g_hist.clear();
  prefs.begin("dewzx", false);
  prefs.remove("hist");
  prefs.end();
}
static void histAdd(String u, String a) {
  u = cutUtf8(u, 160);
  a = cutUtf8(a, 420);
  Turn t; t.u = u; t.a = a;
  g_hist.push_back(t);
  while (g_hist.size() > HIST_MAX_TURNS) g_hist.erase(g_hist.begin());
  while (histBytes() > HIST_MAX_BYTES && g_hist.size() > 1) g_hist.erase(g_hist.begin());
  histSave();
}

// ==========================================================================
//  STATE GLOBAL
// ==========================================================================
enum FaceState : uint8_t { FS_BOOT, FS_IDLE, FS_LISTENING, FS_THINKING, FS_SPEAKING, FS_HAPPY, FS_ERROR, FS_SETUP, FS_TEXT };

static volatile uint8_t g_face = FS_BOOT;
static volatile float g_mic = 0, g_mouth = 0;
static char g_status[64] = "";
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

static void setStatus(const char* s) { portENTER_CRITICAL(&g_mux); strlcpy(g_status, s, sizeof(g_status)); portEXIT_CRITICAL(&g_mux); }
static void setStatus(const String& s) { setStatus(s.c_str()); }
static void setFace(FaceState f) { g_face = f; }

static volatile int  g_pendingMode = 0;   // 1 suara, 2 foto+suara, 3 hapus memory
static volatile bool g_busy = false;
static volatile bool g_abort = false;

static bool g_portal = false, g_portalForced = false;
static uint32_t g_lastWebHit = 0;
static uint32_t g_restartAt = 0;
static bool g_camOk = false, g_micOk = false, g_spkOk = false;

static std::vector<String> g_lines;       // teks jawaban untuk mode baca (kalau TTS gagal)
static volatile uint32_t g_textStart = 0;
static volatile int g_textPages = 1;

static WebServer server(80);
static DNSServer dns;
static WiFiClientSecure g_chatClient;
static WiFiClientSecure g_ttsClient;

// ==========================================================================
//  LCD + WAJAH ANIMASI
// ==========================================================================
static SPIClass lcdSPI(HSPI);
static Adafruit_ST7789 tft(&lcdSPI, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);
static GFXcanvas16* g_cv = nullptr;

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
static uint16_t C_BG = rgb(8, 12, 28), C_EYE = rgb(53, 224, 255), C_WHITE = 0xFFFF, C_DIM = rgb(60, 70, 100);
static uint16_t C_RED = rgb(255, 70, 70), C_GRN = rgb(80, 255, 150), C_YEL = rgb(255, 210, 60);
static uint16_t C_PINK = rgb(255, 110, 160), C_MOUTH = rgb(120, 20, 40);

static void textC(GFXcanvas16& c, const char* s, int y, uint8_t sz, uint16_t col) {
  c.setTextSize(sz); c.setTextColor(col); c.setTextWrap(false);
  int16_t x1, y1; uint16_t w, h;
  c.getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  c.setCursor((240 - (int)w) / 2, y);
  c.print(s);
}
static void thickLine(GFXcanvas16& c, int x0, int y0, int x1, int y1, int th, uint16_t col) {
  for (int o = -th / 2; o <= th / 2; o++) { c.drawLine(x0, y0 + o, x1, y1 + o, col); c.drawLine(x0 + o, y0, x1 + o, y1, col); }
}
static void arcThick(GFXcanvas16& c, int cx, int cy, int r, int a0, int a1, int th, uint16_t col) {
  for (int a = a0; a <= a1; a += 3) {
    float rad = a * 0.0174533f;
    c.fillCircle(cx + (int)(r * cosf(rad)), cy + (int)(r * sinf(rad)), th / 2, col);
  }
}
static void drawEye(GFXcanvas16& c, int cx, int cy, int w, int h, float bs, int gx, int gy, int pr) {
  int hh = max(4, (int)(h * bs));
  int r = min(16, hh / 2);
  c.fillRoundRect(cx - w / 2, cy - hh / 2, w, hh, r, C_EYE);
  if (bs > 0.45f) {
    int px = constrain(gx, -(w / 2 - pr - 2), (w / 2 - pr - 2));
    int py = constrain(gy, -(hh / 2 - pr - 2), (hh / 2 - pr - 2));
    c.fillCircle(cx + px, cy + py, pr, C_BG);
    c.fillCircle(cx + px - pr / 3, cy + py - pr / 3, max(2, pr / 4), C_WHITE);
  }
}

static void drawTopBar(GFXcanvas16& c, uint8_t st, uint32_t t) {
  bool wf = (WiFi.status() == WL_CONNECTED);
  int rssi = wf ? WiFi.RSSI() : -100;
  int lvl = !wf ? 0 : (rssi > -55 ? 4 : (rssi > -65 ? 3 : (rssi > -75 ? 2 : 1)));
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 3;
    c.fillRect(6 + i * 6, 16 - h, 4, h, i < lvl ? C_GRN : C_DIM);
  }
  c.setTextSize(1); c.setTextWrap(false);
  c.setTextColor(wf ? C_WHITE : C_RED);
  c.setCursor(34, 7);
  c.print(wf ? "WiFi" : "Wi-Fi putus");

  const char* lbl = "SIAP"; uint16_t col = C_GRN;
  switch (st) {
    case FS_LISTENING: lbl = "MENDENGAR"; col = C_EYE; break;
    case FS_THINKING:  lbl = "BERPIKIR";  col = C_YEL; break;
    case FS_SPEAKING:  lbl = "BICARA";    col = C_EYE; break;
    case FS_ERROR:     lbl = "ERROR";     col = C_RED; break;
    case FS_TEXT:      lbl = "BACA";      col = C_YEL; break;
    default:
      if (!wf || g_key1.length() == 0) { lbl = "OFFLINE"; col = C_RED; }
  }
  c.fillCircle(142, 11, 4, col);
  c.setTextColor(C_WHITE);
  c.setCursor(151, 7);
  c.print("AI ");
  c.print(lbl);
  c.drawFastHLine(0, 22, 240, C_DIM);
}

static void drawBottom(GFXcanvas16& c, uint8_t st) {
  char s[64];
  portENTER_CRITICAL(&g_mux); strlcpy(s, g_status, sizeof(s)); portEXIT_CRITICAL(&g_mux);
  c.drawFastHLine(0, 218, 240, C_DIM);
  if (!s[0]) return;
  uint16_t col = (st == FS_ERROR) ? C_RED : C_WHITE;
  if (strlen(s) <= 19) { s[19] = 0; textC(c, s, 223, 2, col); }
  else { s[38] = 0; textC(c, s, 226, 1, col); }
}

static void drawFace(GFXcanvas16& c, uint8_t st, uint32_t t) {
  const int ex1 = 80, ex2 = 160, ey = 98;
  static uint32_t nextBlink = 0, blinkStart = 0;
  if (t > nextBlink) { blinkStart = t; nextBlink = t + 2500 + (esp_random() % 3500); }
  float bs = 1.0f;
  uint32_t bt = t - blinkStart;
  if (bt < 220) { float ph = bt / 220.0f; bs = fabsf(1.0f - 2.0f * ph); if (bs < 0.08f) bs = 0.08f; }
  int gx = (int)(sinf(t / 1700.0f) * 6), gy = (int)(sinf(t / 2300.0f + 1) * 3);

  switch (st) {
    case FS_LISTENING: {
      float lv = g_mic;
      int h = 62 + (int)(lv * 8);
      thickLine(c, 58, 54, 102, 50, 4, C_EYE);
      thickLine(c, 138, 50, 182, 54, 4, C_EYE);
      drawEye(c, ex1, ey + 6, 50, h, bs, 0, 0, 15);
      drawEye(c, ex2, ey + 6, 50, h, bs, 0, 0, 15);
      c.fillCircle(120, 160, 9, C_EYE); c.fillCircle(120, 160, 5, C_BG);
      for (int i = 0; i < 13; i++) {
        float l = lv * (0.5f + 0.5f * sinf(t / 90.0f + i * 0.7f));
        int hh = 4 + (int)(l * 22);
        c.fillRoundRect(32 + i * 14, 203 - hh / 2, 8, hh, 3, C_EYE);
      }
      break;
    }
    case FS_THINKING: {
      thickLine(c, 56, 62, 104, 50, 4, C_EYE);
      thickLine(c, 136, 64, 184, 64, 4, C_EYE);
      drawEye(c, ex1, ey, 44, 50, bs, 9, -9, 11);
      drawEye(c, ex2, ey, 44, 50, bs, 9, -9, 11);
      for (int x = 92; x <= 148; x += 2) c.fillCircle(x, 164 + (int)(3 * sinf(x * 0.35f + t / 200.0f)), 2, C_EYE);
      int act = (t / 300) % 3;
      for (int i = 0; i < 3; i++) c.fillCircle(170 + i * 16, 40, 4, i == act ? C_YEL : C_DIM);
      break;
    }
    case FS_SPEAKING: {
      drawEye(c, ex1, ey, 44, 58, bs, gx, 0, 11);
      drawEye(c, ex2, ey, 44, 58, bs, gx, 0, 11);
      float m = g_mouth;
      int mh = 8 + (int)(m * 46), mw = 60 - (int)(m * 12);
      c.fillRoundRect(120 - mw / 2 - 2, 162 - mh / 2 - 2, mw + 4, mh + 4, min(16, (mh + 4) / 2), C_WHITE);
      c.fillRoundRect(120 - mw / 2, 162 - mh / 2, mw, mh, min(14, mh / 2), C_MOUTH);
      if (mh > 26) c.fillRoundRect(106, 162 + mh / 2 - 10, 28, 8, 4, C_PINK);
      break;
    }
    case FS_HAPPY: {
      arcThick(c, ex1, ey + 12, 22, 200, 340, 7, C_EYE);
      arcThick(c, ex2, ey + 12, 22, 200, 340, 7, C_EYE);
      c.fillCircle(46, 140, 11, rgb(255, 100, 150));
      c.fillCircle(194, 140, 11, rgb(255, 100, 150));
      arcThick(c, 120, 120, 46, 25, 155, 6, C_EYE);
      for (int i = 0; i < 3; i++) {
        if (((t / 250) + i) % 3 == 0) {
          int sx = 40 + i * 80, sy = 52 + (i % 2) * 14;
          c.drawFastHLine(sx - 6, sy, 13, C_YEL); c.drawFastVLine(sx, sy - 6, 13, C_YEL);
        }
      }
      break;
    }
    case FS_ERROR: {
      thickLine(c, ex1 - 16, ey - 16, ex1 + 16, ey + 16, 5, C_RED);
      thickLine(c, ex1 - 16, ey + 16, ex1 + 16, ey - 16, 5, C_RED);
      thickLine(c, ex2 - 16, ey - 16, ex2 + 16, ey + 16, 5, C_RED);
      thickLine(c, ex2 - 16, ey + 16, ex2 + 16, ey - 16, 5, C_RED);
      arcThick(c, 120, 196, 34, 215, 325, 6, C_RED);
      break;
    }
    default: {  // FS_IDLE
      drawEye(c, ex1, ey, 44, 58, bs, gx, gy, 11);
      drawEye(c, ex2, ey, 44, 58, bs, gx, gy, 11);
      arcThick(c, 120, 128, 36, 35, 145, 5, C_EYE);
    }
  }
}

static void drawTextPage(GFXcanvas16& c, uint32_t t) {
  int pages = g_textPages;
  int page = (int)((t - g_textStart) / 6000);
  if (page >= pages) page = pages - 1;
  if (page < 0) page = 0;
  const int perPage = 11;
  c.setTextSize(2); c.setTextColor(C_WHITE); c.setTextWrap(false);
  for (int i = 0; i < perPage; i++) {
    size_t idx = (size_t)page * perPage + i;
    if (idx >= g_lines.size()) break;
    c.setCursor(4, 28 + i * 17);
    c.print(g_lines[idx]);
  }
  char pg[16]; snprintf(pg, sizeof(pg), "%d/%d", page + 1, pages);
  c.setTextSize(1); c.setTextColor(C_YEL); c.setCursor(206, 208); c.print(pg);
}

static void drawBoot(GFXcanvas16& c, uint32_t t) {
  textC(c, "DEWZX", 50, 5, C_EYE);
  textC(c, "VOICE AI", 100, 3, C_WHITE);
  int a = (t / 8) % 360;
  for (int i = 0; i < 12; i++) {
    float rad = (a + i * 12) * 0.0174533f;
    c.fillCircle(120 + (int)(34 * cosf(rad)), 168 + (int)(18 * sinf(rad)), 4 - i / 4, i < 10 ? C_EYE : C_DIM);
  }
  drawBottom(c, FS_BOOT);
}

static void drawSetup(GFXcanvas16& c, uint32_t t) {
  textC(c, "MODE SETUP", 10, 2, C_YEL);
  textC(c, "1. Sambung HP ke Wi-Fi:", 40, 1, C_WHITE);
  textC(c, AP_SSID, 54, 2, C_EYE);
  char pw[48]; snprintf(pw, sizeof(pw), "Pass: %s", AP_PASS);
  textC(c, pw, 76, 2, C_WHITE);
  textC(c, "2. Buka browser ke:", 106, 1, C_WHITE);
  textC(c, "192.168.4.1", 120, 3, C_GRN);
  textC(c, "3. Isi Wi-Fi & API key", 156, 1, C_WHITE);
  textC(c, "lalu tekan Simpan", 170, 1, C_WHITE);
  if ((t / 500) % 2) c.fillCircle(120, 205, 5, C_YEL);
}

static void faceTask(void*) {
  for (;;) {
    uint32_t t = millis();
    uint8_t st = g_face;
    GFXcanvas16& c = *g_cv;
    c.fillScreen(C_BG);
    switch (st) {
      case FS_BOOT:  drawBoot(c, t); break;
      case FS_SETUP: drawSetup(c, t); break;
      default:
        drawTopBar(c, st, t);
        if (st == FS_TEXT) drawTextPage(c, t); else drawFace(c, st, t);
        drawBottom(c, st);
    }
    tft.startWrite();
    tft.setAddrWindow(0, 0, 240, 240);
    tft.writePixels(c.getBuffer(), 240 * 240);
    tft.endWrite();
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

// ==========================================================================
//  AUDIO: MIKROFON, SPEAKER
// ==========================================================================
static i2s_chan_handle_t rxChan = nullptr, txChan = nullptr;
static bool g_spkRun = false;

static bool micInit() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 6; cc.dma_frame_num = 320;
  if (i2s_new_channel(&cc, nullptr, &rxChan) != ESP_OK) return false;
  i2s_std_config_t sc = {};
  sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE);
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
  sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  sc.gpio_cfg.bclk = (gpio_num_t)PIN_MIC_BCLK;
  sc.gpio_cfg.ws   = (gpio_num_t)PIN_MIC_WS;
  sc.gpio_cfg.dout = I2S_GPIO_UNUSED;
  sc.gpio_cfg.din  = (gpio_num_t)PIN_MIC_DIN;
  if (i2s_channel_init_std_mode(rxChan, &sc) != ESP_OK) return false;
  return true;  // channel di-enable hanya saat merekam
}

static bool spkInit() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8; cc.dma_frame_num = 512;
  if (i2s_new_channel(&cc, &txChan, nullptr) != ESP_OK) return false;
  i2s_std_config_t sc = {};
  sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000);
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  sc.gpio_cfg.bclk = (gpio_num_t)PIN_SPK_BCLK;
  sc.gpio_cfg.ws   = (gpio_num_t)PIN_SPK_WS;
  sc.gpio_cfg.dout = (gpio_num_t)PIN_SPK_DOUT;
  sc.gpio_cfg.din  = I2S_GPIO_UNUSED;
  if (i2s_channel_init_std_mode(txChan, &sc) != ESP_OK) return false;
  return true;
}

static bool spkBegin(uint32_t rate) {
  if (!g_spkOk) return false;
  if (g_spkRun) { i2s_channel_disable(txChan); g_spkRun = false; }
  i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  if (i2s_channel_reconfig_std_clock(txChan, &clk) != ESP_OK) return false;
  if (i2s_channel_enable(txChan) != ESP_OK) return false;
  g_spkRun = true;
  return true;
}
static bool i2sWriteAll(const void* data, size_t bytes) {
  const uint8_t* p = (const uint8_t*)data;
  while (bytes > 0) {
    size_t w = 0;
    if (i2s_channel_write(txChan, p, bytes, &w, 1000) != ESP_OK) return false;
    p += w; bytes -= w;
  }
  return true;
}
static void spkEnd() {
  if (!g_spkRun) return;
  static int16_t z[512 * 2];
  memset(z, 0, sizeof(z));
  for (int i = 0; i < 5; i++) i2sWriteAll(z, sizeof(z));  // kuras buffer DMA
  i2s_channel_disable(txChan);
  g_spkRun = false;
}
static void beep(int freq, int ms) {
  if (!spkBegin(16000)) return;
  static int16_t b[256 * 2];
  int total = 16 * ms, done = 0;
  float amp = 2500.0f * g_vol / 100.0f;
  while (done < total) {
    int k = min(256, total - done);
    for (int i = 0; i < k; i++) {
      float env = 1.0f;
      int pos = done + i;
      if (pos < 160) env = pos / 160.0f;
      if (total - pos < 160) env = (total - pos) / 160.0f;
      int16_t v = (int16_t)(amp * env * sinf(2.0f * 3.14159f * freq * pos / 16000.0f));
      b[2 * i] = v; b[2 * i + 1] = v;
    }
    i2sWriteAll(b, k * 4);
    done += k;
  }
  spkEnd();
}

static void writeWavHeader(uint8_t* h, uint32_t dataBytes, uint32_t rate) {
  uint32_t v; uint16_t s;
  memcpy(h, "RIFF", 4); v = 36 + dataBytes; memcpy(h + 4, &v, 4);
  memcpy(h + 8, "WAVEfmt ", 8); v = 16; memcpy(h + 16, &v, 4);
  s = 1; memcpy(h + 20, &s, 2); s = 1; memcpy(h + 22, &s, 2);
  memcpy(h + 24, &rate, 4); v = rate * 2; memcpy(h + 28, &v, 4);
  s = 2; memcpy(h + 32, &s, 2); s = 16; memcpy(h + 34, &s, 2);
  memcpy(h + 36, "data", 4); memcpy(h + 40, &dataBytes, 4);
}

// Rekam dengan deteksi diam sederhana. r.speech=false kalau tidak ada ucapan.
static bool recordUtterance(Rec& r) {
  r.speech = false;
  if (!g_micOk) return false;
  const size_t maxS = (size_t)MIC_RATE * REC_MAX_SEC;
  uint8_t* buf = (uint8_t*)pmalloc(44 + maxS * 2);
  if (!buf) return false;
  int16_t* pcm = (int16_t*)(buf + 44);
  static int32_t raw[320];
  size_t total = 0; int blocks = 0, calN = 0, above = 0, quiet = 0;
  float noise = 0, thr = 400;
  bool speaking = false;
  size_t firstAbove = 0;
  uint32_t t0 = millis();
  i2s_channel_enable(rxChan);
  while (total + 320 <= maxS && !g_abort) {
    if (!speaking && millis() - t0 > (uint32_t)NOSPEECH_SEC * 1000 + 1000) break;
    size_t got = 0;
    if (i2s_channel_read(rxChan, raw, sizeof(raw), &got, 300) != ESP_OK || got == 0) continue;
    int cnt = (int)(got / 4);
    uint64_t acc = 0;
    for (int i = 0; i < cnt; i++) {
      int32_t v = raw[i] >> 14;
      if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
      pcm[total + i] = (int16_t)v;
      acc += (uint64_t)((int64_t)v * v);
    }
    float rms = sqrtf((float)acc / (float)(cnt ? cnt : 1));
    size_t blkStart = total;
    total += cnt; blocks++;
    g_mic = min(1.0f, rms / 2500.0f);
    if (blocks <= 5) continue;                       // tunggu mic stabil
    if (blocks <= 15) {                              // kalibrasi noise
      noise += rms; calN++;
      if (blocks == 15) { noise /= calN; thr = constrain(noise * 3.0f, 350.0f, 1500.0f); }
      continue;
    }
    if (!speaking) {
      if (rms > thr) { if (above == 0) firstAbove = blkStart; if (++above >= 3) { speaking = true; quiet = 0; } }
      else above = 0;
      if (millis() - t0 > (uint32_t)NOSPEECH_SEC * 1000) break;
    } else {
      if (rms > thr) quiet = 0; else quiet++;
      if (quiet * 20 >= SILENCE_END_MS) break;
    }
  }
  i2s_channel_disable(rxChan);
  g_mic = 0;
  if (!speaking || g_abort) { free(buf); return true; }
  size_t start = firstAbove > 1280 ? firstAbove - 1280 : 0;
  size_t end = total, tail = (size_t)quiet * 320, keepTail = 3 * 320;
  if (tail > keepTail) end -= (tail - keepTail);
  if (end <= start || (end - start) < (size_t)(MIC_RATE / 2)) { free(buf); return true; }
  size_t len = end - start;
  if (start) memmove(pcm, pcm + start, len * 2);
  writeWavHeader(buf, (uint32_t)(len * 2), MIC_RATE);
  r.wav = buf; r.wavLen = 44 + len * 2; r.speech = true;
  return true;
}

static inline int16_t rd16s(const uint8_t* p) { int16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

// Putar WAV PCM16 (atau PCM16 mentah) dari memori.
static bool playAudioBuf(const uint8_t* d, size_t n, String& err) {
  uint32_t rate = (uint32_t)g_ttsRate; uint16_t ch = 1;
  const uint8_t* pcm = d; size_t len = n;
  if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WAVE", 4) == 0) {
    size_t pos = 12; bool gotData = false; uint16_t tag = 1, bits = 16;
    while (pos + 8 <= n) {
      uint32_t sz = rd32(d + pos + 4); size_t body = pos + 8;
      if (memcmp(d + pos, "fmt ", 4) == 0 && body + 16 <= n) {
        tag = rd16(d + body); ch = rd16(d + body + 2); rate = rd32(d + body + 4); bits = rd16(d + body + 14);
      } else if (memcmp(d + pos, "data", 4) == 0) {
        pcm = d + body;
        len = (sz == 0 || sz == 0xFFFFFFFFu || body + sz > n) ? (n - body) : sz;
        gotData = true; break;
      }
      pos = body + sz + (sz & 1);
    }
    if (!gotData) { err = "WAV tanpa data"; return false; }
    if ((tag != 1 && tag != 0xFFFE) || bits != 16) { err = "WAV bukan PCM16"; return false; }
    if (ch < 1 || ch > 2) { err = "Channel audio aneh"; return false; }
  } else if (n >= 3 && (memcmp(d, "ID3", 3) == 0 || (d[0] == 0xFF && (d[1] & 0xE0) == 0xE0))) {
    err = "TTS kirim MP3, set format wav"; return false;
  } else if (n > 0 && d[0] == '{') {
    String s; for (size_t i = 0; i < n && i < 120; i++) s += (char)d[i];
    Serial.println("[TTS] respons JSON: " + s);
    err = "TTS balas JSON bukan audio"; return false;
  }
  if (rate < 8000 || rate > 48000) { err = "Sample rate aneh"; return false; }
  size_t frames = len / 2 / ch;
  if (frames < 100) { err = "Audio kosong"; return false; }
  if (!spkBegin(rate)) { err = "Speaker gagal"; return false; }
  static int16_t out[512 * 2];
  float gain = g_vol / 100.0f;
  for (size_t i = 0; i < frames && !g_abort; i += 512) {
    size_t k = min((size_t)512, frames - i);
    uint64_t acc = 0;
    for (size_t j = 0; j < k; j++) {
      int16_t s0 = rd16s(pcm + (i + j) * ch * 2);
      acc += (uint64_t)((int64_t)s0 * s0);
      int32_t s = (int32_t)(s0 * gain);
      if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
      out[2 * j] = (int16_t)s; out[2 * j + 1] = (int16_t)s;
    }
    float lv = min(1.0f, sqrtf((float)acc / (float)k) / 6000.0f);
    g_mouth = g_mouth * 0.5f + lv * 0.5f;
    if (!i2sWriteAll(out, k * 4)) { err = "I2S tulis gagal"; spkEnd(); return false; }
  }
  spkEnd();
  g_mouth = 0;
  return true;
}

// ==========================================================================
//  KAMERA
// ==========================================================================
static bool camInit() {
  camera_config_t c;
  memset(&c, 0, sizeof(c));
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = CAM_Y2; c.pin_d1 = CAM_Y3; c.pin_d2 = CAM_Y4; c.pin_d3 = CAM_Y5;
  c.pin_d4 = CAM_Y6; c.pin_d5 = CAM_Y7; c.pin_d6 = CAM_Y8; c.pin_d7 = CAM_Y9;
  c.pin_xclk = CAM_XCLK; c.pin_pclk = CAM_PCLK; c.pin_vsync = CAM_VSYNC; c.pin_href = CAM_HREF;
  c.pin_sccb_sda = CAM_SIOD; c.pin_sccb_scl = CAM_SIOC;
  c.pin_pwdn = CAM_PWDN; c.pin_reset = CAM_RESET;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = CAM_FRAMESIZE;
  c.jpeg_quality = CAM_JPEG_QUALITY;
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  esp_err_t e = esp_camera_init(&c);
  if (e != ESP_OK) { Serial.printf("[CAM] init gagal 0x%x\n", e); return false; }
  sensor_t* s = esp_camera_sensor_get();
  if (s) { s->set_vflip(s, CAM_VFLIP); s->set_hmirror(s, CAM_HMIRROR); }
  return true;
}

static bool capturePhoto(Photo& p) {
  if (!g_camOk) return false;
  for (int i = 0; i < 3; i++) {                 // buang frame awal agar exposure stabil
    camera_fb_t* f = esp_camera_fb_get();
    if (f) esp_camera_fb_return(f);
    vTaskDelay(pdMS_TO_TICKS(90));
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return false;
  bool ok = false;
  if (fb->format == PIXFORMAT_JPEG && fb->len > 1000) {
    p.d = (uint8_t*)pmalloc(fb->len);
    if (p.d) { memcpy(p.d, fb->buf, fb->len); p.n = fb->len; ok = true; }
  }
  esp_camera_fb_return(fb);
  Serial.printf("[CAM] foto %u byte\n", (unsigned)p.n);
  return ok;
}

// ==========================================================================
//  TEKS: PEMBERSIH, PEMECAH KALIMAT, PEMBUNGKUS BARIS
// ==========================================================================
static String cleanMd(const String& in) {
  String o; o.reserve(in.length());
  size_t n = in.length();
  for (size_t i = 0; i < n; i++) {
    uint8_t ch = in[i];
    if ((ch & 0xF8) == 0xF0) { i += 3; continue; }                   // emoji 4-byte
    if (ch == '*' || ch == '#' || ch == '`' || ch == '~' || ch == '|' || ch == '_') continue;
    o += (char)ch;
  }
  return o;
}
static String forSpeech(const String& in) {
  String o; o.reserve(in.length() + 16);
  size_t n = in.length();
  for (size_t i = 0; i < n; i++) {
    char c = in[i];
    if (c == '\r') continue;
    if (c == '\n') {
      char last = o.length() ? o[o.length() - 1] : ' ';
      if (last != '.' && last != ',' && last != ':' && last != ';' && last != '?' && last != '!') o += '.';
      o += ' ';
    } else if (c == '=') o += " sama dengan ";
    else if (c == '+') o += " tambah ";
    else o += c;
  }
  return o;
}
static String toAscii(const String& s) {
  String o; size_t n = s.length();
  for (size_t i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c < 0x80) { o += (char)c; continue; }
    if (c == 0xC3 && i + 1 < n) { uint8_t d = s[i + 1]; if (d == 0x97) o += 'x'; else if (d == 0xB7) o += '/'; i++; continue; }
    if (c == 0xC2 && i + 1 < n) { uint8_t d = s[i + 1]; if (d == 0xB2) o += "^2"; else if (d == 0xB3) o += "^3"; else if (d == 0xB0) o += " der"; i++; continue; }
    if (c == 0xCF && i + 1 < n) { if ((uint8_t)s[i + 1] == 0x80) o += "pi"; i++; continue; }
    if (c == 0xE2 && i + 2 < n) {
      uint8_t d = s[i + 1], e = s[i + 2];
      if (d == 0x88 && e == 0x9A) o += "akar";
      else if (d == 0x80 && (e == 0x93 || e == 0x94)) o += '-';
      else if (d == 0x80 && (e == 0x98 || e == 0x99)) o += '\'';
      else if (d == 0x80 && (e == 0x9C || e == 0x9D)) o += '"';
      else if (d == 0x80 && e == 0xA6) o += "...";
      i += 2; continue;
    }
    if ((c & 0xE0) == 0xC0) i += 1; else if ((c & 0xF0) == 0xE0) i += 2; else if ((c & 0xF8) == 0xF0) i += 3;
  }
  return o;
}
static void wrapLines(const String& s, std::vector<String>& out, size_t cols) {
  String line, word;
  auto flushWord = [&]() {
    if (!word.length()) return;
    if (line.length() + (line.length() ? 1 : 0) + word.length() <= cols) { if (line.length()) line += ' '; line += word; }
    else {
      if (line.length()) out.push_back(line);
      line = "";
      while (word.length() > cols) { out.push_back(word.substring(0, cols)); word = word.substring(cols); }
      line = word;
    }
    word = "";
  };
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      flushWord();
      if (c == '\n') { if (line.length()) out.push_back(line); line = ""; }
    } else word += c;
  }
  flushWord();
  if (line.length()) out.push_back(line);
}
static void splitForTts(const String& s, std::vector<String>& out, size_t firstMax, size_t maxLen) {
  size_t pos = 0, n = s.length(); bool first = true;
  while (pos < n) {
    while (pos < n && (s[pos] == ' ' || s[pos] == '\n')) pos++;
    if (pos >= n) break;
    size_t lim = first ? firstMax : maxLen;
    size_t end = pos + lim;
    if (end >= n) end = n;
    else {
      int cut = -1;
      for (size_t i = end; i > pos + 20; i--) {
        char c = s[i - 1];
        if (c == '.' && i < n && isdigit((uint8_t)s[i]) && i >= 2 && isdigit((uint8_t)s[i - 2])) continue;  // angka desimal
        if (c == '.' || c == '!' || c == '?' || c == ';' || c == '\n' || c == ':') { cut = (int)i; break; }
      }
      if (cut < 0) for (size_t i = end; i > pos + 10; i--) { if (s[i - 1] == ' ' || s[i - 1] == ',') { cut = (int)i; break; } }
      if (cut > 0) end = (size_t)cut;
      else while (end > pos && (((uint8_t)s[end]) & 0xC0) == 0x80) end--;
    }
    String part = s.substring(pos, end); part.trim();
    if (part.length()) out.push_back(part);
    pos = end; first = false;
  }
}
static void parseTagged(const String& text, String& heard, String& ans) {
  String up = text; up.toUpperCase();
  int ij = up.indexOf("JAWAB:");
  if (ij >= 0) {
    String h = text.substring(0, ij);
    int id = up.indexOf("DIDENGAR:");
    if (id >= 0 && id < ij) h = text.substring(id + 9, ij);
    h.trim(); heard = h;
    ans = text.substring(ij + 6); ans.trim();
  } else { heard = ""; ans = text; ans.trim(); }
}

// ==========================================================================
//  KLIEN AI (CHAT MULTIMODAL + TTS)
// ==========================================================================
struct AiReply { bool ok = false; String text; String err; };

static String httpErrText(int code) {
  if (code == 401 || code == 403) return "API key ditolak";
  if (code == 404) return "Endpoint 404";
  if (code == 429) return "Kuota/limit API";
  if (code >= 500) return "Server AI error " + String(code);
  if (code < 0) return "Koneksi gagal (" + String(code) + ")";
  return "HTTP " + String(code);
}
static String extractErrMsg(const String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return "";
  if (doc["error"].is<JsonObject>()) { const char* m = doc["error"]["message"] | ""; return String(m); }
  if (doc["error"].is<const char*>()) return String(doc["error"].as<const char*>());
  const char* m2 = doc["message"] | "";
  return String(m2);
}

static bool buildChatBody(PBuf& b, const String& userText, const Photo* ph, const Rec* au, bool audioPrefixed) {
  size_t est = 4096 + (ph ? ph->n * 4 / 3 + 64 : 0) + (au ? au->wavLen * 4 / 3 + 64 : 0);
  if (!b.reserve(est)) return false;
  bool ok = b.add("{\"model\":\"") && b.addEsc(g_chatModel) && b.add("\",\"max_tokens\":") && b.add(String(MAX_TOKENS));
  ok = ok && b.add(g_stream ? ",\"stream\":true" : ",\"stream\":false");
  ok = ok && b.add(",\"messages\":[{\"role\":\"system\",\"content\":\"") && b.addEsc(String(SYSTEM_PROMPT)) && b.add("\"}");
  for (size_t i = 0; ok && i < g_hist.size(); i++) {
    ok = b.add(",{\"role\":\"user\",\"content\":\"") && b.addEsc(g_hist[i].u) &&
         b.add("\"},{\"role\":\"assistant\",\"content\":\"") && b.addEsc(g_hist[i].a) && b.add("\"}");
  }
  ok = ok && b.add(",{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"") && b.addEsc(userText) && b.add("\"}");
  if (ok && ph) ok = b.add(",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,") && b.addB64(ph->d, ph->n) && b.add("\"}}");
  if (ok && au) {
    ok = b.add(",{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"");
    if (ok && audioPrefixed) ok = b.add("data:;base64,");
    ok = ok && b.addB64(au->wav, au->wavLen) && b.add("\",\"format\":\"wav\"}}");
  }
  ok = ok && b.add("]}]}");
  return ok;
}

// Parser: mendukung JSON biasa maupun SSE (stream).
static bool parseChatResponse(const String& resp, String& out, String& errMsg) {
  out = "";
  String head = resp.substring(0, 40); head.trim();
  bool sse = head.startsWith("data:") || head.startsWith(":") || head.startsWith("event:");
  JsonDocument doc;
  if (sse) {
    int pos = 0, L = (int)resp.length();
    while (pos < L) {
      int e = resp.indexOf('\n', pos); if (e < 0) e = L;
      String line = resp.substring(pos, e); pos = e + 1;
      line.trim();
      if (!line.startsWith("data:")) continue;
      line = line.substring(5); line.trim();
      if (line == "[DONE]") break;
      if (!line.length()) continue;
      doc.clear();
      if (deserializeJson(doc, line)) continue;
      if (doc["error"].is<JsonObject>()) { const char* m = doc["error"]["message"] | "error"; errMsg = m; continue; }
      JsonVariant ch = doc["choices"][0];
      const char* d = ch["delta"]["content"] | "";
      if (d[0]) out += d;
      else { const char* m = ch["message"]["content"] | ""; if (m[0]) out += m; }
    }
    return out.length() > 0;
  }
  DeserializationError e = deserializeJson(doc, resp);
  if (e) { errMsg = "Respon bukan JSON"; return false; }
  if (!doc["error"].isNull()) {
    if (doc["error"].is<JsonObject>()) { const char* m = doc["error"]["message"] | "error"; errMsg = m; }
    else errMsg = doc["error"].as<String>();
    return false;
  }
  JsonVariant c = doc["choices"][0]["message"]["content"];
  if (c.is<const char*>()) out = c.as<String>();
  else if (c.is<JsonArray>()) { for (JsonVariant part : c.as<JsonArray>()) { const char* t = part["text"] | ""; out += t; } }
  return out.length() > 0;
}

static AiReply askAI(const String& userText, const Photo* ph, const Rec* au) {
  AiReply R;
  String url = String(BASE_URL) + CHAT_PATH;
  bool pref = g_audPrefix;
  for (int attempt = 0; attempt < 2; attempt++) {
    PBuf b;
    if (!buildChatBody(b, userText, ph, au, pref)) { b.free_(); R.err = "Memori habis"; return R; }
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(60000);
    http.setReuse(false);
    if (!http.begin(g_chatClient, url)) { b.free_(); R.err = "URL AI salah"; return R; }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + g_key1);
    int code = http.POST((uint8_t*)b.d, b.n);
    String resp;
    if (code > 0) resp = http.getString();
    http.end();
    g_chatClient.stop();
    b.free_();
    Serial.printf("[AI] attempt %d HTTP %d, %u byte\n", attempt, code, (unsigned)resp.length());
    if (code == 200) {
      String em;
      if (parseChatResponse(resp, R.text, em)) {
        R.ok = true;
        if (au && pref != g_audPrefix) { g_audPrefix = pref; prefs.begin("dewzx", false); prefs.putBool("ap", pref); prefs.end(); }
        return R;
      }
      Serial.println("[AI] parse gagal: " + em + " | " + resp.substring(0, 300));
      R.err = em.length() ? ("AI: " + cutUtf8(toAscii(em), 30)) : "Jawaban AI kosong";
      return R;
    }
    Serial.println("[AI] body: " + resp.substring(0, 400));
    if (au && attempt == 0 && (code == 400 || code == 422)) { pref = !pref; continue; }  // coba format base64 lain
    String em = extractErrMsg(resp);
    R.err = httpErrText(code);
    if (em.length() && code > 0) R.err += ": " + cutUtf8(toAscii(em), 22);
    return R;
  }
  return R;
}

static bool ttsFetch(const String& text, PsramSink& sink, String& err) {
  const String& key = g_key2.length() ? g_key2 : g_key1;
  String url = String(BASE_URL) + g_ttsPath;
  PBuf b;
  bool ok = b.add("{\"model\":\"") && b.addEsc(g_ttsModel) && b.add("\",\"input\":\"") && b.addEsc(text) && b.add("\"");
  if (ok && g_ttsVoice.length()) ok = b.add(",\"voice\":\"") && b.addEsc(g_ttsVoice) && b.add("\"");
  if (ok && g_ttsFmt.length())   ok = b.add(",\"response_format\":\"") && b.addEsc(g_ttsFmt) && b.add("\"");
  ok = ok && b.add("}");
  if (!ok) { b.free_(); err = "Memori habis"; return false; }
  for (int attempt = 0; attempt < 2; attempt++) {
    sink.n = 0;
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(30000);
    http.setReuse(true);
    if (!http.begin(g_ttsClient, url)) { b.free_(); err = "URL TTS salah"; return false; }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + key);
    int code = http.POST((uint8_t*)b.d, b.n);
    if (code == 200) {
      http.writeToStream(&sink);
      http.end();
      if (sink.n > 0) { b.free_(); return true; }
      b.free_(); err = "Audio TTS kosong"; return false;
    }
    String body = (code > 0) ? http.getString() : String("");
    http.end();
    Serial.printf("[TTS] HTTP %d %s\n", code, body.substring(0, 300).c_str());
    if (code < 0 && attempt == 0) { g_ttsClient.stop(); continue; }
    b.free_();
    err = "TTS " + httpErrText(code);
    String em = extractErrMsg(body);
    if (em.length() && code > 0) err += ": " + cutUtf8(toAscii(em), 18);
    return false;
  }
  b.free_();
  err = "TTS gagal";
  return false;
}

// ==========================================================================
//  ALUR INTERAKSI
// ==========================================================================
static void showError(const String& m, uint32_t ms = 3500) {
  Serial.println("[ERR] " + m);
  setStatus(toAscii(m));
  setFace(FS_ERROR);
  vTaskDelay(pdMS_TO_TICKS(ms));
  setFace(FS_IDLE);
  setStatus("");
}

static void showTextFallback(const String& answer, const String& note) {
  g_lines.clear();
  wrapLines(toAscii(answer), g_lines, 19);
  int pages = ((int)g_lines.size() + 10) / 11;
  if (pages < 1) pages = 1;
  g_textPages = pages;
  g_textStart = millis();
  setStatus(note.length() ? note : String("Ketuk tombol: tutup"));
  setFace(FS_TEXT);
  uint32_t until = millis() + (uint32_t)pages * 6000 + 2000;
  while (millis() < until && !g_abort) vTaskDelay(pdMS_TO_TICKS(100));
  setFace(FS_IDLE);
  setStatus("");
  vTaskDelay(pdMS_TO_TICKS(150));
  g_lines.clear();
}

static void speakAnswer(const String& answerClean) {
  String speech = forSpeech(answerClean);
  std::vector<String> parts;
  splitForTts(speech, parts, 110, 200);
  if (parts.empty()) return;
  setFace(FS_SPEAKING);
  bool ttsFailed = false; String ttsErr;
  for (size_t i = 0; i < parts.size() && !g_abort; i++) {
    PsramSink sink; String err;
    if (!ttsFetch(parts[i], sink, err)) { ttsFailed = true; ttsErr = err; sink.release(); break; }
    bool ok = playAudioBuf(sink.d, sink.n, err);
    sink.release();
    if (!ok && !g_abort) { ttsFailed = true; ttsErr = err; break; }
  }
  spkEnd();
  g_ttsClient.stop();
  g_mouth = 0;
  if (ttsFailed) {
    Serial.println("[TTS] gagal: " + ttsErr);
    showTextFallback(answerClean, ttsErr);   // tetap tampilkan jawaban di layar
    return;
  }
  if (g_abort) { setFace(FS_IDLE); setStatus(""); return; }
  setFace(FS_HAPPY);
  vTaskDelay(pdMS_TO_TICKS(1800));
  setFace(FS_IDLE);
  setStatus("");
}

static void runInteraction(bool photoMode) {
  g_abort = false;
  if (g_portal) return;
  if (WiFi.status() != WL_CONNECTED) { showError("Wi-Fi belum terhubung"); return; }
  if (g_key1.length() == 0) { showError("API key kosong - tahan tombol 3 dtk"); return; }
  if (!g_micOk) { showError("Mikrofon error"); return; }

  Photo ph; bool havePhoto = false;
  if (photoMode) {
    if (!g_camOk) { showError("Kamera error"); return; }
    setFace(FS_IDLE); setStatus("Arahkan ke soal...");
    beep(1200, 70);
    vTaskDelay(pdMS_TO_TICKS(900));
    if (g_abort) { setStatus(""); return; }
    setStatus("Ambil foto...");
    havePhoto = capturePhoto(ph);
    if (!havePhoto) { showError("Gagal ambil foto"); return; }
    beep(1800, 60);
  }

  setFace(FS_LISTENING);
  setStatus(photoMode ? "Tanyakan soalnya..." : "Silakan bicara...");
  beep(1000, 70);
  vTaskDelay(pdMS_TO_TICKS(120));
  Rec rec;
  bool okRec = recordUtterance(rec);
  g_mic = 0;
  if (!okRec) { ph.free_(); showError("Gagal merekam"); return; }
  if (g_abort) { ph.free_(); rec.free_(); setFace(FS_IDLE); setStatus(""); return; }

  String userText;
  if (!rec.speech) {
    if (!havePhoto) {
      setFace(FS_IDLE); setStatus("Tidak ada suara");
      vTaskDelay(pdMS_TO_TICKS(1300)); setStatus("");
      return;
    }
    userText = "Tidak ada pertanyaan suara. Lihat foto: jika ada soal sekolah, jawab dengan langkah ringkas; jika bukan, jelaskan isi foto.";
  } else {
    userText = havePhoto ? "Pengguna mengirim foto dan merekam pertanyaannya sebagai audio. Jawab pertanyaan dalam audio terkait foto ini."
                         : "Pengguna berbicara lewat audio. Jawab ucapannya.";
  }

  setFace(FS_THINKING);
  setStatus("Berpikir...");
  AiReply r = askAI(userText, havePhoto ? &ph : nullptr, rec.speech ? &rec : nullptr);
  ph.free_(); rec.free_();
  if (g_abort) { setFace(FS_IDLE); setStatus(""); return; }
  if (!r.ok) { showError(r.err.length() ? r.err : String("AI gagal")); return; }

  String clean = cleanMd(r.text), heard, ans;
  parseTagged(clean, heard, ans);
  if (ans.length() == 0) { showError("Jawaban AI kosong"); return; }
  String hu = heard.length() ? heard : String(rec.speech ? "(suara)" : "(tanpa suara)");
  if (havePhoto) hu = "[foto] " + hu;
  histAdd(hu, ans);
  Serial.println("[AI] didengar: " + heard);
  Serial.println("[AI] jawab: " + ans);

  setStatus(heard.length() ? ("Q: " + cutUtf8(toAscii(heard), 34)) : String("Menjawab..."));
  speakAnswer(ans);
}

static void aiTask(void*) {
  for (;;) {
    int m = g_pendingMode;
    if (m) {
      g_pendingMode = 0;
      if (m == 3) {
        histClear();
        beep(700, 80); beep(500, 120);
        setStatus("Memori dihapus");
        vTaskDelay(pdMS_TO_TICKS(1200));
        setStatus("");
      } else runInteraction(m == 2);
      g_busy = false;
      g_abort = false;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

// ==========================================================================
//  PORTAL SETUP (CAPTIVE PORTAL)
// ==========================================================================
static String htmlEsc(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;"; else o += c;
  }
  return o;
}

static const char PAGE_HEAD[] = R"HTML(<!doctype html><html lang="id"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>DEWZX Setup</title><style>
body{font-family:system-ui,sans-serif;background:#0b1020;color:#e8f0ff;margin:0;padding:16px}
.c{max-width:460px;margin:auto}h1{color:#35e0ff;margin:0 0 4px}small,.note{color:#8fa3c7}
label{display:block;margin:14px 0 4px;font-weight:600}
input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid #2a3a63;background:#121a33;color:#fff;font-size:16px}
button{width:100%;margin-top:20px;padding:14px;border:0;border-radius:12px;background:#35e0ff;color:#04121a;font-size:17px;font-weight:700}
details{margin-top:18px;border:1px solid #2a3a63;border-radius:10px;padding:8px 12px}summary{cursor:pointer;color:#35e0ff;padding:6px 0}
.note{font-size:13px;margin-top:4px}.ok{color:#5cff9d}
</style></head><body><div class="c"><h1>DEWZX VOICE AI</h1><small>Pengaturan perangkat</small>
<form method="POST" action="/save">)HTML";

static String buildPage() {
  String h = String(PAGE_HEAD);
  h += "<label>Nama Wi-Fi (SSID)</label><input name=ssid list=nets required autocapitalize=off autocomplete=off value=\"" + htmlEsc(g_ssid) + "\"><datalist id=nets></datalist>";
  h += "<div class=note id=sc>Mencari jaringan...</div>";
  h += "<label>Password Wi-Fi</label><input name=pass type=password placeholder=\"(kosongkan jika tidak diubah)\">";
  h += "<label>API Key Qwen3.8-Omni-Flash</label><input name=k1 type=password placeholder=\"";
  h += g_key1.length() ? "tersimpan - kosongkan jika tidak diubah" : "tempel API key";
  h += "\"><label>API Key Cicada-TTS <small>(opsional)</small></label><input name=k2 type=password placeholder=\"";
  h += g_key2.length() ? "tersimpan - kosongkan jika tidak diubah" : "kosong = pakai API key pertama";
  h += "\"><label style='font-weight:400'><input type=checkbox name=clr2 value=1 style='width:auto'> Hapus API key kedua</label>";
  h += "<div class=note>Base URL: " + htmlEsc(String(BASE_URL)) + " (tertanam di firmware)</div>";
  h += "<details><summary>Pengaturan lanjutan</summary>";
  h += "<label>Model chat</label><input name=cm value=\"" + htmlEsc(g_chatModel) + "\">";
  h += "<label>Path TTS</label><input name=tp value=\"" + htmlEsc(g_ttsPath) + "\">";
  h += "<label>Model TTS</label><input name=tm value=\"" + htmlEsc(g_ttsModel) + "\">";
  h += "<label>Voice TTS <small>(kosong = tidak dikirim)</small></label><input name=tv value=\"" + htmlEsc(g_ttsVoice) + "\">";
  h += "<label>Format audio TTS <small>(harus wav)</small></label><input name=tf value=\"" + htmlEsc(g_ttsFmt) + "\">";
  h += "<label>Sample rate jika audio tanpa header</label><input name=tr type=number value=\"" + String(g_ttsRate) + "\">";
  h += "<label>Volume (0-100)</label><input name=vol type=number min=0 max=100 value=\"" + String(g_vol) + "\">";
  h += "<label>Format base64 audio</label><select name=ap><option value=1";
  h += g_audPrefix ? " selected" : "";
  h += ">data:;base64, (gaya DashScope)</option><option value=0";
  h += g_audPrefix ? "" : " selected";
  h += ">base64 polos (gaya OpenAI)</option></select>";
  h += "<label>Respons streaming</label><select name=st><option value=1";
  h += g_stream ? " selected" : "";
  h += ">Ya</option><option value=0";
  h += g_stream ? "" : " selected";
  h += ">Tidak</option></select></details>";
  h += "<button type=submit>Simpan & Restart</button></form>";
  h += R"HTML(<script>
var tries=0;function sc(){fetch('/scan').then(r=>r.json()).then(a=>{var d=document.getElementById('nets');d.innerHTML='';a.forEach(function(s){var o=document.createElement('option');o.value=s;d.appendChild(o)});
document.getElementById('sc').textContent=a.length?('Ditemukan '+a.length+' jaringan (ketuk kolom SSID)'):'Mencari jaringan...';
if(!a.length&&tries++<6)setTimeout(sc,3000)}).catch(function(){})}sc();
</script></div></body></html>)HTML";
  return h;
}

static void webHit() { g_lastWebHit = millis(); }

static void handleRoot() { webHit(); server.send(200, "text/html; charset=utf-8", buildPage()); }

static void handleScan() {
  webHit();
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) { WiFi.scanNetworks(true); server.send(200, "application/json", "[]"); return; }
  if (n == WIFI_SCAN_RUNNING) { server.send(200, "application/json", "[]"); return; }
  String j = "[";
  int cnt = 0;
  for (int i = 0; i < n && cnt < 25; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    s.replace("\\", "\\\\"); s.replace("\"", "\\\"");
    if (cnt++) j += ",";
    j += "\"" + s + "\"";
  }
  j += "]";
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  server.send(200, "application/json", j);
}

static String trimmedArg(const char* name) { String v = server.arg(name); v.trim(); return v; }

static void handleSave() {
  webHit();
  String ssid = trimmedArg("ssid");
  if (!ssid.length()) { server.send(400, "text/plain", "SSID wajib diisi"); return; }
  String p = server.arg("pass");
  if (p.length() == 0 && ssid == g_ssid) p = g_pass;
  g_ssid = ssid; g_pass = p;
  String k1 = trimmedArg("k1"); if (k1.length()) g_key1 = k1;
  String k2 = trimmedArg("k2"); if (k2.length()) g_key2 = k2;
  if (server.arg("clr2") == "1") g_key2 = "";
  String v;
  v = trimmedArg("cm"); if (v.length()) g_chatModel = v;
  v = trimmedArg("tp"); if (v.length()) { if (v[0] != '/') v = "/" + v; g_ttsPath = v; }
  v = trimmedArg("tm"); if (v.length()) g_ttsModel = v;
  g_ttsVoice = trimmedArg("tv");
  v = trimmedArg("tf"); if (v.length()) g_ttsFmt = v;
  v = trimmedArg("tr"); if (v.length()) g_ttsRate = constrain(v.toInt(), 8000, 48000);
  v = trimmedArg("vol"); if (v.length()) g_vol = constrain(v.toInt(), 0, 100);
  if (server.hasArg("ap")) g_audPrefix = (server.arg("ap") == "1");
  if (server.hasArg("st")) g_stream = (server.arg("st") == "1");
  cfgSave();
  server.send(200, "text/html; charset=utf-8",
              "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
              "<body style='font-family:system-ui;background:#0b1020;color:#e8f0ff;padding:24px'>"
              "<h2 style='color:#5cff9d'>Tersimpan!</h2><p>DEWZX restart dan menyambung ke Wi-Fi. Kamu boleh menutup halaman ini.</p></body>");
  g_restartAt = millis() + 1500;
}

static void handleNotFound() {   // captive portal: arahkan semua ke halaman setup
  webHit();
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

static void startPortal(bool forced) {
  if (g_portal) { g_portalForced = g_portalForced || forced; return; }
  g_portal = true; g_portalForced = forced; g_lastWebHit = millis();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(150);
  dns.start(53, "*", WiFi.softAPIP());
  server.begin();
  WiFi.scanNetworks(true);
  setFace(FS_SETUP);
  Serial.printf("[PORTAL] AP %s / %s  -> http://%s\n", AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
}
static void stopPortal() {
  if (!g_portal) return;
  server.stop(); dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  g_portal = false; g_portalForced = false;
  setFace(FS_IDLE); setStatus("");
}

// ==========================================================================
//  WI-FI + TOMBOL
// ==========================================================================
static void wifiMaintain() {
  static uint32_t lastTry = 0;
  bool conn = (WiFi.status() == WL_CONNECTED);
  if (!conn && g_ssid.length() && millis() - lastTry > (g_portal ? 30000UL : 15000UL)) {
    lastTry = millis();
    WiFi.disconnect(false, false);
    WiFi.begin(g_ssid.c_str(), g_pass.c_str());
  }
  if (g_portal && !g_portalForced && conn && millis() - g_lastWebHit > 120000UL) stopPortal();
}

static void onClicks(int c) {
  if (g_portal) return;
  if (g_busy || g_pendingMode) { g_abort = true; return; }   // ketuk saat sibuk = batalkan
  g_busy = true;
  g_pendingMode = (c == 1) ? 1 : (c == 2 ? 2 : 3);
}

static void handleButton() {
  static bool down = false, longFired = false;
  static uint32_t t0 = 0, lastRel = 0;
  static int clicks = 0;
  bool pressed = (digitalRead(BTN_PIN) == LOW);
  uint32_t now = millis();
  if (pressed && !down) { down = true; t0 = now; longFired = false; }
  else if (pressed && down) {
    if (!longFired && now - t0 >= 3000) { longFired = true; clicks = 0; if (g_busy) g_abort = true; startPortal(true); }
  } else if (!pressed && down) {
    down = false;
    uint32_t d = now - t0;
    if (!longFired && d >= 30 && d < 1500) { clicks++; lastRel = now; }
  }
  if (!down && clicks > 0 && now - lastRel >= 380) { int c = clicks; clicks = 0; onClicks(c); }
}

// ==========================================================================
//  SETUP & LOOP
// ==========================================================================
void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  // LCD
  lcdSPI.begin(PIN_LCD_SCK, -1, PIN_LCD_MOSI, -1);
  tft.init(240, 240, LCD_SPI_MODE);
  tft.setSPISpeed(LCD_SPI_HZ);
  tft.setRotation(0);
  tft.fillScreen(0);
  g_cv = new GFXcanvas16(240, 240);
  if (!g_cv || !g_cv->getBuffer()) {
    tft.fillScreen(ST77XX_RED);
    Serial.println("Canvas gagal dialokasikan");
    while (true) delay(1000);
  }
  setStatus("Memulai...");
  xTaskCreatePinnedToCore(faceTask, "face", 8192, nullptr, 1, nullptr, 0);

  if (!psramFound()) {
    setStatus("PSRAM mati! set OPI PSRAM");
    Serial.println("PSRAM tidak terdeteksi - atur Tools > PSRAM di compile");
    delay(4000);
  }

  cfgLoad();
  histLoad();
  g_chatClient.setInsecure();   // TANPA validasi sertifikat (lihat catatan keamanan)
  g_ttsClient.setInsecure();

  setStatus("Kamera...");
  g_camOk = camInit();
  setStatus("Audio...");
  g_micOk = micInit();
  g_spkOk = spkInit();
  Serial.printf("[INIT] cam=%d mic=%d spk=%d psram=%u\n", g_camOk, g_micOk, g_spkOk, (unsigned)ESP.getPsramSize());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);

  WiFi.persistent(false);
  WiFi.setHostname("dewzx");
  WiFi.setAutoReconnect(true);
  if (g_ssid.length() == 0) {
    startPortal(false);
  } else {
    setStatus("Menyambung Wi-Fi...");
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(g_ssid.c_str(), g_pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
    if (WiFi.status() != WL_CONNECTED) startPortal(false);
    else Serial.println("[WIFI] terhubung: " + WiFi.localIP().toString());
  }

  xTaskCreatePinnedToCore(aiTask, "ai", 32768, nullptr, 2, nullptr, 1);
  if (!g_portal) {
    setStatus("");
    setFace(FS_IDLE);
    if (g_key1.length() == 0) { setStatus("API key belum diisi"); }
  }
}

void loop() {
  if (g_portal) { dns.processNextRequest(); server.handleClient(); }
  handleButton();
  wifiMaintain();
  if (g_restartAt && millis() > g_restartAt) ESP.restart();
  delay(5);
}
