/**
 * @file main.cpp
 * @brief IoT Button Press Logger with Web Interface
 * @details This system logs button presses with timestamps and provides a web interface
 * for monitoring and configuration.
 */

#include <WiFiManager.h>
#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <time.h>
#include "config.h"
#include <queue>

/** @name System Constants
 *  @{
 */
const unsigned long RESET_HOLD_TIME = 5000;
const char *NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 3600; // GMT+1
const int DAYLIGHT_OFFSET_SEC = 3600;
const int DEBOUNCE_DELAY = 250;
/** @} */

/** @name Global Variables
 *  @{
 */
std::queue<String> buttonLog;
bool isAccessPointMode = false;
/** @} */

/** @name Server Instances
 *  @{
 */
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
/** @} */

/** @brief Button structure for handling button states and counts */
struct Button
{
  const uint8_t PIN;
  ulong numberOfPresses;
  bool isPressed;
};

Button button1 = {4, 0, false};
volatile ulong currentInterruptTime = 0;
volatile ulong previousInterruptTime = 0;

// Function Declarations
/** @name File Operations
 *  @{
 */
/**
 * @brief Appends data to a file
 * @param filename The path to the file
 * @param data The data to append
 */
void appendToFile(const String &filename, const String &data);
String readFileContents(const String &filename);
void writeToFile(const String &filename, const String &data);
unsigned long loadButtonCountFromFile();
/** @} */

/** @name Server Handlers
 *  @{
 */
/**
 * @brief Handles the root web request
 * @param request The incoming web request
 */
void handleRootRequest(AsyncWebServerRequest *request);
void handleWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                          AwsEventType type, void *arg, uint8_t *data, size_t len);
void handleServiceModeRequest(AsyncWebServerRequest *request);
/** @} */

/** @name Setup Functions
 *  @{
 */
/**
 * @brief Initializes the web server
 */
void setupWebServer();
void setupNTP();
bool waitForNTPSync(int maxAttempts = 10);
/** @} */

/** @name Button Functions
 *  @{
 */
/**
 * @brief ISR for button press
 */
void IRAM_ATTR onButtonPress();
void handleOnButtonPress();
void processFifoBuffer();
/** @} */

/**
 * @brief Arduino setup function
 */
void setup()
{
  Serial.begin(115200);
  SPIFFS.begin(true);

  WiFiManager wm;
  // wm.resetSettings();

  bool res = wm.autoConnect();

  if (!res)
  {
    Serial.println("Failed to connect");
    // ESP.restart();
  }
  else
  {
    Serial.println("Connected to WiFi");
  }

  wm.stopConfigPortal();
  delay(1000);

  setupNTP();
  setupWebServer();

  pinMode(button1.PIN, INPUT_PULLUP);
  attachInterrupt(button1.PIN, onButtonPress, FALLING);

  button1.numberOfPresses = loadButtonCountFromFile();

  Serial.println("Setup complete");
}

/**
 * @brief Arduino main loop
 */
void loop()
{
  ws.cleanupClients();

  handleOnButtonPress();
  processFifoBuffer();
}

/**
 * @brief Handles button press interrupt
 */
void IRAM_ATTR onButtonPress()
{
  currentInterruptTime = millis();

  if (currentInterruptTime - previousInterruptTime > DEBOUNCE_DELAY)
  {
    button1.isPressed = true;
    previousInterruptTime = currentInterruptTime;
  }
}

void handleOnButtonPress()
{
  if (button1.isPressed)
  {
    // Get the current time
    struct tm timeinfo;
    getLocalTime(&timeinfo);

    // strftime formats the timestamp
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeinfo);

    // Add to fifo queue and +1 count
    buttonLog.push(String(timestamp));
    button1.numberOfPresses++;
    button1.isPressed = false;
    Serial.println("Button pressed");
  }
}

void processFifoBuffer()
{
  while (!buttonLog.empty())
  {
    String buttonPressTimestamp = buttonLog.front();
    buttonLog.pop();

    Serial.println("Pulse time - Fifo: " + buttonPressTimestamp);

    JsonDocument doc;
    doc["buttonPressTimestamp"] = buttonPressTimestamp;
    doc["buttonPressCount"] = button1.numberOfPresses;

    String jsonString;
    serializeJson(doc, jsonString);

    ws.textAll(jsonString);
    appendToFile(config::ButtonLogPath, jsonString);
  }
}

/**
 * @brief Initializes the web server
 */
void setupWebServer()
{
  server.on("/", HTTP_GET, handleRootRequest);
  server.on("/serviceMode", HTTP_POST, handleServiceModeRequest);

  ws.onEvent(handleWebSocketEvent);
  server.addHandler(&ws);
  server.begin();
  Serial.println("Web server started");
}

/**
 * @brief Handles the root web request
 * @param request The incoming web request
 */
void handleRootRequest(AsyncWebServerRequest *request)
{
  request->send(SPIFFS, "/index.html", "text/html");
}

void handleWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                          AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  if (type == WS_EVT_CONNECT)
  {
    File file = SPIFFS.open(config::ButtonLogPath, FILE_READ);
    if (!file)
    {
      Serial.println("Failed to open file for reading");
      return;
    }

    // Send each line as a separate WebSocket message
    while (file.available())
    {
      String jsonLine = file.readStringUntil('\n');
      if (!jsonLine.isEmpty())
      {
        client->text(jsonLine);
      }
    }

    file.close();
  }
}

void handleServiceModeRequest(AsyncWebServerRequest *request)
{
  if (!request->hasParam("action", true))
  {
    request->send(400, "text/plain", "Action parameter missing");
    return;
  }

  String action = request->getParam("action", true)->value();
  if (action == "reset")
  {
    // Clear the button log
    SPIFFS.remove(config::ButtonLogPath);
    button1.numberOfPresses = 0;
    request->send(200, "text/plain", "Data reset successfully");
    return;
  }
  else if (action == "resetWiFi")
  {
    // Reset WiFi settings
    request->send(200, "text/plain", "WiFi settings reset successfully. Rebooting...");
    WiFiManager wm;
    wm.resetSettings();
    delay(1000);
    ESP.restart();
    return;
  }

  request->send(400, "text/plain", "Invalid action");
}

/**
 * @brief Initializes NTP
 */
void setupNTP()
{
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  if (!waitForNTPSync())
  {
    Serial.println("NTP sync failed! Restarting...");
    ESP.restart();
  }
}

/**
 * @brief Waits for NTP synchronization
 * @param maxAttempts Maximum number of attempts
 * @return true if synchronized, false otherwise
 */
bool waitForNTPSync(int maxAttempts)
{
  Serial.println("Waiting for NTP sync...");
  struct tm timeinfo;
  for (int attempts = 0; attempts < maxAttempts; ++attempts)
  {
    if (getLocalTime(&timeinfo))
    {
      Serial.println("NTP synchronized successfully!");
      return true;
    }
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nFailed to sync with NTP server!");
  return false;
}

/**
 * @brief Appends data to a file
 * @param filename The path to the file
 * @param data The data to append
 */
void appendToFile(const String &filename, const String &data)
{
  File file = SPIFFS.open(filename, FILE_APPEND);
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    return;
  }
  file.println(data);
  file.close();
}

void writeToFile(const String &filename, const String &data)
{
  File file = SPIFFS.open(filename, FILE_WRITE);
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    return;
  }
  file.print(data);
  file.close();
}

String readFileContents(const String &filename)
{
  File file = SPIFFS.open(filename, FILE_READ);
  if (!file)
  {
    Serial.println("Failed to open file for reading");
    return "";
  }

  String content;
  while (file.available())
  {
    content = file.readStringUntil('\n');
  }
  file.close();
  return content;
}

unsigned long loadButtonCountFromFile()
{
  String content = readFileContents(config::ButtonLogPath);

  JsonDocument doc;

  deserializeJson(doc, content);

  return doc["buttonPressCount"].as<ulong>();
}