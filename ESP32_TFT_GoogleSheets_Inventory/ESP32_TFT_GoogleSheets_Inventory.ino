/*
  ESP32 + 2.8" ILI9341 TFT + XPT2046 touch + Google Sheets
  Temporary input: Arduino Serial Monitor, 115200 baud, Newline.
  After tapping STOCK IN or STOCK OUT, type ITEM001 and press Enter.
  You can also type IN ITEM001 or OUT ITEM001 to override the mode.
  Type CAL to redo touchscreen calibration.
  Based on the Google Apps Script API used in this conversation:
  POST JSON {key, id, action, qty, device}
  Response JSON {ok, item_name, stock} or {ok:false,error}.

  Required Arduino libraries:
    Adafruit GFX Library, Adafruit ILI9341, XPT2046_Touchscreen,
    ArduinoJson (version 7), ESP32 board platform (includes Preferences).

  WARNING: setInsecure() is for a controlled prototype only. For a real
  deployment replace it with proper TLS certificate verification.
*/

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>

// -------- EDIT THE FOUR CREDENTIAL/URL VALUES BELOW --------
const char* WIFI_SSID  = "ARCEGA WIFI";
const char* WIFI_PASS  = "@Arcega2023";
const char* WEB_APP_URL = "https://script.google.com/macros/s/AKfycbz6LokGVBPKzSOLGwnbmDTsDAa5LUR7PQTsbzj5jQyBZGTRuPPOx7IIGILHcp2ycEfS/exec";
const char* API_KEY    = "This_is_a_secret_key";
const char* DEVICE_ID  = "SCANNER01";

// -------- SAME PINS AS THE SUCCESSFUL TFT TEST --------
#define TFT_CS    5
#define TFT_DC    27
#define TFT_RST   26
#define TOUCH_CS  33
#define TOUCH_IRQ 25
#define SPI_SCK   18
#define SPI_MISO  19
#define SPI_MOSI  23

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
Preferences prefs;

const uint16_t SCREEN_BG = ILI9341_BLACK;
const uint16_t PANEL_BG = 0x18E3;
const uint16_t HEADER_BG = ILI9341_NAVY;
const uint16_t GREEN = 0x0440;
const uint16_t RED = 0xB104;
const uint16_t GREY = 0x4208;

// Three-point touchscreen calibration, saved in ESP32 flash.
struct RawPoint { int16_t x, y; };
struct TouchCal {
  uint32_t magic;
  RawPoint topLeft, topRight, bottomLeft;
};
TouchCal calibration;
const uint32_t CAL_MAGIC = 0x494E5631;
const int CAL_LEFT = 35, CAL_RIGHT = 285;
const int CAL_TOP = 45, CAL_BOTTOM = 205;

bool stockInMode = false; // Default operation: STOCK OUT
bool touchHeld = false;
String serialInput = "";
String statusTitle = "Ready";
String statusLine1 = "Tap STOCK IN / STOCK OUT";
String statusLine2 = "Then type ITEM001 in Serial";
uint16_t statusColor = ILI9341_CYAN;
uint32_t lastWifiCheck = 0;
bool wifiWasConnected = false;

String shorten(String s, unsigned int limit) {
  if (s.length() > limit) return s.substring(0, limit - 3) + "...";
  return s;
}

float calDet() {
  float dx1 = calibration.topRight.x - calibration.topLeft.x;
  float dy1 = calibration.topRight.y - calibration.topLeft.y;
  float dx2 = calibration.bottomLeft.x - calibration.topLeft.x;
  float dy2 = calibration.bottomLeft.y - calibration.topLeft.y;
  return dx1 * dy2 - dy1 * dx2;
}

RawPoint captureCalibrationPoint(const char* name, int x, int y) {
  tft.fillScreen(SCREEN_BG);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(12, 10);
  tft.print("Calibrate: ");
  tft.println(name);
  tft.drawCircle(x, y, 12, ILI9341_WHITE);
  tft.drawFastHLine(x - 18, y, 36, ILI9341_YELLOW);
  tft.drawFastVLine(x, y - 18, 36, ILI9341_YELLOW);
  tft.setTextSize(1);
  tft.setCursor(40, 226);
  tft.print("Touch the cross, then release");

  while (touch.touched()) delay(20); // require finger release
  delay(150);
  while (!touch.touched()) delay(15);
  delay(60); // allow contact to settle

  long totalX = 0, totalY = 0;
  for (int i = 0; i < 5; ++i) {
    TS_Point p = touch.getPoint();
    totalX += p.x;
    totalY += p.y;
    delay(15);
  }
  while (touch.touched()) delay(20);
  delay(150);
  RawPoint result = {(int16_t)(totalX / 5), (int16_t)(totalY / 5)};
  Serial.printf("%s raw: X=%d Y=%d\n", name, result.x, result.y);
  return result;
}

void calibrateTouch() {
  do {
    Serial.println("Calibrate: touch the 3 displayed crosses.");
    calibration.topLeft = captureCalibrationPoint("1 of 3", CAL_LEFT, CAL_TOP);
    calibration.topRight = captureCalibrationPoint("2 of 3", CAL_RIGHT, CAL_TOP);
    calibration.bottomLeft = captureCalibrationPoint("3 of 3", CAL_LEFT, CAL_BOTTOM);
    if (fabsf(calDet()) < 10000.0f) {
      Serial.println("Calibration failed; try again.");
      tft.fillScreen(ILI9341_RED);
      tft.setTextColor(ILI9341_WHITE);
      tft.setTextSize(2);
      tft.setCursor(40, 100);
      tft.print("Please try again");
      delay(1400);
    }
  } while (fabsf(calDet()) < 10000.0f);

  calibration.magic = CAL_MAGIC;
  prefs.putBytes("touch-cal", &calibration, sizeof(calibration));
  Serial.println("Touch calibration saved!");
}

bool touchToPixel(TS_Point raw, int& px, int& py) {
  float det = calDet();
  if (fabsf(det) < 10000.0f) return false;

  float dx1 = calibration.topRight.x - calibration.topLeft.x;
  float dy1 = calibration.topRight.y - calibration.topLeft.y;
  float dx2 = calibration.bottomLeft.x - calibration.topLeft.x;
  float dy2 = calibration.bottomLeft.y - calibration.topLeft.y;
  float rx = raw.x - calibration.topLeft.x;
  float ry = raw.y - calibration.topLeft.y;

  float u = (rx * dy2 - ry * dx2) / det;
  float v = (dx1 * ry - dy1 * rx) / det;
  px = constrain((int)lroundf(CAL_LEFT + u * (CAL_RIGHT - CAL_LEFT)), 0, 319);
  py = constrain((int)lroundf(CAL_TOP + v * (CAL_BOTTOM - CAL_TOP)), 0, 239);
  return true;
}

void drawWifi() {
  tft.fillRect(245, 6, 72, 25, HEADER_BG);
  tft.setTextSize(1);
  bool online = (WiFi.status() == WL_CONNECTED);
  tft.setTextColor(online ? ILI9341_GREEN : ILI9341_RED);
  tft.setCursor(257, 16);
  tft.print(online ? "ONLINE" : "OFFLINE");
}

void drawButtons() {
  tft.fillRoundRect(12, 67, 142, 55, 7, stockInMode ? GREEN : GREY);
  tft.fillRoundRect(166, 67, 142, 55, 7, !stockInMode ? RED : GREY);
  tft.drawRoundRect(12, 67, 142, 55, 7, ILI9341_WHITE);
  tft.drawRoundRect(166, 67, 142, 55, 7, ILI9341_WHITE);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(33, 85);
  tft.print("STOCK IN");
  tft.setCursor(180, 85);
  tft.print("STOCK OUT");
}

void drawStatus() {
  tft.fillRoundRect(10, 151, 300, 79, 7, PANEL_BG);
  tft.drawRoundRect(10, 151, 300, 79, 7, ILI9341_DARKCYAN);
  tft.setTextSize(2);
  tft.setTextColor(statusColor);
  tft.setCursor(20, 161);
  tft.print(shorten(statusTitle, 23));
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(20, 193);
  tft.print(shorten(statusLine1, 45));
  tft.setCursor(20, 212);
  tft.print(shorten(statusLine2, 45));
}

void setStatus(String title, String line1, String line2, uint16_t color) {
  statusTitle = title;
  statusLine1 = line1;
  statusLine2 = line2;
  statusColor = color;
  drawStatus();
}

void drawScreen() {
  tft.fillScreen(SCREEN_BG);
  tft.fillRect(0, 0, 320, 38, HEADER_BG);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(11, 11);
  tft.print("INVENTORY");
  drawWifi();
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(14, 49);
  tft.print("Choose the inventory operation:");
  drawButtons();
  tft.setCursor(14, 134);
  tft.print("Serial: ITEM001 / IN ITEM001 / OUT ITEM001");
  drawStatus();
}

// Read Apps Script's JSON response after Google's redirect.
// Never re-send a POST automatically if its result is uncertain.
String getRedirectedResponse(String url) {
  for (int i = 0; i < 4; ++i) {
    if (!url.startsWith("https://")) return "";
    WiFiClientSecure client;
    client.setInsecure(); // Prototype only
    HTTPClient http;
    if (!http.begin(client, url)) return "";
    const char* keys[] = {"Location"};
    http.collectHeaders(keys, 1);
    http.setTimeout(20000);
    int code = http.GET();
    if (code == 200) {
      String body = http.getString();
      http.end();
      return body;
    }
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
      String nextUrl = http.header("Location");
      http.end();
      url = nextUrl;
    } else {
      Serial.printf("Redirect GET failed: HTTP %d\n", code);
      http.end();
      return "";
    }
  }
  return "";
}

void updateInventory(const String& action, const String& itemID) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: Wi-Fi is offline.");
    setStatus("Wi-Fi offline", itemID, "Reconnect before scanning", ILI9341_RED);
    WiFi.reconnect();
    return;
  }

  setStatus("Sending to Sheets", itemID, "Please wait...", ILI9341_YELLOW);
  JsonDocument request;
  request["key"] = API_KEY;
  request["id"] = itemID;
  request["action"] = action;
  request["qty"] = 1;
  request["device"] = DEVICE_ID;
  String payload;
  serializeJson(request, payload);

  WiFiClientSecure client;
  client.setInsecure(); // Prototype only
  HTTPClient http;
  if (!http.begin(client, WEB_APP_URL)) {
    Serial.println("Could not open Google Apps Script URL.");
    setStatus("Connection error", itemID, "Check your Web App URL", ILI9341_RED);
    return;
  }
  const char* keys[] = {"Location"};
  http.collectHeaders(keys, 1);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(20000);
  int code = http.POST(payload);
  String response = "";
  String nextUrl = "";
  if (code == 200) response = http.getString();
  else if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
    nextUrl = http.header("Location");
  else Serial.printf("POST returned HTTP %d\n", code);
  http.end();

  if (!nextUrl.isEmpty()) response = getRedirectedResponse(nextUrl);
  if (response.isEmpty()) {
    Serial.println("UNKNOWN RESULT: check Sheets before trying again.");
    setStatus("Result unknown", itemID, "Check Sheet before retry", ILI9341_ORANGE);
    return;
  }

  JsonDocument result;
  DeserializationError err = deserializeJson(result, response);
  if (err) {
    Serial.println("Invalid JSON: " + shorten(response, 160));
    setStatus("Invalid response", itemID, "See Arduino Serial Monitor", ILI9341_RED);
    return;
  }

  if (result["ok"].as<bool>()) {
    String itemName = result["item_name"].as<String>();
    int stock = result["stock"].as<int>();
    Serial.printf("SUCCESS: %s %s (%s), new stock = %d\n",
                  action.c_str(), itemID.c_str(), itemName.c_str(), stock);
    setStatus("UPDATED: " + action, itemID + " - " + itemName,
              "Current stock: " + String(stock), ILI9341_GREEN);
  } else {
    String error = result["error"].as<String>();
    Serial.println("API error: " + error);
    setStatus("Not updated", itemID, error, ILI9341_RED);
  }
}

void setMode(bool stockIn) {
  stockInMode = stockIn;
  drawButtons();
  setStatus(stockIn ? "STOCK IN selected" : "STOCK OUT selected",
            "Waiting for item ID", "Type ITEM001 in Serial", ILI9341_CYAN);
  Serial.printf("Mode: %s. Enter an item ID.\n", stockIn ? "IN" : "OUT");
}

void handleTouch() {
  bool touching = touch.touched();
  if (touching && !touchHeld) {
    TS_Point p = touch.getPoint();
    int x, y;
    if (touchToPixel(p, x, y)) {
      Serial.printf("Touch: x=%d y=%d\n", x, y);
      if (y >= 67 && y <= 122) {
        if (x >= 12 && x <= 154) setMode(true);
        else if (x >= 166 && x <= 308) setMode(false);
      }
    }
    touchHeld = true; // One action for each press; wait for release
  } else if (!touching) {
    touchHeld = false;
  }
}

void processCommand(String command) {
  command.trim();
  if (command.isEmpty()) return;

  if (command.equalsIgnoreCase("CAL")) {
    calibrateTouch();
    touchHeld = false;
    drawScreen();
    return;
  }

  String action = stockInMode ? "IN" : "OUT";
  String itemID = command;
  int space = command.indexOf(' ');
  if (space > 0) {
    String prefix = command.substring(0, space);
    prefix.toUpperCase();
    if (prefix == "IN" || prefix == "OUT") {
      action = prefix;
      itemID = command.substring(space + 1);
      itemID.trim();
      if (stockInMode != (action == "IN")) setMode(action == "IN");
    }
  }
  if (itemID.isEmpty() || itemID.indexOf(' ') >= 0 || itemID.length() > 50) {
    Serial.println("Invalid item ID. Example: ITEM001");
    setStatus("Invalid item ID", "Try ITEM001", "No transaction sent", ILI9341_RED);
    return;
  }
  Serial.println("Processing: " + action + " " + itemID);
  updateInventory(action, itemID);
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("ESP32 TFT + Google Sheets Inventory");

  pinMode(TFT_CS, OUTPUT);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TOUCH_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  tft.begin();
  tft.setRotation(1);  // Landscape: 320x240
  touch.begin();
  touch.setRotation(1);

  prefs.begin("inventory", false);
  bool stored = prefs.getBytesLength("touch-cal") == sizeof(calibration);
  if (stored) prefs.getBytes("touch-cal", &calibration, sizeof(calibration));
  if (!stored || calibration.magic != CAL_MAGIC || fabsf(calDet()) < 10000.0f)
    calibrateTouch();

  drawScreen();
  Serial.println("Connecting to Wi-Fi...");
  setStatus("Connecting Wi-Fi", "Please wait...", "", ILI9341_YELLOW);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 18000) {
    delay(400);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
    Serial.println("Wi-Fi connected: " + WiFi.localIP().toString());
  else
    Serial.println("Wi-Fi not connected. Check credentials.");

  wifiWasConnected = (WiFi.status() == WL_CONNECTED);
  drawWifi();
  setStatus("Ready", "Tap Stock In or Stock Out", "Type ITEM001 in Serial", ILI9341_CYAN);
  Serial.println("Commands: ITEM001 | IN ITEM001 | OUT ITEM001 | CAL");
  Serial.println("Serial Monitor: 115200 baud, Newline");
}

void loop() {
  handleTouch();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (!serialInput.isEmpty()) {
        processCommand(serialInput);
        serialInput = "";
      }
    } else if (c >= 32 && serialInput.length() < 80) {
      serialInput += c;
    }
  }

  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    bool online = (WiFi.status() == WL_CONNECTED);
    if (online != wifiWasConnected) {
      wifiWasConnected = online;
      drawWifi();
      Serial.println(online ? "Wi-Fi restored" : "Wi-Fi lost");
    }
    if (!online) WiFi.reconnect();
  }
  delay(20);
}
