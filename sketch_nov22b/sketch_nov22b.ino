
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>

// TFT display pins
#define TFT_CS   5
#define TFT_DC   27
#define TFT_RST  26

// Touchscreen pins
#define TOUCH_CS  33
#define TOUCH_IRQ 25

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

void setup() {
  Serial.begin(115200);

  pinMode(TFT_CS, OUTPUT);
  pinMode(TOUCH_CS, OUTPUT);

  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TOUCH_CS, HIGH);

  SPI.begin(18, 19, 23);

  // Initialize display
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLACK);

  // Initialize touchscreen
  touch.begin();
  touch.setRotation(1);

  // Draw test interface
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(40, 20);
  tft.println("INVENTORY SYSTEM");

  tft.drawRoundRect(
    20, 70, 280, 110, 10,
    ILI9341_WHITE
  );

  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(55, 100);
  tft.println("Touch the screen");

  Serial.println("TFT Touch Test Ready!");
}

void loop() {
  if (touch.touched()) {
    TS_Point p = touch.getPoint();

    Serial.print("Touch X: ");
    Serial.print(p.x);
    Serial.print(" Y: ");
    Serial.println(p.y);

    tft.fillRect(
      25, 90, 270, 70,
      ILI9341_BLACK
    );

    tft.setTextColor(ILI9341_GREEN);
    tft.setTextSize(2);

    tft.setCursor(40, 100);
    tft.print("X: ");
    tft.println(p.x);

    tft.setCursor(40, 130);
    tft.print("Y: ");
    tft.println(p.y);

    delay(200);
  }
}
