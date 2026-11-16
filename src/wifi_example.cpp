///////////////////////////////////////////////// SETUP /////////////////////////////////////////////////
// Libraries
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <time.h>

// NOTE: Computer and micro-controller need to be connected to the same WiFi!!
// Classroom WiFi
constexpr char wifiSsid[] = "NSA Security Van HQ";
constexpr char wifiPassword[] = "windowstothehallway";

// Apartment WiFi
//constexpr char wifiSsid[] = "SpectrumSetup-F0";
//constexpr char wifiPassword[] = "smartroad648";

constexpr char locationName[] = "SG-32, Science Building, MSOE, Milwaukee, WI";
constexpr char latitude[] = "43.04302803179632";
constexpr char longitude[] = "-87.90678040825065";

// U.S. Central time: daylight saving starts in March and ends in November.
constexpr char centralTimeZone[] = "CST6CDT,M3.2.0/2,M11.1.0/2";

constexpr uint8_t pixelLevel = 64;  // brightness of each color channel (0-255)

// HTML code
constexpr char page[] PROGMEM = R"html(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Weather, Time and NeoPixel</title>
</head>

<body>
  <h1>ESP32 Weather, Time and NeoPixel</h1>

  <p>Weather location: <span id="location"></span></p>
  <p id="weather">Press the button to get the temperature.</p>
  <button id="weatherButton" onclick="update('/weather', 'weather', this)">Update Weather</button>

  <p id="clock">Press the button to get Central time.</p>
  <button id="clockButton" onclick="update('/clock', 'clock', this)">Update Central Time</button>

  <p id="red">Red is off.</p>
  <button onclick="update('/red', 'red', this)">Toggle Red</button>

  <p id="green">Green is off.</p>
  <button onclick="update('/green', 'green', this)">Toggle Green</button>

  <p id="blue">Blue is off.</p>
  <button onclick="update('/blue', 'blue', this)">Toggle Blue</button>

  <script>
    async function update(url, id, button) {
      button.disabled = true;
      document.getElementById(id).textContent = 'Updating...';
      try {
        const response = await fetch(url, {cache: 'no-store'});
        const text = await response.text();
        document.getElementById(id).textContent = response.ok ? text : 'Error: ' + text;
      } catch (error) {
        document.getElementById(id).textContent = 'Could not reach the ESP32: ' + error;
      } finally {
        button.disabled = false;
      }
    }

    fetch('/location').then(response => response.text())
      .then(text => document.getElementById('location').textContent = text)
      .catch(() => document.getElementById('location').textContent = 'Unavailable');

    // Only read-only requests on page load (the color routes toggle the LED).
    update('/weather', 'weather', document.getElementById('weatherButton'));
    update('/clock', 'clock', document.getElementById('clockButton'));
  </script>
</body>
</html>
)html";


// Misc variables
WebServer server(80);
bool wasConnected = false;
bool serverStarted = false;
uint32_t lastReconnectMs = 0;


// Json
bool getJson(const String& url, JsonDocument& document, String& error) {
    if (WiFi.status() != WL_CONNECTED) {
        error = "Wi-Fi is disconnected.";
        return false;
    }
    WiFiClient client;
    client.setTimeout(10);
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(10000);
    if (!http.begin(client, url)) {
        error = "Could not initialize HTTP request.";
        return false;
    }

    const int status = http.GET();
    if (status != HTTP_CODE_OK) {
        error = "HTTP request failed: ";
        error += status > 0 ? String(status) : HTTPClient::errorToString(status);
        http.end();
        return false;
    }

    const DeserializationError result = deserializeJson(document, http.getString());
    http.end();
    if (result) {
        error = String("Invalid JSON response: ") + result.c_str();
        return false;
    }
    return true;
}


// Pulls weather data from a website
bool getWeather(String& result) {
    const String url = String("http://api.open-meteo.com/v1/forecast?latitude=") +
        latitude + "&longitude=" + longitude +
        "&current=temperature_2m&temperature_unit=fahrenheit&timezone=America%2FChicago";
    JsonDocument document;
    if (!getJson(url, document, result)) {
        return false;
    }
    if (!document["current"]["temperature_2m"].is<float>() ||
        !document["current"]["time"].is<const char*>()) {
        result = "Weather response is missing the temperature or observation time.";
        return false;
    }

    result = String(locationName) + ": " +
        String(document["current"]["temperature_2m"].as<float>(), 1) +
        " F (observation time: " + document["current"]["time"].as<String>() + " Central)";
    return true;
}


// Pulls time data
bool getCentralTime(String& result) {
    const time_t now = time(nullptr);
    if (now < 1700000000) {
        result = "Waiting for NTP time synchronization; try again shortly.";
        return false;
    }
    struct tm localTime;
    char formattedTime[40];
    if (localtime_r(&now, &localTime) == nullptr ||
        strftime(formattedTime, sizeof(formattedTime), "%Y-%m-%d %H:%M:%S %Z", &localTime) == 0) {
        result = "Could not format the Central time.";
        return false;
    }
    result = formattedTime;
    return true;
}


// NeoPixel color state: each channel is toggled independently
bool redOn = false;
bool greenOn = false;
bool blueOn = false;

void applyPixel() {
    neopixelWrite(PIN_NEOPIXEL,
                  redOn ? pixelLevel : 0,
                  greenOn ? pixelLevel : 0,
                  blueOn ? pixelLevel : 0);
}

bool toggleRed(String& result) {
    redOn = !redOn;
    applyPixel();
    result = redOn ? "Red is on." : "Red is off.";
    return true;
}

bool toggleGreen(String& result) {
    greenOn = !greenOn;
    applyPixel();
    result = greenOn ? "Green is on." : "Green is off.";
    return true;
}

bool toggleBlue(String& result) {
    blueOn = !blueOn;
    applyPixel();
    result = blueOn ? "Blue is on." : "Blue is off.";
    return true;
}


void sendUpdate(bool (*update)(String&)) {
    String result;
    const bool success = update(result);
    Serial.println(result);
    server.sendHeader("Cache-Control", "no-store");
    server.send(success ? 200 : 502, "text/plain; charset=utf-8", result);
}


// Main setup
void setup() {
    delay(3000);  // Wait for the serial monitor to open
    Serial.begin(115200);

    // Connect to WiFi
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSsid, wifiPassword);

    Serial.print("Connecting to Wi-Fi: ");
    Serial.println(wifiSsid);

    // Web routes
    server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", page); });
    server.on("/location", HTTP_GET, []() { server.send(200, "text/plain; charset=utf-8", locationName); });
    server.on("/weather", HTTP_GET, []() { sendUpdate(getWeather); });
    server.on("/clock", HTTP_GET, []() { sendUpdate(getCentralTime); });
    server.on("/red", HTTP_GET, []() { sendUpdate(toggleRed); });
    server.on("/green", HTTP_GET, []() { sendUpdate(toggleGreen); });
    server.on("/blue", HTTP_GET, []() { sendUpdate(toggleBlue); });

    // Set initial states
    pinMode(1, INPUT_PULLDOWN);
    pinMode(2, INPUT_PULLDOWN);
    pinMode(NEOPIXEL_POWER, OUTPUT);
    digitalWrite(NEOPIXEL_POWER, NEOPIXEL_POWER_ON);
    applyPixel();  // starts with all channels off
}

///////////////////////////////////////////////// MAIN CODE /////////////////////////////////////////////////
void loop() {
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !wasConnected) {
        Serial.print("Connected. Open http://");
        Serial.println(WiFi.localIP());
        configTzTime(centralTimeZone, "pool.ntp.org", "time.nist.gov");
        if (!serverStarted) {
            server.begin();
            serverStarted = true;
        }
        Serial.println("Synchronizing the clock with NTP. Use the web page to request updates.");
    } else if (!connected && wasConnected) {
        Serial.println("Wi-Fi disconnected; reconnecting.");
    }

    wasConnected = connected;

    if (connected) {
        server.handleClient();
    } else if (millis() - lastReconnectMs >= 15000) {
        lastReconnectMs = millis();
        Serial.println("Still connecting to Wi-Fi...");
        WiFi.reconnect();
    }

    delay(5);
}