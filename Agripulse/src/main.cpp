#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <ModbusMaster.h>
#include <SPIFFS.h>

// Agripulse ESP32 firmware
// - Reads a 7-in-1 RS485 (Modbus) soil sensor
// - Shows values on a 1.77" SPI TFT (ST77xx)
// - Provides a phone-accessible web UI via ESP32 Wi-Fi AP
// - Saves CSV history to SPIFFS
// This file is intentionally kept simple and well-commented
// so it's easier to understand and extend.

// ------------------------------------------------------------
// Agriculture sensor prototype for ESP32 + RS485 + TFT + phone
// ------------------------------------------------------------
// Hardware pins (change if your wiring differs)
// TFT (SPI)
#define TFT_SCLK 18
#define TFT_MOSI 23
#define TFT_CS    5
#define TFT_DC    26
#define TFT_RST   27
#define TFT_BL    25 // backlight

// User buttons (3 tact switches wired to GPIO and GND)
#define BTN_UP    35
#define BTN_DOWN  32
#define BTN_OK    33

// RS485 (Modbus) interface
#define RS485_DE  4   // driver enable pin (HIGH to transmit)
#define RS485_RX  16  // Serial2 RX
#define RS485_TX  17  // Serial2 TX

// Generic 7-in-1 soil sensor register map.
// IMPORTANT: Confirm Modbus register addresses for your sensor model.
// This example assumes the sensor exposes 7 input registers in this order:
// 0: N (x10), 1: P (x10), 2: K (x10), 3: pH (x10), 4: EC (x10), 5: Moisture (x10), 6: Temperature (x10)
#define SENSOR_SLAVE_ID       1
#define REGISTER_COUNT        7
#define SENSOR_START_REGISTER 0x0000

// Misc constants
#define WIFI_AP_SSID    "Agripulse-Setup"
#define WIFI_AP_PASSWORD "agri1234"
#define SERVER_PORT      80
#define LOG_PATH         "/log.csv"
#define CSV_HEADER       "timestamp,N,P,K,pH,EC,Moisture,Temp"
#define BUTTON_DEBOUNCE_MS 220

const char *AP_SSID = WIFI_AP_SSID;
const char *AP_PASSWORD = WIFI_AP_PASSWORD;

struct SoilReading {
  float nitrogen = 0.0f;
  float phosphorus = 0.0f;
  float potassium = 0.0f;
  float ph = 0.0f;
  float ec = 0.0f;
  float moisture = 0.0f;
  float temperature = 0.0f;
};

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
WebServer server(SERVER_PORT);
ModbusMaster modbus;
Preferences prefs;

SoilReading lastReading;
String lastStatus = "Ready";

int menuIndex = 0;
const char *menuItems[] = {"Scan sensor", "History"};
const int menuCount = 2;
bool historyView = false;
int historyOffset = 0;

void setBacklight(bool enabled) {
  // Turn display backlight on/off
  digitalWrite(TFT_BL, enabled ? HIGH : LOW);
}

// Show a short status message on the bottom of the TFT.
void showMessage(const String &message) {
  lastStatus = message;
  tft.fillRect(0, 120, 128, 40, ST7735_BLACK);
  tft.setCursor(4, 122);
  tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  tft.setTextSize(1);
  tft.print(message.substring(0, 18));
}

// Helper to format float values consistently with one decimal place.
String fmt(float v) {
  return String(v, 1);
}

void renderMenu() {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_GREEN, ST7735_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 4);
  tft.println("AgriPulse");

  for (int i = 0; i < menuCount; ++i) {
    int y = 35 + i * 26;
    uint16_t background = i == menuIndex ? ST7735_BLUE : ST7735_BLACK;
    uint16_t text = i == menuIndex ? ST7735_WHITE : ST7735_GREEN;

    tft.fillRoundRect(6, y - 2, 116, 22, 4, background);
    tft.setTextColor(text, background);
    tft.setTextSize(1);
    tft.setCursor(14, y);
    tft.print(menuItems[i]);
  }

  tft.setTextColor(ST7735_YELLOW, ST7735_BLACK);
  tft.setCursor(4, 110);
  tft.setTextSize(1);
  tft.println("Up/Down/OK");

  showMessage(lastStatus);
}

void renderReading(const SoilReading &reading) {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_GREEN, ST7735_BLACK);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.println("Soil scan");

  tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  tft.setCursor(4, 22);
  tft.print("N:");
  tft.print(fmt(reading.nitrogen));
  tft.print(" mg/kg");

  tft.setCursor(4, 34);
  tft.print("P:");
  tft.print(fmt(reading.phosphorus));
  tft.print(" mg/kg");

  tft.setCursor(4, 46);
  tft.print("K:");
  tft.print(fmt(reading.potassium));
  tft.print(" mg/kg");

  tft.setCursor(4, 58);
  tft.print("pH:");
  tft.print(fmt(reading.ph));

  tft.setCursor(4, 70);
  tft.print("EC:");
  tft.print(fmt(reading.ec));
  tft.print(" uS/cm");

  tft.setCursor(4, 82);
  tft.print("Moist:");
  tft.print(fmt(reading.moisture));
  tft.print("%");

  tft.setCursor(4, 94);
  tft.print("Temp:");
  tft.print(fmt(reading.temperature));
  tft.print("C");

  tft.setTextColor(ST7735_YELLOW, ST7735_BLACK);
  tft.setCursor(4, 112);
  tft.print("OK = back");

  showMessage(lastStatus);
}

void saveReadingToPreferences(const SoilReading &reading) {
  prefs.begin("agri", false);
  prefs.putFloat("N", reading.nitrogen);
  prefs.putFloat("P", reading.phosphorus);
  prefs.putFloat("K", reading.potassium);
  prefs.putFloat("pH", reading.ph);
  prefs.putFloat("EC", reading.ec);
  prefs.putFloat("MOIST", reading.moisture);
  prefs.putFloat("TEMP", reading.temperature);
  prefs.end();
}

void loadReadingFromPreferences() {
  prefs.begin("agri", true);
  lastReading.nitrogen = prefs.getFloat("N", 0.0f);
  lastReading.phosphorus = prefs.getFloat("P", 0.0f);
  lastReading.potassium = prefs.getFloat("K", 0.0f);
  lastReading.ph = prefs.getFloat("pH", 0.0f);
  lastReading.ec = prefs.getFloat("EC", 0.0f);
  lastReading.moisture = prefs.getFloat("MOIST", 0.0f);
  lastReading.temperature = prefs.getFloat("TEMP", 0.0f);
  prefs.end();
}

String buildCsv() {
  String csv = "timestamp,N,P,K,pH,EC,Moisture,Temp\n";
  csv += String(millis());
  csv += ",";
  csv += String(lastReading.nitrogen, 1);
  csv += ",";
  csv += String(lastReading.phosphorus, 1);
  csv += ",";
  csv += String(lastReading.potassium, 1);
  csv += ",";
  csv += String(lastReading.ph, 1);
  csv += ",";
  csv += String(lastReading.ec, 1);
  csv += ",";
  csv += String(lastReading.moisture, 1);
  csv += ",";
  csv += String(lastReading.temperature, 1);
  csv += "\n";
  return csv;
}

bool appendReadingToLog(const SoilReading &reading) {
  if (!SPIFFS.begin(true)) {
    Serial.println("Failed to mount SPIFFS");
    return false;
  }

  bool writeHeader = !SPIFFS.exists(LOG_PATH);
  File f = SPIFFS.open(LOG_PATH, FILE_APPEND);
  if (!f) {
    Serial.println("Failed to open log file");
    return false;
  }

  if (writeHeader) {
    f.println(CSV_HEADER);
  }

  String line = "";
  line += String(millis());
  line += "," + String(reading.nitrogen, 1);
  line += "," + String(reading.phosphorus, 1);
  line += "," + String(reading.potassium, 1);
  line += "," + String(reading.ph, 1);
  line += "," + String(reading.ec, 1);
  line += "," + String(reading.moisture, 1);
  line += "," + String(reading.temperature, 1);

  f.println(line);
  f.close();
  return true;
}

String buildJson() {
  String json = "{\n";
  json += "  \"nitrogen\": " + String(lastReading.nitrogen, 1) + ",\n";
  json += "  \"phosphorus\": " + String(lastReading.phosphorus, 1) + ",\n";
  json += "  \"potassium\": " + String(lastReading.potassium, 1) + ",\n";
  json += "  \"ph\": " + String(lastReading.ph, 1) + ",\n";
  json += "  \"ec\": " + String(lastReading.ec, 1) + ",\n";
  json += "  \"moisture\": " + String(lastReading.moisture, 1) + ",\n";
  json += "  \"temperature\": " + String(lastReading.temperature, 1) + "\n";
  json += "}\n";
  return json;
}

bool parseHistoryLine(const String &line, SoilReading &reading) {
  if (line.length() == 0 || line.startsWith("timestamp")) {
    return false;
  }

  float values[REGISTER_COUNT];
  int fieldStart = 0;
  for (int field = 0; field < REGISTER_COUNT + 1; ++field) {
    int fieldEnd = line.indexOf(',', fieldStart);
    if (fieldEnd < 0) {
      fieldEnd = line.length();
    }

    if (field > 0) {
      values[field - 1] = line.substring(fieldStart, fieldEnd).toFloat();
    }

    fieldStart = fieldEnd + 1;
    if (fieldEnd == line.length()) {
      break;
    }
  }

  reading.nitrogen = values[0];
  reading.phosphorus = values[1];
  reading.potassium = values[2];
  reading.ph = values[3];
  reading.ec = values[4];
  reading.moisture = values[5];
  reading.temperature = values[6];
  return true;
}

bool loadHistoryRecord(int offset, SoilReading &reading) {
  if (!SPIFFS.begin(true) || !SPIFFS.exists(LOG_PATH)) {
    return false;
  }

  File file = SPIFFS.open(LOG_PATH, "r");
  if (!file) {
    return false;
  }

  int recordCount = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    SoilReading ignored;
    if (parseHistoryLine(line, ignored)) {
      ++recordCount;
    }
  }
  file.close();

  int target = recordCount - 1 - offset;
  if (target < 0) {
    return false;
  }

  file = SPIFFS.open(LOG_PATH, "r");
  if (!file) {
    return false;
  }

  int recordIndex = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    SoilReading current;
    if (parseHistoryLine(line, current)) {
      if (recordIndex == target) {
        reading = current;
        file.close();
        return true;
      }
      ++recordIndex;
    }
  }

  file.close();
  return false;
}

void renderHistory() {
  SoilReading reading;
  if (loadHistoryRecord(historyOffset, reading)) {
    lastStatus = "History scan";
    renderReading(reading);
    return;
  }

  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_GREEN, ST7735_BLACK);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.println("History");
  tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
  tft.setCursor(4, 30);
  tft.println("No saved scans");
  tft.setTextColor(ST7735_YELLOW, ST7735_BLACK);
  tft.setCursor(4, 112);
  tft.println("OK = back");
  showMessage("No history");
}

void sendHistory(bool download) {
  if (!SPIFFS.begin(true)) {
    server.send(500, "text/plain", "SPIFFS error");
    return;
  }

  if (!SPIFFS.exists(LOG_PATH)) {
    server.send(200, "text/plain", "No history");
    return;
  }

  File f = SPIFFS.open(LOG_PATH, "r");
  if (!f) {
    server.send(500, "text/plain", "Failed to open log");
    return;
  }

  String csv = "";
  while (f.available()) {
    csv += (char)f.read();
  }
  f.close();
  if (download) {
    server.sendHeader("Content-Disposition", "attachment; filename=agri_history.csv");
  }
  server.send(200, "text/csv", csv);
}

void handleHistory() {
  sendHistory(false);
}

void handleHistoryDownload() {
  sendHistory(true);
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "</head><body><ol>"
                "<li>Nitrogen: " + String(lastReading.nitrogen, 1) + "</li>"
                "<li>Phosphorus: " + String(lastReading.phosphorus, 1) + "</li>"
                "<li>Potassium: " + String(lastReading.potassium, 1) + "</li>"
                "<li>pH: " + String(lastReading.ph, 1) + "</li>"
                "<li>EC: " + String(lastReading.ec, 1) + "</li>"
                "<li>Moisture: " + String(lastReading.moisture, 1) + "%</li>"
                "<li>Temperature: " + String(lastReading.temperature, 1) + " C</li>"
                "</ol></body></html>";
  server.send(200, "text/html", html);
}

void handleDownload() {
  String csv = buildCsv();
  server.sendHeader("Content-Disposition", "attachment; filename=agri_scan.csv");
  server.send(200, "text/csv", csv);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

bool readSensorFromModbus() {
  modbus.clearResponseBuffer();
  modbus.clearTransmitBuffer();

  uint8_t result = modbus.readInputRegisters(SENSOR_START_REGISTER, REGISTER_COUNT);
  if (result == modbus.ku8MBSuccess) {
    SoilReading reading;
    reading.nitrogen = static_cast<float>(modbus.getResponseBuffer(0)) / 10.0f;
    reading.phosphorus = static_cast<float>(modbus.getResponseBuffer(1)) / 10.0f;
    reading.potassium = static_cast<float>(modbus.getResponseBuffer(2)) / 10.0f;
    reading.ph = static_cast<float>(modbus.getResponseBuffer(3)) / 10.0f;
    reading.ec = static_cast<float>(modbus.getResponseBuffer(4)) / 10.0f;
    reading.moisture = static_cast<float>(modbus.getResponseBuffer(5)) / 10.0f;
    reading.temperature = static_cast<float>(modbus.getResponseBuffer(6)) / 10.0f;

    lastReading = reading;
    saveReadingToPreferences(lastReading);
    lastStatus = "Sensor ready";
    return true;
  }

  lastStatus = "Sensor read failed";
  return false;
}

void initTft() {
  pinMode(TFT_BL, OUTPUT);
  setBacklight(true);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST7735_BLACK);
  tft.setTextWrap(false);
}

void initButtons() {
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);
}

void initRs485() {
  pinMode(RS485_DE, OUTPUT);
  digitalWrite(RS485_DE, LOW);

  Serial2.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  modbus.begin(SENSOR_SLAVE_ID, Serial2);
  modbus.preTransmission([]() {
    digitalWrite(RS485_DE, HIGH);
  });
  modbus.postTransmission([]() {
    digitalWrite(RS485_DE, LOW);
  });
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  delay(1000);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  lastStatus = "WiFi up";
}

void handleOnBoardButtons() {
  static unsigned long lastPress = 0;
  if (millis() - lastPress < BUTTON_DEBOUNCE_MS) {
    return;
  }

  if (historyView) {
    if (digitalRead(BTN_UP) == LOW) {
      if (historyOffset > 0) {
        --historyOffset;
        renderHistory();
      } else {
        showMessage("Newest scan");
      }
      lastPress = millis();
      return;
    }

    if (digitalRead(BTN_DOWN) == LOW) {
      ++historyOffset;
      if (!loadHistoryRecord(historyOffset, lastReading)) {
        --historyOffset;
        showMessage("Oldest scan");
      } else {
        renderHistory();
      }
      lastPress = millis();
      return;
    }

    if (digitalRead(BTN_OK) == LOW) {
      historyView = false;
      historyOffset = 0;
      renderMenu();
      lastPress = millis();
      return;
    }
  }

  if (digitalRead(BTN_UP) == LOW) {
    menuIndex = (menuIndex + menuCount - 1) % menuCount;
    renderMenu();
    lastPress = millis();
    return;
  }

  if (digitalRead(BTN_DOWN) == LOW) {
    menuIndex = (menuIndex + 1) % menuCount;
    renderMenu();
    lastPress = millis();
    return;
  }

  if (digitalRead(BTN_OK) == LOW) {
    if (menuIndex == 0) {
      lastStatus = "Scanning...";
      renderMenu();
      if (readSensorFromModbus()) {
        renderReading(lastReading);
      } else {
        renderMenu();
      }
    } else if (menuIndex == 1) {
      historyView = true;
      historyOffset = 0;
      renderHistory();
    }
    lastPress = millis();
  }
}

void setup() {
  Serial.begin(115200);
  initTft();
  initButtons();
  initRs485();
  loadReadingFromPreferences();

  startAccessPoint();
  // prepare filesystem for history logging
  if (!SPIFFS.begin(true)) {
    Serial.println("Warning: SPIFFS mount failed");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_GET, []() {
    if (readSensorFromModbus()) {
      appendReadingToLog(lastReading);
      server.send(200, "application/json", buildJson());
    } else {
      server.send(500, "text/plain", "Sensor read failed");
    }
  });
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/history", HTTP_GET, handleHistory);
  server.on("/history/download", HTTP_GET, handleHistoryDownload);
  server.onNotFound(handleNotFound);
  server.begin();

  renderMenu();
}

void loop() {
  server.handleClient();
  handleOnBoardButtons();
}