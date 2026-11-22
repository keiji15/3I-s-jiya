
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ========== YOUR SETTINGS ==========
const char* WIFI_SSID = "ARCEGA WIFI";
const char* WIFI_PASS = "@Arcega2023";

const char* WEB_APP_URL = "https://script.google.com/macros/s/AKfycbz6LokGVBPKzSOLGwnbmDTsDAa5LUR7PQTsbzj5jQyBZGTRuPPOx7IIGILHcp2ycEfS/exec";

const char* API_KEY = "This_is_a_secret_key";
const char* DEVICE_ID = "SCANNER01";

// ========== SERIAL INPUT ==========
String input = "";

// Retrieve Google's redirected JSON response
String getResponse(String url) {
  for (int attempt = 0; attempt < 3; attempt++) {
    WiFiClientSecure client;

    // Prototype only: skips TLS certificate checking
    client.setInsecure();

    HTTPClient http;

    if (!http.begin(client, url)) {
      Serial.println("Response connection failed.");
      return "";
    }

    const char* headers[] = {"Location"};
    http.collectHeaders(headers, 1);
    http.setTimeout(20000);

    int status = http.GET();

    if (status == 200) {
      String result = http.getString();
      http.end();
      return result;
    }

    if (status == 301 || status == 302 ||
        status == 303 || status == 307) {
      String nextURL = http.header("Location");
      http.end();

      if (nextURL.isEmpty()) return "";

      url = nextURL;
      continue;
    }

    Serial.printf("Response HTTP error: %d\n", status);
    http.end();
    return "";
  }

  return "";
}

// ========== GOOGLE SHEETS REQUEST ==========
void updateInventory(String action, String itemID) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: Wi-Fi disconnected.");
    WiFi.reconnect();
    return;
  }

  JsonDocument request;

  request["key"] = API_KEY;
  request["id"] = itemID;
  request["action"] = action;
  request["qty"] = 1;
  request["device"] = DEVICE_ID;

  String payload;
  serializeJson(request, payload);

  WiFiClientSecure client;
  client.setInsecure(); // Prototype testing only

  HTTPClient http;

  if (!http.begin(client, WEB_APP_URL)) {
    Serial.println("Cannot connect to web app.");
    return;
  }

  const char* headers[] = {"Location"};
  http.collectHeaders(headers, 1);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(20000);

  Serial.println("Sending inventory request...");

  int status = http.POST(payload);

  String response = "";
  String redirectURL = "";

  if (status == 200) {
    response = http.getString();
  } else if (status == 302 || status == 303 ||
             status == 301) {
    redirectURL = http.header("Location");
  } else {
    Serial.printf("HTTP status: %d\n", status);
  }

  http.end();

  if (redirectURL.length() > 0) {
    response = getResponse(redirectURL);
  }

  if (response.isEmpty()) {
    Serial.println("Result unknown.");
    Serial.println("Check Google Sheets before retrying.");
    return;
  }

  JsonDocument result;

  if (deserializeJson(result, response)) {
    Serial.println("Invalid server response:");
    Serial.println(response);
    return;
  }

  if (result["ok"].as<bool>()) {
    Serial.println("------------------------");
    Serial.println("INVENTORY UPDATED!");
    Serial.print("Item: ");
    Serial.println(result["item_name"].as<String>());
    Serial.print("Action: ");
    Serial.println(action);
    Serial.print("Remaining stock: ");
    Serial.println(result["stock"].as<int>());
    Serial.println("------------------------");
  } else {
    Serial.print("ERROR: ");
    Serial.println(result["error"].as<String>());
  }
}

// ========== COMMAND PROCESSING ==========
void processCommand(String command) {
  command.trim();

  int separator = command.indexOf(' ');

  if (separator < 0) {
    Serial.println("Use: IN ITEM001 or OUT ITEM001");
    return;
  }

  String action = command.substring(0, separator);
  String itemID = command.substring(separator + 1);

  action.toUpperCase();
  itemID.trim();

  if ((action != "IN" && action != "OUT") ||
      itemID.isEmpty()) {
    Serial.println("Invalid command.");
    return;
  }

  updateInventory(action, itemID);
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("ESP32 Inventory System");
  Serial.println("Connecting to Wi-Fi...");

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWi-Fi connection failed.");
  }

  Serial.println("\nInventory System Ready");
  Serial.println("Commands:");
  Serial.println("IN ITEM001");
  Serial.println("OUT ITEM001");
}

// ========== MAIN LOOP ==========
void loop() {
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      if (!input.isEmpty()) {
        processCommand(input);
        input = "";
      }
    } else if (input.length() < 100) {
      input += c;
    }
  }
}
