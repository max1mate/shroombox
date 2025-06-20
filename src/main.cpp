#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ==== WiFi Credentials ====
const char* ssid = "Home";
const char* password = "4803166053";

// ==== Hardware Pins ====
#define DHTPIN 4                 // DHT22 data pin
#define DHTTYPE DHT22
#define RELAY_PIN 13            // Relay for mister
#define LED_PIN 23              // LED alert
#define MIST_BUTTON_PIN 27      // Physical mist toggle button
#define I2C_SDA 22
#define I2C_SCL 19
#define BUZZER_PIN 12

// ==== Display Config ====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ==== Logic Constants ====
#define HUMIDITY_SETPOINT 75.0
#define HUMIDITY_BUFFER 2.0
#define MIST_DURATION 10000
#define WATER_OUT_TIMEOUT 120000
#define HUMIDITY_ALERT_TIMEOUT 900000  // 15 minutes
#define DEBOUNCE_DELAY 200

// ==== Objects and Globals ====
DHT my_sensor(DHTPIN, DHTTYPE);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

float humidity = 0.0, temperatureC = 0.0, temperatureF = 0.0;
bool misting = false, manualMode = false;
bool waterOut = false, humidityAlert = false;
bool autoMistingEnabled = true;

unsigned long mistStartTime = 0;
unsigned long totalMistingTime = 0;
unsigned long lastLoopTime = 0;
unsigned long lastSetpointTime = 0;
float humidityAtMistStart = 0.0;
unsigned long ledBlinkTimer = 0;
bool ledOn = false;
bool humidityAlertAttempted = false;
unsigned long humidityAlertMistStart = 0;


// === Debounce Button State ===
unsigned long lastMistButtonTime = 0;
bool lastMistButtonState = HIGH;

// === Logging ===
std::vector<String> csvLog;

// === HTML Web Dashboard ===
String generateDashboardHTML() {
  return R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8">
  <meta name='viewport' content='width=device-width, initial-scale=1.0'>
  <title>Shroombox Grow Monitor</title>
  <style>
    body { background: #111; color: #eee; font-family: Arial; text-align: center; padding: 20px; }
    button { padding: 10px; margin: 5px; background: #333; color: #fff; border: none; border-radius: 5px; cursor: pointer; }
    canvas { max-width: 90vw; margin-top: 20px; background: #222; border-radius: 10px; }
  </style>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
</head><body>
  <h2>Shroombox Grow Dashboard</h2>
  <p>Humidity: <span id='hum'>--</span> %</p>
  <p>Temperature: <span id='temp'>--</span> °F</p>
  <p>Mister: <span id='mist'>--</span></p>
  <p>Mister Uptime: <span id='uptime'>--</span></p>
  <p id="alert" style="color: red; font-weight: bold;"></p>
  <p>Auto Mode: <span id="autoStatus">--</span></p>
  <button onclick="fetch('/mister/on')">Mister ON</button>
  <button onclick="fetch('/mister/off')">Mister OFF</button>
  <button onclick="fetch('/reset_alert')">Reset Alerts</button>
  <button onclick="fetch('/toggle_auto')">Toggle Auto Mode</button>
  <button onclick="window.location='/data.csv'">Download CSV</button>
  <canvas id="chart"></canvas>
  <script>
    let labels = [], hum = [], temp = [];
    const chart = new Chart(document.getElementById('chart').getContext('2d'), {
      type: 'line',
      data: {
        labels: [], datasets: [
          { label: 'Humidity (%)', data: [], borderColor: 'aqua', backgroundColor: 'transparent' },
          { label: 'Temp (°F)', data: [], borderColor: 'orange', backgroundColor: 'transparent' }
        ]
      },
      options: {
        responsive: true, scales: { y: { beginAtZero: false } },
        plugins: { legend: { labels: { color: "#fff" } } }
      }
    });

    const ws = new WebSocket(`ws://${location.host}/ws`);
    ws.onmessage = function (event) {
      const data = JSON.parse(event.data);
      document.getElementById("hum").innerText = data.humidity.toFixed(1);
      document.getElementById("temp").innerText = data.temp_f.toFixed(1);
      document.getElementById("mist").innerText = data.misting ? "ON" : "OFF";
      document.getElementById("uptime").innerText = data.uptime.toFixed(2) + " %";
      document.getElementById("autoStatus").innerText = data.auto ? "Enabled" : "Disabled";

      const alertEl = document.getElementById("alert");
      alertEl.innerText = data.waterOut ? "⚠️ Water Out - Check Reservoir!" :
                         data.humidityAlert ? "⚠️ Humidity - Not Reaching Setpoint!" : "";

      const time = new Date().toLocaleTimeString();
      labels.push(time); hum.push(data.humidity); temp.push(data.temp_f);
      if (labels.length > 20) { labels.shift(); hum.shift(); temp.shift(); }

      chart.data.labels = labels;
      chart.data.datasets[0].data = hum;
      chart.data.datasets[1].data = temp;
      chart.update();
    };
  </script>
</body></html>
)rawliteral";
}

// === Bitmap Icons (8x8 or 16x16 pixels) ===
const uint8_t droplet_icon[] PROGMEM = {
  B00011000,
  B00111100,
  B01111110,
  B11111111,
  B11111111,
  B01111110,
  B00111100,
  B00011000
};

const uint8_t alert_icon[] PROGMEM = {
  B00011000,
  B00011000,
  B00011000,
  B00011000,
  B00000000,
  B00011000,
  B00011000,
  B00000000
};

const uint8_t wifi_icon[] PROGMEM = {
  B00000000,
  B00011000,
  B00111100,
  B01111110,
  B11011011,
  B10000001,
  B00011000,
  B00011000
};

// === Setup ===
void setup() {
  Serial.begin(9600);
  my_sensor.begin();
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(MIST_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  

   // Initialize I2C manually before SSD1306
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 not found");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.println("SSD1306 Ready");
    display.display();
  }

  // === SSD1306 OLED Display ===
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 not found");
  }
  display.clearDisplay();

  // === Connect WiFi ===
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  // === HTTP Routes ===
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/html", generateDashboardHTML());
  });

  server.on("/mister/on", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!waterOut) {
      misting = true; manualMode = true;
      mistStartTime = millis();
      humidityAtMistStart = humidity;
      digitalWrite(RELAY_PIN, HIGH);
      Serial.println("Mister turned ON (Server)");
    } else {
      Serial.println("⚠️ Server Mister blocked by Water Out");
    }
    request->send(200, "text/plain", "Mister ON");
  });

  server.on("/mister/off", HTTP_GET, [](AsyncWebServerRequest* request) {
    misting = false; manualMode = false;
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("Mister turned OFF (Server)");
    request->send(200, "text/plain", "Mister OFF");
  });

  server.on("/reset_alert", HTTP_GET, [](AsyncWebServerRequest* request) {
    waterOut = false;
    humidityAlert = false;
    digitalWrite(LED_PIN, LOW);
    Serial.println("Alerts Reset");
    request->send(200, "text/plain", "Alerts Reset");
  });

  server.on("/toggle_auto", HTTP_GET, [](AsyncWebServerRequest* request) {
    autoMistingEnabled = !autoMistingEnabled;
    Serial.println(String("Auto Misting: ") + (autoMistingEnabled ? "Enabled" : "Disabled"));
    request->send(200, "text/plain", autoMistingEnabled ? "Auto On" : "Auto Off");
  });

  server.on("/data.csv", HTTP_GET, [](AsyncWebServerRequest* request) {
    String csv = "Time (s),Humidity (%),Temperature (F)\n";
    for (auto &line : csvLog) csv += line + "\n";
    request->send(200, "text/csv", csv);
  });

  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client,
                AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      Serial.println("WebSocket client connected");
    }
  });

  server.addHandler(&ws);
  server.begin();
}

// === Main Loop ===
void loop() {
  unsigned long now = millis();
  unsigned long elapsed = now - lastLoopTime;
  lastLoopTime = now;

  humidity = my_sensor.readHumidity();
  temperatureC = my_sensor.readTemperature();
  temperatureF = temperatureC * 1.8 + 32;

  if (isnan(humidity) || isnan(temperatureC)) {
    Serial.println("Sensor error");
    delay(2000);
    return;
  }

  // Log misting uptime
  if (misting) totalMistingTime += elapsed;
  float uptimePercent = (float)totalMistingTime / now * 100.0;

  // Send JSON via WebSocket
  String json = "{\"humidity\":" + String(humidity, 1) +
                ",\"temp_f\":" + String(temperatureF, 1) +
                ",\"misting\":" + String(misting ? "true" : "false") +
                ",\"uptime\":" + String(uptimePercent, 2) +
                ",\"waterOut\":" + String(waterOut ? "true" : "false") +
                ",\"humidityAlert\":" + String(humidityAlert ? "true" : "false") +
                ",\"auto\":" + String(autoMistingEnabled ? "true" : "false") + "}";
  ws.textAll(json);

  // Save CSV data
  csvLog.push_back(String(now / 1000) + "," + String(humidity, 1) + "," + String(temperatureF, 1));
  if (csvLog.size() > 200) csvLog.erase(csvLog.begin());

  // 🔔 Blink LED and buzz if alert is active
if (waterOut || humidityAlert) {
  if (now - ledBlinkTimer > 500) {
    ledOn = !ledOn;
    digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
    digitalWrite(BUZZER_PIN, ledOn ? HIGH : LOW);  // Buzzer beeps with LED
    ledBlinkTimer = now;
  }
} else {
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);  // Make sure buzzer is silent if no alert
}

  // ✅ Clear humidity alert if setpoint is reached
if (humidity >= HUMIDITY_SETPOINT) {
  humidityAlert = false;
  humidityAlertAttempted = false;
  lastSetpointTime = now;
}

// 🚨 Trigger humidity alert if setpoint not met in time
if (!humidityAlert && (now - lastSetpointTime > HUMIDITY_ALERT_TIMEOUT)) {
  humidityAlert = true;
  humidityAlertAttempted = false;  // Reset attempt flag
  Serial.println("⚠️ Humidity Alert Triggered: Not reaching setpoint.");
}

// 🧪 Try misting for 30s once when alert is first triggered
if (humidityAlert && !humidityAlertAttempted) {
  humidityAlertMistStart = now;
  humidityAlertAttempted = true;
  misting = true;
  mistStartTime = now;
  humidityAtMistStart = humidity;
  digitalWrite(RELAY_PIN, HIGH);
  Serial.println("🔁 Attempting misting due to humidity alert...");
}

// ⏱️ Stop humidity alert misting attempt after 30s
if (humidityAlertAttempted && misting && (now - humidityAlertMistStart >= 30000)) {
  misting = false;
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("🛑 Finished humidity alert misting attempt.");
}


// === Update OLED ===
display.clearDisplay();
display.setTextColor(WHITE);

// === Humidity & Temp ===
display.setTextSize(2);
display.setCursor(20, 0);
display.print("H: ");
display.print(humidity, 0);
display.print("%");

display.setCursor(20, 20);
display.print("T: ");
display.print(temperatureF, 0);
display.print("F");

// === Icons ===
// Droplet icon when misting
if (misting) {
  static bool blink = false;
  blink = !blink;
  if (blink)
    display.drawBitmap(0, 0, droplet_icon, 8, 8, WHITE);
}

// Alert icon (humidity or water out)
if (humidityAlert || waterOut) {
  display.drawBitmap(0, 20, alert_icon, 8, 8, WHITE);
}

// Wi-Fi icon
display.drawBitmap(0, 52, wifi_icon, 8, 8, WHITE);

// === Misting Status ===
display.setTextSize(1);
display.setCursor(20, 44);
display.print("Mister: ");
display.println(misting ? "ON" : "OFF");

// === WiFi IP ===
display.setCursor(20, 54);
if (WiFi.status() == WL_CONNECTED) {
  display.print(WiFi.localIP());
} else {
  display.print("Connecting...");
}

display.display();


  // Setpoint timer tracking
  if (humidity >= HUMIDITY_SETPOINT) lastSetpointTime = now;
  if (now - lastSetpointTime > HUMIDITY_ALERT_TIMEOUT) humidityAlert = true;

  // Auto Misting Logic
  if (!manualMode && autoMistingEnabled && !waterOut &&
    humidity < (HUMIDITY_SETPOINT - HUMIDITY_BUFFER) && !misting) {
  misting = true;
  mistStartTime = now;
  humidityAtMistStart = humidity;
  digitalWrite(RELAY_PIN, HIGH);
  Serial.println("Mister turned ON (Auto)");
}

  if (!manualMode && misting && now - mistStartTime > MIST_DURATION) {
    misting = false;
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("Mister turned OFF (Auto)");
  }

  // Water Out Logic
  if (misting && (now - mistStartTime > WATER_OUT_TIMEOUT)) {
    if (humidity <= humidityAtMistStart + 1.0) {
      misting = false;
      waterOut = true;
      digitalWrite(RELAY_PIN, LOW);
      Serial.println("⚠️ Water Out Detected! Mister shut off.");
    }
  }

  // === Physical Button: Mist Toggle ===
bool mistButtonState = digitalRead(MIST_BUTTON_PIN);
if (mistButtonState == LOW && lastMistButtonState == HIGH && (now - lastMistButtonTime > DEBOUNCE_DELAY)) {
  lastMistButtonTime = now;
  manualMode = true;
  misting = !misting;
  if (misting) {
    mistStartTime = millis();
    humidityAtMistStart = humidity;
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println("Mister turned ON (Physical Button)");
  } else {
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("Mister turned OFF (Physical Button)");
  }
}
lastMistButtonState = mistButtonState;



  delay(2000);
}
