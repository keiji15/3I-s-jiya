/*
  ESP32 + 2.8" ILI9341 TFT + XPT2046 touch + Google Sheets
  Temporary input: Arduino Serial Monitor, 115200 baud, Newline.
  Tap STOCK IN / STOCK OUT, then SINGLE / BULK.
  In BULK mode, tap the quantity box and enter 1-100 using keypad.
  In Serial Monitor, type ITEM001 to simulate one QR scan.
  Optional commands: IN ITEM001 50, OUT ITEM001 5, QTY 50,
  SINGLE, BULK, CAL.
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
#include <cstring>
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
bool bulkMode = false;    // Default: 1 unit per scan
int bulkQuantity = 1;    // Apps Script currently permits 1..100

enum UiPage { PAGE_HOME, PAGE_KEYPAD };
UiPage currentPage = PAGE_HOME;
String qtyEntry = "";
bool keypadError = false;
bool touchHeld = false;
String serialInput = "";
String statusTitle = "Ready";
String statusLine1 = "Tap a mode and select quantity";
String statusLine2 = "Serial Monitor: ITEM001";
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

// ---------- HOME SCREEN: 320 x 240 landscape ----------
void drawButtons() {
  tft.fillRoundRect(10, 39, 145, 44, 6, stockInMode ? GREEN : GREY);
  tft.fillRoundRect(165, 39, 145, 44, 6, !stockInMode ? RED : GREY);
  tft.drawRoundRect(10, 39, 145, 44, 6, ILI9341_WHITE);
  tft.drawRoundRect(165, 39, 145, 44, 6, ILI9341_WHITE);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(30, 53); tft.print("STOCK IN");
  tft.setCursor(178, 53); tft.print("STOCK OUT");
}

void drawScanMode() {
  tft.fillRoundRect(10, 91, 145, 32, 5, !bulkMode ? HEADER_BG : GREY);
  tft.fillRoundRect(165, 91, 145, 32, 5, bulkMode ? HEADER_BG : GREY);
  tft.drawRoundRect(10, 91, 145, 32, 5, ILI9341_WHITE);
  tft.drawRoundRect(165, 91, 145, 32, 5, ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(46, 100); tft.print("SINGLE");
  tft.setCursor(210, 100); tft.print("BULK");
}

void drawQuantityPanel() {
  tft.fillRoundRect(10, 131, 300, 38, 6, PANEL_BG);
  tft.drawRoundRect(10, 131, 300, 38, 6,
                    bulkMode ? ILI9341_CYAN : GREY);
  tft.setTextColor(ILI9341_WHITE);
  if (bulkMode) {
    tft.setTextSize(2);
    tft.setCursor(20, 142);
    tft.print("QTY: ");
    tft.print(bulkQuantity);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_CYAN);
    tft.setCursor(207, 146);
    tft.print("TAP TO EDIT");
  } else {
    tft.setTextSize(2);
    tft.setCursor(25, 141);
    tft.print("1 UNIT PER SCAN");
  }
}

void drawStatus() {
  tft.fillRoundRect(10, 176, 300, 61, 6, PANEL_BG);
  tft.drawRoundRect(10, 176, 300, 61, 6, ILI9341_DARKCYAN);
  tft.setTextSize(2);
  tft.setTextColor(statusColor);
  tft.setCursor(18, 182);
  tft.print(shorten(statusTitle, 23));
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(18, 206);
  tft.print(shorten(statusLine1, 47));
  tft.setCursor(18, 222);
  tft.print(shorten(statusLine2, 47));
}

void setStatus(String title, String line1, String line2, uint16_t color) {
  statusTitle = title;
  statusLine1 = line1;
  statusLine2 = line2;
  statusColor = color;
  if (currentPage == PAGE_HOME) drawStatus();
}

void drawScreen() {
  currentPage = PAGE_HOME;
  tft.fillScreen(SCREEN_BG);
  tft.fillRect(0, 0, 320, 32, HEADER_BG);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(11, 9);
  tft.print("INVENTORY");
  drawWifi();
  drawButtons();
  drawScanMode();
  drawQuantityPanel();
  drawStatus();
}

// ---------- BULK QUANTITY NUMERIC KEYPAD ----------
const int NUM_X[3] = {9, 86, 163};
const int NUM_Y[4] = {82, 119, 156, 193};
const char* KEY_LABELS[4][3] = {
  {"1", "2", "3"},
  {"4", "5", "6"},
  {"7", "8", "9"},
  {"CLR", "0", "DEL"}
};

void drawKeypadEntry() {
  tft.fillRect(9, 35, 302, 43, SCREEN_BG);
  tft.setTextSize(3);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(13, 38);
  tft.print("QTY: ");
  tft.print(qtyEntry.isEmpty() ? "_" : qtyEntry);
  tft.setTextSize(1);
  tft.setCursor(207, 66);
  tft.setTextColor(keypadError ? ILI9341_RED : ILI9341_CYAN);
  tft.print(keypadError ? "ENTER 1-100" : "MAX: 100");
}

void drawKeypad() {
  currentPage = PAGE_KEYPAD;
  tft.fillScreen(SCREEN_BG);
  tft.fillRect(0, 0, 320, 32, HEADER_BG);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 9);
  tft.print("BULK QUANTITY");
  drawKeypadEntry();

  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 3; ++col) {
      const int x = NUM_X[col], y = NUM_Y[row];
      tft.fillRoundRect(x, y, 70, 32, 4, PANEL_BG);
      tft.drawRoundRect(x, y, 70, 32, 4, ILI9341_LIGHTGREY);
      tft.setTextColor(ILI9341_WHITE);
      tft.setTextSize(2);
      const char* label = KEY_LABELS[row][col];
      int labelWidth = strlen(label) * 12; // 6 px * text size 2
      tft.setCursor(x + (70 - labelWidth) / 2, y + 8);
      tft.print(label);
    }
  }

  tft.fillRoundRect(242, 82, 69, 66, 5, GREEN);
  tft.fillRoundRect(242, 156, 69, 69, 5, RED);
  tft.drawRoundRect(242, 82, 69, 66, 5, ILI9341_WHITE);
  tft.drawRoundRect(242, 156, 69, 69, 5, ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(253, 108); tft.print("SAVE");
  tft.setCursor(248, 184); tft.print("BACK");
}

void openKeypad() {
  qtyEntry = ""; // overwrite the previous quantity when typing
  keypadError = false;
  drawKeypad();
}

void handleKeypadTouch(int x, int y) {
  if (x >= 242 && x <= 311) {
    if (y >= 82 && y <= 148) {
      int selected = qtyEntry.toInt();
      if (qtyEntry.isEmpty() || selected < 1 || selected > 100) {
        keypadError = true;
        drawKeypadEntry();
        return;
      }
      bulkQuantity = selected;
      drawScreen();
      setStatus("BULK quantity set", String("Qty: ") + bulkQuantity,
                "Enter item ID in Serial", ILI9341_CYAN);
      Serial.printf("Bulk quantity: %d\n", bulkQuantity);
    } else if (y >= 156 && y <= 225) {
      drawScreen(); // cancel without changing saved quantity
    }
    return;
  }

  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 3; ++col) {
      if (x < NUM_X[col] || x > NUM_X[col] + 70 ||
          y < NUM_Y[row] || y > NUM_Y[row] + 32) continue;
      const char* key = KEY_LABELS[row][col];
      if (strcmp(key, "CLR") == 0) qtyEntry = "";
      else if (strcmp(key, "DEL") == 0) {
        if (qtyEntry.length() > 0) qtyEntry.remove(qtyEntry.length() - 1);
      } else if (qtyEntry.length() < 3) {
        qtyEntry += key;
      }
      keypadError = false;
      drawKeypadEntry();
      return;
    }
  }
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

void updateInventory(const String& action, const String& itemID, int quantity) {
  if (quantity < 1 || quantity > 100) {
    setStatus("Invalid quantity", "Enter 1-100", "No update sent", ILI9341_RED);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: Wi-Fi is offline.");
    setStatus("Wi-Fi offline", itemID, "Reconnect before scanning", ILI9341_RED);
    WiFi.reconnect();
    return;
  }

  setStatus("Sending to Sheets", itemID,
            "Qty: " + String(quantity) + " | Please wait", ILI9341_YELLOW);
  JsonDocument request;
  request["key"] = API_KEY;
  request["id"] = itemID;
  request["action"] = action;
  request["qty"] = quantity;
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
    Serial.printf("SUCCESS: %s %s (%s), qty=%d, new stock=%d\n",
                  action.c_str(), itemID.c_str(), itemName.c_str(), quantity, stock);
    setStatus("UPDATED: " + action, itemID + " - " + itemName,
              "Qty: " + String(quantity) + "  |  Stock: " + String(stock),
              ILI9341_GREEN);
  } else {
    String error = result["error"].as<String>();
    Serial.println("API error: " + error);
    setStatus("Not updated", itemID, error, ILI9341_RED);
  }
}

void setMode(bool stockIn) {
  stockInMode = stockIn;
  if (currentPage == PAGE_HOME) drawButtons();
  else drawScreen();
  setStatus(stockIn ? "STOCK IN selected" : "STOCK OUT selected",
            "Mode: " + String(bulkMode ? "BULK" : "SINGLE"),
            "Enter item ID in Serial", ILI9341_CYAN);
  Serial.printf("Operation: %s\n", stockIn ? "IN" : "OUT");
}

void setScanMode(bool bulk) {
  bulkMode = bulk;
  if (currentPage == PAGE_HOME) {
    drawScanMode();
    drawQuantityPanel();
  } else drawScreen();
  setStatus(bulk ? "BULK mode" : "SINGLE mode",
            bulk ? "Tap QTY to edit, then scan" : "One unit per scan",
            "Enter item ID in Serial", ILI9341_CYAN);
  Serial.printf("Scan mode: %s; quantity per scan = %d\n",
                bulk ? "BULK" : "SINGLE", bulk ? bulkQuantity : 1);
}

void handleTouch() {
  bool touching = touch.touched();
  if (touching && !touchHeld) {
    TS_Point p = touch.getPoint();
    int x, y;
    if (touchToPixel(p, x, y)) {
      Serial.printf("Touch: x=%d y=%d\n", x, y);
      if (currentPage == PAGE_KEYPAD) {
        handleKeypadTouch(x, y);
      } else {
        if (y >= 39 && y <= 83) {
          if (x >= 10 && x <= 155) setMode(true);
          else if (x >= 165 && x <= 310) setMode(false);
        } else if (y >= 91 && y <= 123) {
          if (x >= 10 && x <= 155) setScanMode(false);
          else if (x >= 165 && x <= 310) setScanMode(true);
        } else if (bulkMode && y >= 131 && y <= 169 &&
                   x >= 10 && x <= 310) {
          openKeypad();
        }
      }
    }
    touchHeld = true;
  } else if (!touching) {
    touchHeld = false;
  }
}

bool parseQuantity(const String& input, int& result) {
  if (input.isEmpty() || input.length() > 3) return false;
  for (size_t i = 0; i < input.length(); ++i)
    if (input[i] < '0' || input[i] > '9') return false;
  result = input.toInt();
  return result >= 1 && result <= 100;
}

bool isValidItemID(const String& id) {
  if (id.isEmpty() || id.length() > 50) return false;
  for (size_t i = 0; i < id.length(); ++i) {
    char c = id[i];
    if (!((c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  ITEM001         : use touchscreen operation and qty");
  Serial.println("  IN ITEM001      : stock in using current qty");
  Serial.println("  OUT ITEM001     : stock out using current qty");
  Serial.println("  IN ITEM001 50   : override operation and qty");
  Serial.println("  OUT ITEM001 5   : override operation and qty");
  Serial.println("  QTY 50          : set quantity and BULK mode");
  Serial.println("  SINGLE / BULK   : choose scan mode");
  Serial.println("  IN / OUT        : choose operation");
  Serial.println("  CAL             : recalibrate touch");
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
  if (command.equalsIgnoreCase("HELP")) { printHelp(); return; }
  if (command.equalsIgnoreCase("SINGLE")) { setScanMode(false); return; }
  if (command.equalsIgnoreCase("BULK")) { setScanMode(true); return; }
  if (command.equalsIgnoreCase("IN")) { setMode(true); return; }
  if (command.equalsIgnoreCase("OUT")) { setMode(false); return; }

  // Split into up to 3 space-separated tokens; reject extra tokens.
  String parts[3];
  int n = 0;
  int pos = 0;
  while (pos < (int)command.length()) {
    while (pos < (int)command.length() && command[pos] == ' ') ++pos;
    if (pos >= (int)command.length()) break;
    if (n >= 3) {
      Serial.println("Too many words; type HELP for examples.");
      return;
    }
    int end = command.indexOf(' ', pos);
    if (end < 0) end = command.length();
    parts[n++] = command.substring(pos, end);
    pos = end;
  }
  if (n == 0) return;

  String first = parts[0];
  first.toUpperCase();
  if (first == "QTY") {
    int q;
    if (n != 2 || !parseQuantity(parts[1], q)) {
      Serial.println("Quantity must be between 1 and 100.");
      setStatus("Invalid quantity", "Try QTY 50", "No update sent", ILI9341_RED);
      return;
    }
    bulkQuantity = q;
    setScanMode(true);
    return;
  }

  String action = stockInMode ? "IN" : "OUT";
  int quantity = bulkMode ? bulkQuantity : 1;
  String itemID;
  if (first == "IN" || first == "OUT") {
    if (n < 2) { printHelp(); return; }
    action = first;
    itemID = parts[1];
    if (stockInMode != (action == "IN")) setMode(action == "IN");
    if (n == 3) {
      if (!parseQuantity(parts[2], quantity)) {
        Serial.println("Quantity must be 1 to 100.");
        setStatus("Invalid quantity", "No update sent", "Try IN ITEM001 50", ILI9341_RED);
        return;
      }
      bulkQuantity = quantity;
      setScanMode(true);
    }
  } else if (n == 1) {
    itemID = parts[0];
  } else {
    printHelp();
    return;
  }

  if (!isValidItemID(itemID)) {
    Serial.println("Invalid item ID; letters, numbers, _ and - only.");
    setStatus("Invalid item ID", "Try ITEM001", "No update sent", ILI9341_RED);
    return;
  }

  Serial.printf("Sending: %s %s x%d\n",
                action.c_str(), itemID.c_str(), quantity);
  updateInventory(action, itemID, quantity);
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
  setStatus("Ready", "Select mode and quantity", "Serial: ITEM001", ILI9341_CYAN);
  printHelp();
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
      if (currentPage == PAGE_HOME) drawWifi();
      Serial.println(online ? "Wi-Fi restored" : "Wi-Fi lost");
    }
    if (!online) WiFi.reconnect();
  }
  delay(20);
}
