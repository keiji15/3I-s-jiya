ESP32 TFT Bulk Inventory - Quick Start

1. Extract the ZIP, open ESP32_TFT_Bulk_Inventory.ino in Arduino IDE.
2. Set your Wi-Fi name/password, Apps Script deployment URL, and API key at the top.
3. Use your previously working pins and install libraries: Adafruit GFX, Adafruit ILI9341, XPT2046_Touchscreen, ArduinoJson v7.
4. Upload to ESP32 Dev Module. Calibration is reused from the prior sketch.
5. Tap STOCK IN or STOCK OUT, then SINGLE or BULK.
6. In BULK, tap QTY, enter 1-100 using touchscreen and SAVE.
7. In Arduino Serial Monitor (115200, Newline), enter ITEM001 once to process the selected quantity.
8. In SINGLE, ITEM001 processes one unit.
Alternative Serial commands: QTY 50, IN ITEM001 50, OUT ITEM001 5, SINGLE, BULK, CAL, HELP.

Existing Apps Script supports 1-100 quantity per request; no script changes are needed.
Keep the Google Apps Script API key private. HTTPS certificate verification is disabled in this prototype sketch; enable verified TLS before real deployment.
If the result is unknown, check the sheet before repeating the transaction.
