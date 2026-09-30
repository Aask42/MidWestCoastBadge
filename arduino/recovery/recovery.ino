#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#define TFT_SCK 6
#define TFT_MOSI 7
#define TFT_DC 10
#define TFT_RST 8
#define TFT_BL 0
#define SCREEN_W 240
#define SCREEN_H 320

constexpr uint16_t C_BG = 0x1082;
constexpr uint16_t C_FG = 0xFFFF;
constexpr uint16_t C_ACCENT = 0x07FF;
constexpr uint16_t C_OK = 0x07E0;
constexpr uint16_t C_WARN = 0xF800;
constexpr uint32_t WIFI_TIMEOUT_MS = 30000;

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, GFX_NOT_DEFINED, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX *panel = new Arduino_ST7789(
    bus, TFT_RST, 0, true, SCREEN_W, SCREEN_H);

Preferences request;
char updateUrl[160] = "";
char updateMd5[40] = "";

void centered(const char *text, int y, uint8_t size, uint16_t color) {
  panel->setTextSize(size);
  panel->setTextColor(color);
  panel->setCursor((SCREEN_W - strlen(text) * 6 * size) / 2, y);
  panel->print(text);
}

void statusScreen(const char *title, const char *detail, uint16_t color) {
  panel->fillScreen(C_BG);
  centered("DC34 RECOVERY", 34, 2, C_ACCENT);
  panel->drawFastHLine(20, 62, SCREEN_W - 40, C_FG);
  centered(title, 112, 2, color);
  centered(detail, 150, 1, C_FG);
}

void fail(const char *message) {
  Serial.printf("recovery: %s\n", message);
  request.putString("error", message);
  statusScreen("UPDATE FAILED", message, C_WARN);
  centered("connect USB to recover", 190, 1, C_FG);
  while (true) delay(1000);
}

const esp_partition_t *mainPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
}

bool parseHttpUrl(const char *url, char *host, size_t hostSize,
                  uint16_t &port, const char *&path) {
  constexpr char PREFIX[] = "http://";
  if (strncmp(url, PREFIX, sizeof(PREFIX) - 1) != 0) return false;
  const char *start = url + sizeof(PREFIX) - 1;
  path = strchr(start, '/');
  const char *end = path ? path : start + strlen(start);
  const char *colon = static_cast<const char *>(memchr(start, ':', end - start));
  const char *hostEnd = colon ? colon : end;
  if (hostEnd == start || (size_t)(hostEnd - start) >= hostSize) return false;
  memcpy(host, start, hostEnd - start);
  host[hostEnd - start] = '\0';
  port = colon ? (uint16_t)atoi(colon + 1) : 80;
  if (!path) path = "/";
  return port != 0;
}

int readHttpLength(WiFiClient &client) {
  char line[128];
  size_t used = client.readBytesUntil('\n', line, sizeof(line) - 1);
  line[used] = '\0';
  int status = 0;
  if (sscanf(line, "HTTP/%*s %d", &status) != 1 || status != 200) return -1;

  int length = -1;
  while (true) {
    used = client.readBytesUntil('\n', line, sizeof(line) - 1);
    line[used] = '\0';
    if (used == 0 || (used == 1 && line[0] == '\r')) break;
    if (strncasecmp(line, "Content-Length:", 15) == 0) {
      length = atoi(line + 15);
    }
  }
  return length;
}

void bootMain() {
  const esp_partition_t *main = mainPartition();
  if (!main || esp_ota_set_boot_partition(main) != ESP_OK) {
    fail("main partition missing");
  }
  delay(250);
  ESP.restart();
  while (true) delay(1000);
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  panel->begin(80000000);
  statusScreen("STARTING", "checking update request", C_ACCENT);

  request.begin("recovery", false);
  const String phase = request.getString("phase", "");
  if (phase == "booting") {
    fail("new firmware did not start");
  }
  if (phase != "download") {
    statusScreen("NO UPDATE", "starting main firmware", C_OK);
    bootMain();
  }

  request.getString("url", updateUrl, sizeof(updateUrl));
  request.getString("md5", updateMd5, sizeof(updateMd5));
  if (!updateUrl[0]) fail("update URL missing");

  Preferences settings;
  char ssid[33] = "";
  char password[64] = "";
  settings.begin("badge", true);
  settings.getString("ssid", ssid, sizeof(ssid));
  settings.getString("wpw", password, sizeof(password));
  settings.end();
  if (!ssid[0]) fail("WiFi settings missing");

  statusScreen("CONNECTING", ssid, C_ACCENT);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startedAt < WIFI_TIMEOUT_MS) {
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) fail("WiFi timeout");

  statusScreen("DOWNLOADING", "do not remove power", C_ACCENT);
  char host[64];
  uint16_t port;
  const char *path;
  if (!parseHttpUrl(updateUrl, host, sizeof(host), port, path)) {
    fail("only plain HTTP supported");
  }
  WiFiClient client;
  client.setTimeout(15);
  if (!client.connect(host, port)) fail("update server unreachable");
  client.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                path, host);
  const int length = readHttpLength(client);
  const esp_partition_t *main = mainPartition();
  if (!main || length <= 0 || (size_t)length > main->size) {
    client.stop();
    fail("firmware size invalid");
  }
  if (!Update.begin((size_t)length, U_FLASH)) {
    client.stop();
    fail("flash write refused");
  }
  if (updateMd5[0]) Update.setMD5(updateMd5);

  const size_t written = Update.writeStream(client);
  client.stop();
  if (written != (size_t)length) {
    Update.abort();
    fail("firmware download short");
  }
  if (!Update.end(true)) fail("firmware verification failed");

  request.putString("phase", "booting");
  request.remove("error");
  statusScreen("VERIFIED", "starting new firmware", C_OK);
  delay(750);
  bootMain();
}

void loop() { delay(1000); }