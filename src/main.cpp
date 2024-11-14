#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <time.h>
#include "config.h"
#include <queue>

// Constants
const unsigned long RESET_HOLD_TIME = 5000;
const char *NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 3600; // GMT+1
const int DAYLIGHT_OFFSET_SEC = 3600;

// Globals
std::queue<String> buttonLog;
bool isAccessPointMode = false;

// Web Server
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Utility Functions
void appendToFile(const String &filename, const String &data);
String readFileContents(const String &filename);

// Initialization Functions
void setupWebServer();
void setupWiFi();
void setupNTP();
bool waitForNTPSync(int maxAttempts = 10);

void handleRootRequest(AsyncWebServerRequest *request);
void handleWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void handleServiceModeRequest(AsyncWebServerRequest *request);

// Structs
struct Button
{
  const uint8_t PIN;
  ulong numberOfPresses;
  bool isPressed;
};

// Button and Interrupt Functions
Button button1 = {4, 0, false};
volatile ulong currentInterruptTime = 0;
volatile ulong previousInterruptTime = 0;
const int DEBOUNCE_DELAY = 250;

void IRAM_ATTR onButtonPress();

void handleOnButtonPress();

// Core Functionality
void processFifoBuffer();

unsigned long loadButtonCountFromFile();

void setupWiFi();
void startAccessPoint();
void saveWiFiConfig(const String &ssid, const String &password);

// Setup Function
void setup()
{
  Serial.begin(115200);
  SPIFFS.begin(true);

  // Check if WiFi credentials are available
  String wifiConfig = readFileContents(config::wifiConfigFile);
  if (wifiConfig.isEmpty())
  {
    // If no WiFi config, start AP mode
    isAccessPointMode = true;
    startAccessPoint();
  }
  else
  {
    setupWiFi();
    setupNTP();
  }

  setupWebServer();

  pinMode(button1.PIN, INPUT_PULLUP);
  attachInterrupt(button1.PIN, onButtonPress, FALLING);

  button1.numberOfPresses = loadButtonCountFromFile();

  Serial.println("Setup complete");
}

// Main Loop
void loop()
{
  ws.cleanupClients();

  handleOnButtonPress();
  processFifoBuffer();

  // String content = readFileContents(config::ButtonLogPath);
  // if (!content.isEmpty())
  // {
  //   Serial.println(content);
  //   // ws.textAll(content);
  //   delay(2000);
  // }
}

// Function Implementations

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

// WiFi setup

void setupWebServer()
{
  server.on("/", HTTP_GET, handleRootRequest);
  server.on("/serviceMode", HTTP_POST, handleServiceModeRequest);

  server.on("/wifiConfig", HTTP_POST, [](AsyncWebServerRequest *request)
            {
        if (!request->hasParam("ssid", true) || !request->hasParam("password", true))
        {
            request->send(400, "text/plain", "Missing SSID or password");
            return;
        }

        String ssid = request->getParam("ssid", true)->value();
        String password = request->getParam("password", true)->value();
        
        // Save the credentials to SPIFFS
        saveWiFiConfig(ssid, password);

        // Connect to the new WiFi
        WiFi.begin(ssid.c_str(), password.c_str());
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000)
        {
            delay(100);
        }

        if (WiFi.status() == WL_CONNECTED)
        {
            request->send(200, "text/plain", "WiFi Configured! Rebooting...");
            ESP.restart(); // Reboot to apply the new WiFi settings
        }
        else
        {
            request->send(500, "text/plain", "Failed to connect to WiFi");
        } });

  ws.onEvent(handleWebSocketEvent);
  server.addHandler(&ws);
  server.begin();
  Serial.println("Web server started");
}

void startAccessPoint()
{
  WiFi.softAP("ESP32-Setup", "123456789");
  Serial.println("Access Point Started. Connect to the AP with SSID: ESP32-Setup and password: 123456789");

  setupWebServer(); // Start the web server to handle SSID/password input page
}

/// NTP setup

void setupNTP()
{
  if (isAccessPointMode) return; // Skip NTP setup in AP mode

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  if (!waitForNTPSync())
  {
    Serial.println("NTP sync failed! Restarting...");
    ESP.restart();
  }
}

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

void setupWiFi()
{
  // Check if WiFi credentials are available
  String wifiConfig = readFileContents(config::wifiConfigFile);
  if (wifiConfig.isEmpty())
  {
    // If no WiFi config, start AP mode
    startAccessPoint();
  }
  else
  {
    // Parse the saved SSID and password
    JsonDocument doc;
    deserializeJson(doc, wifiConfig);
    const char *ssid = doc["ssid"];
    const char *password = doc["password"];

    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi...");
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000)
    {
      Serial.print(".");
      delay(100);
    }

    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("\nFailed to connect to WiFi");
    }
    else
    {
      Serial.println("\nConnected to WiFi");
      Serial.println("IP Address: " + WiFi.localIP().toString());
    }
  }
}

void saveWiFiConfig(const String &ssid, const String &password)
{
  JsonDocument doc;
  doc["ssid"] = ssid;
  doc["password"] = password;

  String jsonString;
  serializeJson(doc, jsonString);

  appendToFile(config::wifiConfigFile, jsonString); // Save config to file
}

void handleRootRequest(AsyncWebServerRequest *request)
{
  request->send(SPIFFS, "/index.html", "text/html");
}

void handleWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
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

  request->send(400, "text/plain", "Invalid action");
}

// File handling functions

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

ulong loadButtonCountFromFile()
{
  String content = readFileContents(config::ButtonLogPath);

  JsonDocument doc;

  deserializeJson(doc, content);

  return doc["buttonPressCount"].as<ulong>();
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