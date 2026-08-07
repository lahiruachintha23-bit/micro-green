#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "DHT.h"

// ==================== Pin Definitions ====================
// Ultrasonic sensor
#define TRIG_PIN 5
#define ECHO_PIN 18
// Motor driver (H-bridge)
#define IN1 25
#define IN2 26
// Soil moisture sensor
#define SOIL_PIN 34
// Relay for water pump (LOW = ON, HIGH = OFF)
#define RELAY_PIN 33
// Flow sensor (YF-S201)
#define FLOW_PIN 27
// AM2301 temperature and humidity sensor
#define AM2301_PIN 4
// Float switch (water tank level)
#define FLOAT_SWITCH_PIN 35
// Fan relays
#define FAN1_RELAY 16
#define FAN2_RELAY 17
#define FAN3_RELAY 19
// Water mister relay
#define WATER_MISTER_RELAY 21

// ==================== Constants ====================
// Motor run time (ms)
#define MOTOR_RUN_TIME 12000 // 12 seconds
// Soil threshold (wet when > value)
#define SOIL_THRESHOLD 2000
// Flow threshold (ml/min) – above this stops the pump
#define FLOW_THRESHOLD 900.0
// Growth stage thresholds (distance in cm)
#define GERMINATION_DISTANCE 25 // < 25cm = germination stage
#define GROWTH_DISTANCE 15      // 15-25cm = growth stage
#define HARVEST_DISTANCE 10     // <= 10cm = harvest stage
// Flow sensor conversion
#define PULSES_PER_LITER 450.0 // YF-S201 ~ 450 pulses per liter
// DHT sensor type (AM2301 is DHT21 equivalent)
#define DHT_TYPE DHT21
// Temperature and humidity thresholds
#define TEMP_THRESHOLD 28.0   // °C - turn fans on if temp > this
#define HUMIDITY_THRESHOLD 60 // % - turn fans on if humidity > this
// Mister spray duration
#define MISTER_SPRAY_DURATION 10000 // milliseconds (10 seconds)
// Germination period for mister operation
#define GERMINATION_PERIOD_DAYS 3 // days after first motor trigger

// WiFi configuration
const char *STA_SSID = "HUAWEI Y9 Prime 2019";
const char *STA_PASSWORD = "lasantha";
const char *AP_SSID = "MicrogreenTray_AP";
const char *AP_PASSWORD = "microgreen123";

WebServer server(80);

// ==================== Global Variables ====================
// DHT sensor for temperature and humidity
DHT dht(AM2301_PIN, DHT_TYPE);
float temperature = 0.0; // °C
float humidity = 0.0;    // %
unsigned long lastDHTReadTime = 0;
const unsigned long DHT_READ_INTERVAL = 2000; // 2 seconds (DHT21 min interval)

// Float switch (water tank level)
bool floatSwitchState = false;     // current state
bool lastFloatSwitchState = false; // previous state for change detection
unsigned long lastFloatSwitchChangeTime = 0;

// Fan control
bool fansActive = false;

// Mister control
bool misterActive = false;
unsigned long misterSprayStartTime = 0;
bool misterSprayInProgress = false;

// Germination tracking
unsigned long germinationStartTime = 0; // millis() when motor first triggers
const char *GERMINATION_FILE = "/germination_time.json";

// Mister schedule state (for tracking if misters should work today)
bool misterScheduledToday_6am = false;
bool misterScheduledToday_6pm = false;
// Motor state
bool motorTriggered = false; // trigger only once
bool motorRunning = false;   // currently running
unsigned long motorStartTime = 0;
bool motorManualMode = false; // manual override

// Flow sensor
volatile int pulseCount = 0;    // pulses counted in the current interval
float flow_ml_min = 0.0;        // last computed flow rate (ml/min)
unsigned long lastFlowTime = 0; // last time flow was computed
bool lastPumpWasOn = false;     // detect pump state change

// Plant growth stage
String growthStage = "Unknown";
long currentHeight = 0;

// Pump control mode
String pumpMode = "Auto"; // Auto, ManualOn, ManualOff

// Fan control mode
String fanMode = "Auto"; // Auto, ManualOn, ManualOff

// Mister control mode
String misterMode = "Auto"; // Auto, ManualOn, ManualOff

// Serial throttling
const unsigned long SERIAL_INTERVAL = 1000; // ms between status prints
unsigned long lastSerialTime = 0;
int lastSoilValue = -1;
long lastDistance = -1;
float lastFlowMlMin = 0.0;
String lastPumpState = "OFF";

// History logging
const char *HEIGHT_FILE = "/height_history.json";
const char *WATER_FILE = "/water_events.json";
const char *FLOAT_SWITCH_FILE = "/float_switch_events.json";

// ==================== Germination Time Management ====================
void saveGerminationTime(unsigned long time)
{
  File file = SPIFFS.open(GERMINATION_FILE, "w");
  StaticJsonDocument<64> doc;
  doc["time"] = time;
  serializeJson(doc, file);
  file.close();
  Serial.print("Germination time saved: ");
  Serial.println(time);
}

unsigned long loadGerminationTime()
{
  if (!SPIFFS.exists(GERMINATION_FILE))
  {
    return 0;
  }
  File file = SPIFFS.open(GERMINATION_FILE, "r");
  StaticJsonDocument<64> doc;
  deserializeJson(doc, file);
  file.close();
  return doc["time"].as<unsigned long>();
}

unsigned long getDaysSinceGermination()
{
  if (germinationStartTime == 0)
  {
    return 0;
  }
  unsigned long elapsedMs = millis() - germinationStartTime;
  unsigned long days = elapsedMs / (24UL * 60UL * 60UL * 1000UL);
  return days;
}

// ==================== Float Switch Event Logging ====================
void logFloatSwitchEvent(bool state)
{
  if (!SPIFFS.exists(FLOAT_SWITCH_FILE))
  {
    File file = SPIFFS.open(FLOAT_SWITCH_FILE, "w");
    file.print("[]");
    file.close();
  }

  File file = SPIFFS.open(FLOAT_SWITCH_FILE, "r");
  StaticJsonDocument<4096> doc;
  deserializeJson(doc, file);
  file.close();

  JsonObject entry = doc.createNestedObject();
  entry["time"] = millis() / 1000;
  entry["state"] = state ? "water_low" : "water_ok";

  // Keep only last 10 days
  const unsigned long RETENTION = 864000;
  unsigned long now = millis() / 1000;
  JsonArray arr = doc.as<JsonArray>();
  int idx = 0;
  while (idx < arr.size())
  {
    unsigned long entryTime = arr[idx]["time"].as<unsigned long>();
    if (now - entryTime > RETENTION)
    {
      arr.remove(idx);
    }
    else
    {
      idx++;
    }
  }

  file = SPIFFS.open(FLOAT_SWITCH_FILE, "w");
  serializeJson(doc, file);
  file.close();
}

String readFloatSwitchEvents()
{
  if (!SPIFFS.exists(FLOAT_SWITCH_FILE))
  {
    return "[]";
  }
  File file = SPIFFS.open(FLOAT_SWITCH_FILE, "r");
  String data = file.readString();
  file.close();
  return data;
}

// ==================== Interrupt Service Routine ====================
void IRAM_ATTR countPulse()
{
  pulseCount++; // called on every rising edge of the flow sensor
}

// ==================== Distance Measurement ====================
long readDistanceCM()
{
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseInLong(ECHO_PIN, HIGH, 50000); // 50 ms timeout
  if (duration == 0)
  {
    return -1;
  }

  long distance = duration * 0.034 / 2;
  return distance;
}

String calculateGrowthStage(long distance)
{
  if (distance <= 0)
  {
    return "Unknown";
  }
  if (distance <= HARVEST_DISTANCE)
  {
    return "Harvest";
  }
  if (distance <= GROWTH_DISTANCE)
  {
    return "Growth";
  }
  return "Germination";
}

String getPumpState()
{
  return digitalRead(RELAY_PIN) == LOW ? "ON" : "OFF";
}

String getPumpModeLabel()
{
  if (pumpMode == "ManualOn")
  {
    return "Manual On";
  }
  if (pumpMode == "ManualOff")
  {
    return "Manual Off";
  }
  return "Auto";
}

String getFanModeLabel()
{
  if (fanMode == "ManualOn")
  {
    return "Manual On";
  }
  if (fanMode == "ManualOff")
  {
    return "Manual Off";
  }
  return "Auto";
}

String getMisterModeLabel()
{
  if (misterMode == "ManualOn")
  {
    return "Manual On";
  }
  if (misterMode == "ManualOff")
  {
    return "Manual Off";
  }
  return "Auto";
}

// ==================== SPIFFS Data Functions ====================
void logHeightData(long height)
{
  if (!SPIFFS.exists(HEIGHT_FILE))
  {
    File file = SPIFFS.open(HEIGHT_FILE, "w");
    file.print("[]");
    file.close();
  }

  File file = SPIFFS.open(HEIGHT_FILE, "r");
  StaticJsonDocument<8192> doc;
  deserializeJson(doc, file);
  file.close();

  JsonObject entry = doc.createNestedObject();
  entry["time"] = millis() / 1000;
  entry["height"] = height;
  entry["stage"] = growthStage;

  // Keep only last 10 days of data (86400 * 10 seconds)
  const unsigned long RETENTION = 864000;
  unsigned long now = millis() / 1000;
  JsonArray arr = doc.as<JsonArray>();
  int idx = 0;
  while (idx < arr.size())
  {
    unsigned long entryTime = arr[idx]["time"].as<unsigned long>();
    if (now - entryTime > RETENTION)
    {
      arr.remove(idx);
    }
    else
    {
      idx++;
    }
  }

  file = SPIFFS.open(HEIGHT_FILE, "w");
  serializeJson(doc, file);
  file.close();
}

void logWaterEvent()
{
  if (!SPIFFS.exists(WATER_FILE))
  {
    File file = SPIFFS.open(WATER_FILE, "w");
    file.print("[]");
    file.close();
  }

  File file = SPIFFS.open(WATER_FILE, "r");
  StaticJsonDocument<4096> doc;
  deserializeJson(doc, file);
  file.close();

  JsonObject entry = doc.createNestedObject();
  entry["time"] = millis() / 1000;
  entry["duration"] = "pump_on";

  // Keep only last 10 days
  const unsigned long RETENTION = 864000;
  unsigned long now = millis() / 1000;
  JsonArray arr = doc.as<JsonArray>();
  int idx = 0;
  while (idx < arr.size())
  {
    unsigned long entryTime = arr[idx]["time"].as<unsigned long>();
    if (now - entryTime > RETENTION)
    {
      arr.remove(idx);
    }
    else
    {
      idx++;
    }
  }

  file = SPIFFS.open(WATER_FILE, "w");
  serializeJson(doc, file);
  file.close();
}

String readHeightHistory()
{
  if (!SPIFFS.exists(HEIGHT_FILE))
  {
    return "[]";
  }
  File file = SPIFFS.open(HEIGHT_FILE, "r");
  String data = file.readString();
  file.close();
  return data;
}

String readWaterEvents()
{
  if (!SPIFFS.exists(WATER_FILE))
  {
    return "[]";
  }
  File file = SPIFFS.open(WATER_FILE, "r");
  String data = file.readString();
  file.close();
  return data;
}

void handleRoot()
{
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>Microgreen Tray Dashboard</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js@3.9.1/dist/chart.min.js"></script>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      padding: 20px;
      color: #333;
    }
    .container { max-width: 1200px; margin: 0 auto; }
    h1 {
      color: white;
      font-size: 2.5em;
      margin-bottom: 30px;
      text-align: center;
      font-weight: 700;
    }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 20px; margin-bottom: 30px; }
    .card {
      background: white;
      border-radius: 15px;
      padding: 25px;
      box-shadow: 0 10px 40px rgba(0,0,0,0.15);
      transition: transform 0.3s, box-shadow 0.3s;
    }
    .card:hover { transform: translateY(-5px); box-shadow: 0 15px 50px rgba(0,0,0,0.2); }
    .stat-label { font-size: 0.9em; color: #666; font-weight: 600; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 8px; }
    .stat-value { font-size: 2.5em; font-weight: 700; color: #667eea; }
    .stat-unit { font-size: 0.6em; color: #999; margin-left: 5px; }
    .stage-badge {
      display: inline-block;
      padding: 8px 16px;
      border-radius: 20px;
      font-weight: 600;
      margin-top: 10px;
    }
    .stage-germination { background: #ffeaa7; color: #d63031; }
    .stage-growth { background: #74b9ff; color: #0984e3; }
    .stage-harvest { background: #a29bfe; color: #6c5ce7; }
    .stage-unknown { background: #dfe6e9; color: #636e72; }
    .controls {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
      gap: 10px;
      margin-top: 15px;
    }
    button {
      padding: 12px 16px;
      border: none;
      border-radius: 8px;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.3s;
      font-size: 0.9em;
    }
    .btn-primary { background: #667eea; color: white; }
    .btn-primary:hover { background: #5568d3; }
    .btn-danger { background: #ff7675; color: white; }
    .btn-danger:hover { background: #d63031; }
    .btn-success { background: #55efc4; color: #222; }
    .btn-success:hover { background: #00b894; }
    .btn-default { background: #dfe6e9; color: #222; }
    .btn-default:hover { background: #b2bec3; }
    .btn:disabled { opacity: 0.5; cursor: not-allowed; }
    .chart-container {
      position: relative;
      height: 300px;
      background: white;
      border-radius: 15px;
      padding: 20px;
      box-shadow: 0 10px 40px rgba(0,0,0,0.15);
      margin-bottom: 30px;
    }
    .mode-toggle {
      display: flex;
      gap: 10px;
      margin-bottom: 15px;
      background: #f0f0f0;
      border-radius: 8px;
      padding: 5px;
    }
    .mode-btn {
      flex: 1;
      padding: 8px;
      border: none;
      background: transparent;
      cursor: pointer;
      font-weight: 600;
      border-radius: 6px;
      transition: all 0.3s;
    }
    .mode-btn.active { background: white; box-shadow: 0 2px 8px rgba(0,0,0,0.1); }
    .status-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 15px; margin-top: 15px; }
    .status-item { background: #f8f9fa; padding: 12px; border-radius: 8px; text-align: center; }
    .status-item label { display: block; font-size: 0.85em; color: #666; margin-bottom: 5px; }
    .status-item span { display: block; font-size: 1.3em; font-weight: 700; color: #333; }
  </style>
</head>
<body>
  <div class="container">
    <h1>🌱 Smart Microgreen Tray</h1>

    <div class="grid">
      <div class="card">
        <div class="stat-label">Plant Height</div>
        <div class="stat-value"><span id="height">--</span><span class="stat-unit">cm</span></div>
        <div id="stageBadge" class="stage-badge stage-unknown">Unknown</div>
      </div>

      <div class="card">
        <div class="stat-label">Soil Moisture</div>
        <div class="stat-value"><span id="soilValue">--</span></div>
        <div class="status-grid" style="margin-top: 10px;">
          <div class="status-item"><label>Threshold</label><span>2000</span></div>
          <div class="status-item"><label>Status</label><span id="soilStatus">--</span></div>
        </div>
      </div>

      <div class="card">
        <div class="stat-label">Flow Rate</div>
        <div class="stat-value"><span id="flowValue">--</span><span class="stat-unit">ml/min</span></div>
        <div class="status-grid" style="margin-top: 10px;">
          <div class="status-item"><label>Threshold</label><span>900</span></div>
          <div class="status-item"><label>Safe</label><span id="flowStatus">--</span></div>
        </div>
      </div>

      <div class="card">
        <div class="stat-label">Temperature & Humidity</div>
        <div style="display: flex; gap: 15px; margin-top: 15px;">
          <div style="flex: 1;">
            <div style="font-size: 0.9em; color: #666; margin-bottom: 8px;">Temperature</div>
            <div class="stat-value"><span id="temperature">--</span><span class="stat-unit">°C</span></div>
          </div>
          <div style="flex: 1;">
            <div style="font-size: 0.9em; color: #666; margin-bottom: 8px;">Humidity</div>
            <div class="stat-value"><span id="humidity">--</span><span class="stat-unit">%</span></div>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="stat-label">Water Tank Level</div>
        <div class="status-grid" style="margin-top: 15px;">
          <div class="status-item" style="grid-column: 1 / -1;">
            <label>Status</label>
            <span id="floatSwitchStatus" style="font-size: 1.5em;">✓ OK</span>
          </div>
          <div style="grid-column: 1 / -1; padding: 10px; background: #f0f0f0; border-radius: 8px; margin-top: 10px; display: none;" id="waterLowAlert">
            <span style="color: #d63031; font-weight: bold;">⚠️ LOW WATER LEVEL</span>
          </div>
        </div>
      </div>
    </div>

    <div class="grid">
      <div class="card">
        <div class="stat-label">Fan Control</div>
        <div class="mode-toggle">
          <button class="mode-btn active" onclick="setFanMode('Auto')">Auto</button>
          <button class="mode-btn" onclick="setFanMode('ManualOn')">ON</button>
          <button class="mode-btn" onclick="setFanMode('ManualOff')">OFF</button>
        </div>
        <div class="status-grid">
          <div class="status-item"><label>Mode</label><span id="fanMode">Auto</span></div>
          <div class="status-item"><label>Status</label><span id="fansStatus">OFF</span></div>
          <div class="status-item"><label>Trigger</label><span style="font-size: 0.8em;">Temp >28°C<br/>Humidity >60%</span></div>
        </div>
      </div>

      <div class="card">
        <div class="stat-label">Mister Control</div>
        <div class="mode-toggle">
          <button class="mode-btn active" onclick="setMisterMode('Auto')">Auto</button>
          <button class="mode-btn" onclick="setMisterMode('ManualOn')">ON</button>
          <button class="mode-btn" onclick="setMisterMode('ManualOff')">OFF</button>
        </div>
        <div class="status-grid">
          <div class="status-item"><label>Mode</label><span id="misterMode">Auto</span></div>
          <div class="status-item"><label>Status</label><span id="misterStatus">OFF</span></div>
          <div class="status-item"><label>Days</label><span id="daysSinceGermination">--</span></div>
        </div>
        <div class="controls" style="margin-top: 10px;">
          <button class="btn-primary" onclick="misterSprayNow()">Spray Now (10s)</button>
        </div>
        <div style="margin-top: 15px; padding: 10px; background: #ffeaa7; border-radius: 8px; display: none;" id="misterCountdown">
          <span style="font-weight: bold;">Misters active for <span id="misterDaysRemaining">0</span> more days</span>
        </div>
      </div>
    </div>

    <div class="grid">
      <div class="card">
        <div class="stat-label">Pump Control</div>
        <div class="mode-toggle">
          <button class="mode-btn active" onclick="setMode('Auto')">Auto</button>
          <button class="mode-btn" onclick="setMode('ManualOn')">ON</button>
          <button class="mode-btn" onclick="setMode('ManualOff')">OFF</button>
        </div>
        <div class="status-grid">
          <div class="status-item"><label>Mode</label><span id="pumpMode">Auto</span></div>
          <div class="status-item"><label>State</label><span id="pumpState">OFF</span></div>
        </div>
      </div>

      <div class="card">
        <div class="stat-label">Motor Control</div>
        <div class="controls">
          <button class="btn-success" onclick="motorAction('on')">Motor ON</button>
          <button class="btn-danger" onclick="motorAction('off')">Motor OFF</button>
          <button class="btn-default" onclick="motorAction('reset')">Reset</button>
        </div>
        <div class="status-grid" style="margin-top: 15px;">
          <div class="status-item"><label>Status</label><span id="motorStatus">OFF</span></div>
        </div>
      </div>

      <div class="card">
        <div class="stat-label">Germination Control</div>
        <div class="controls">
          <button class="btn-success" onclick="germinationAction('start')">Start</button>
          <button class="btn-danger" onclick="germinationAction('reset')">Reset</button>
        </div>
        <div class="status-grid" style="margin-top: 15px;">
          <div class="status-item"><label>Status</label><span id="germinationStatus">Not Started</span></div>
        </div>
      </div>
    </div>

    <div class="chart-container">
      <canvas id="heightChart"></canvas>
    </div>

    <div class="chart-container">
      <canvas id="waterChart"></canvas>
    </div>
  </div>

  <script>
    let heightChart, waterChart;
    let lastFloatSwitchState = null;
    let lastDaysSinceGermination = -1;

    function updateStatus() {
      fetch('/status')
        .then(r => r.json())
        .then(data => {
          document.getElementById('height').textContent = data.distance > 0 ? data.distance : '--';
          document.getElementById('soilValue').textContent = data.soilValue;
          document.getElementById('soilStatus').textContent = data.soilValue > 2000 ? 'Wet' : 'Dry';
          document.getElementById('flowValue').textContent = parseFloat(data.flowValue).toFixed(1);
          document.getElementById('flowStatus').textContent = data.flowValue > 900 ? '⚠️ High' : '✓ OK';
          document.getElementById('pumpMode').textContent = data.pumpMode;
          document.getElementById('pumpState').textContent = data.pumpState;
          document.getElementById('motorStatus').textContent = data.motorStatus || 'OFF';
          
          // Update fan mode and mister mode displays
          document.getElementById('fanMode').textContent = data.fanMode || 'Auto';
          document.getElementById('misterMode').textContent = data.misterMode || 'Auto';
          document.getElementById('germinationStatus').textContent = data.daysSinceGermination > 0 ? 'Active (' + data.daysSinceGermination + ' days)' : 'Not Started';

          // Update temperature and humidity
          document.getElementById('temperature').textContent = parseFloat(data.temperature).toFixed(1);
          document.getElementById('humidity').textContent = parseFloat(data.humidity).toFixed(1);

          // Update float switch status
          const floatStatusSpan = document.getElementById('floatSwitchStatus');
          const waterLowAlert = document.getElementById('waterLowAlert');
          if (data.floatSwitch) {
            floatStatusSpan.textContent = '⚠️ LOW';
            floatStatusSpan.style.color = '#d63031';
            waterLowAlert.style.display = 'block';
            if (lastFloatSwitchState !== true) {
              showNotification('⚠️ LOW WATER LEVEL DETECTED! Check water tank immediately.');
            }
          } else {
            floatStatusSpan.textContent = '✓ OK';
            floatStatusSpan.style.color = '#00b894';
            waterLowAlert.style.display = 'none';
            if (lastFloatSwitchState === true) {
              showNotification('✓ Water level restored to normal');
            }
          }
          lastFloatSwitchState = data.floatSwitch;

          // Update fan status
          const fansStatusSpan = document.getElementById('fansStatus');
          if (data.fansActive) {
            fansStatusSpan.textContent = '🔄 ON';
            fansStatusSpan.style.color = '#00b894';
          } else {
            fansStatusSpan.textContent = 'OFF';
            fansStatusSpan.style.color = '#333';
          }

          // Update mister status
          const misterStatusSpan = document.getElementById('misterStatus');
          const daysSinceGerm = data.daysSinceGermination || 0;
          const daysRemaining = Math.max(0, 3 - daysSinceGerm);
          
          document.getElementById('daysSinceGermination').textContent = daysSinceGerm;
          
          if (daysSinceGerm < 3) {
            const misterCountdown = document.getElementById('misterCountdown');
            misterCountdown.style.display = 'block';
            document.getElementById('misterDaysRemaining').textContent = daysRemaining;
            
            if (data.misterActive) {
              misterStatusSpan.textContent = '💦 SPRAYING';
              misterStatusSpan.style.color = '#0984e3';
            } else {
              misterStatusSpan.textContent = 'Ready (6am/6pm)';
              misterStatusSpan.style.color = '#f39c12';
            }
          } else {
            misterStatusSpan.textContent = 'OFF (germination done)';
            misterStatusSpan.style.color = '#666';
            document.getElementById('misterCountdown').style.display = 'none';
          }
          
          if (lastDaysSinceGermination !== daysSinceGerm && daysSinceGerm >= 3 && lastDaysSinceGermination >= 0) {
            showNotification('✓ Germination period ended. Misters have been turned off. Use main water pump for watering.');
          }
          lastDaysSinceGermination = daysSinceGerm;

          const stageClasses = ['stage-germination', 'stage-growth', 'stage-harvest', 'stage-unknown'];
          const badge = document.getElementById('stageBadge');
          stageClasses.forEach(c => badge.classList.remove(c));
          badge.textContent = data.growthStage;
          badge.classList.add('stage-' + data.growthStage.toLowerCase());

          updateModeButtons(data.pumpMode);
        })
        .catch(e => console.error(e));
    }

    function showNotification(message) {
      // Simple notification - you can enhance this with a toast/popup library
      console.log('NOTIFICATION: ' + message);
      // Create a temporary alert div if desired
      const notifDiv = document.createElement('div');
      notifDiv.style.cssText = 'position: fixed; top: 20px; right: 20px; background: #667eea; color: white; padding: 15px 20px; border-radius: 8px; box-shadow: 0 4px 12px rgba(0,0,0,0.2); z-index: 1000; max-width: 300px;';
      notifDiv.textContent = message;
      document.body.appendChild(notifDiv);
      setTimeout(() => notifDiv.remove(), 5000);
    }

    function setMode(mode) {
      fetch('/control?mode=' + mode).then(updateStatus).catch(e => console.error(e));
    }

    function motorAction(action) {
      fetch('/motor?action=' + action).then(updateStatus).catch(e => console.error(e));
    }

    function setFanMode(mode) {
      fetch('/fan?mode=' + mode).then(updateStatus).catch(e => console.error(e));
    }

    function setMisterMode(mode) {
      fetch('/mister?mode=' + mode).then(updateStatus).catch(e => console.error(e));
    }

    function misterSprayNow() {
      fetch('/mister?action=spray').then(updateStatus).catch(e => console.error(e));
    }

    function germinationAction(action) {
      fetch('/germination?action=' + action).then(updateStatus).catch(e => console.error(e));
    }

    function updateModeButtons(mode) {
      document.querySelectorAll('.mode-btn').forEach((btn, idx) => {
        btn.classList.remove('active');
        const modes = ['Auto', 'ManualOn', 'ManualOff'];
        if (modes[idx] === mode) btn.classList.add('active');
      });
    }

    async function loadCharts() {
      try {
        const heightData = await fetch('/heightHistory').then(r => r.json());
        const waterData = await fetch('/waterEvents').then(r => r.json());

        const heightLabels = heightData.map(d => new Date(d.time * 1000).toLocaleTimeString());
        const heightValues = heightData.map(d => d.height);

        const heightCtx = document.getElementById('heightChart').getContext('2d');
        if (heightChart) heightChart.destroy();
        heightChart = new Chart(heightCtx, {
          type: 'line',
          data: {
            labels: heightLabels,
            datasets: [{
              label: 'Plant Height (cm)',
              data: heightValues,
              borderColor: '#667eea',
              backgroundColor: 'rgba(102, 126, 234, 0.1)',
              tension: 0.4,
              fill: true,
              pointRadius: 3,
              pointBackgroundColor: '#667eea'
            }]
          },
          options: {
            responsive: true,
            maintainAspectRatio: false,
            plugins: { legend: { display: true, position: 'top' } },
            scales: { y: { beginAtZero: true, title: { display: true, text: 'Height (cm)' } } }
          }
        });

        const waterLabels = waterData.map(d => new Date(d.time * 1000).toLocaleTimeString());
        const waterValues = waterData.map((d, idx) => idx + 1);

        const waterCtx = document.getElementById('waterChart').getContext('2d');
        if (waterChart) waterChart.destroy();
        waterChart = new Chart(waterCtx, {
          type: 'bar',
          data: {
            labels: waterLabels,
            datasets: [{
              label: 'Water Events',
              data: waterValues,
              backgroundColor: '#55efc4',
              borderColor: '#00b894',
              borderWidth: 2
            }]
          },
          options: {
            responsive: true,
            maintainAspectRatio: false,
            plugins: { legend: { display: true, position: 'top' } },
            scales: { y: { beginAtZero: true, ticks: { stepSize: 1 } } }
          }
        });
      } catch (e) {
        console.error('Chart load error:', e);
      }
    }

    setInterval(updateStatus, 1000);
    setInterval(loadCharts, 5000);
    updateStatus();
    loadCharts();
  </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html.c_str());
}

void handleStatus()
{
  char flowStr[16];
  dtostrf(flow_ml_min, 5, 1, flowStr);
  char tempStr[16];
  dtostrf(temperature, 5, 1, tempStr);
  char humStr[16];
  dtostrf(humidity, 5, 1, humStr);

  String pumpModeLabel = getPumpModeLabel();
  String pumpStateLabel = getPumpState();
  unsigned long daysSinceGerm = getDaysSinceGermination();

  String fanModeLabel = getFanModeLabel();
  String misterModeLabel = getMisterModeLabel();

  char buffer[1024];
  snprintf(buffer, sizeof(buffer),
           "{\"distance\":%ld,\"growthStage\":\"%s\",\"soilValue\":%d,\"flowValue\":%s,\"pumpMode\":\"%s\",\"pumpState\":\"%s\",\"motorStatus\":\"%s\",\"temperature\":%s,\"humidity\":%s,\"floatSwitch\":%s,\"fansActive\":%s,\"fanMode\":\"%s\",\"misterActive\":%s,\"misterMode\":\"%s\",\"daysSinceGermination\":%lu}",
           currentHeight,
           growthStage.c_str(),
           analogRead(SOIL_PIN),
           flowStr,
           pumpModeLabel.c_str(),
           pumpStateLabel.c_str(),
           motorRunning ? "ON" : "OFF",
           tempStr,
           humStr,
           floatSwitchState ? "true" : "false",
           fansActive ? "true" : "false",
           fanModeLabel.c_str(),
           misterSprayInProgress ? "true" : "false",
           misterModeLabel.c_str(),
           daysSinceGerm);

  server.send(200, "application/json", buffer);
}

void handleControl()
{
  if (!server.hasArg("mode"))
  {
    server.send(400, "text/plain", "mode query missing");
    return;
  }
  String mode = server.arg("mode");
  if (mode == "Auto" || mode == "ManualOn" || mode == "ManualOff")
  {
    pumpMode = mode;
    server.send(200, "text/plain", "OK");
  }
  else
  {
    server.send(400, "text/plain", "invalid mode");
  }
}

void handleMotorControl()
{
  if (!server.hasArg("action"))
  {
    server.send(400, "text/plain", "action query missing");
    return;
  }
  String action = server.arg("action");
  if (action == "on")
  {
    motorManualMode = true;
    digitalWrite(IN1, HIGH);
    digitalWrite(IN2, LOW);
    motorRunning = true;
    motorStartTime = millis();
    server.send(200, "text/plain", "Motor ON");
  }
  else if (action == "off")
  {
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
    motorRunning = false;
    motorManualMode = false;
    server.send(200, "text/plain", "Motor OFF");
  }
  else if (action == "reset")
  {
    motorTriggered = false;
    motorManualMode = false;
    motorRunning = false;
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
    server.send(200, "text/plain", "Motor RESET");
  }
  else
  {
    server.send(400, "text/plain", "invalid action");
  }
}

void handleHeightHistory()
{
  server.send(200, "application/json", readHeightHistory().c_str());
}

void handleWaterEvents()
{
  server.send(200, "application/json", readWaterEvents().c_str());
}

void handleFloatSwitchEvents()
{
  server.send(200, "application/json", readFloatSwitchEvents().c_str());
}

void handleFanControl()
{
  if (!server.hasArg("mode"))
  {
    server.send(400, "text/plain", "mode query missing");
    return;
  }
  String mode = server.arg("mode");
  if (mode == "Auto" || mode == "ManualOn" || mode == "ManualOff")
  {
    fanMode = mode;
    if (mode == "ManualOn")
    {
      digitalWrite(FAN1_RELAY, LOW);
      digitalWrite(FAN2_RELAY, LOW);
      digitalWrite(FAN3_RELAY, LOW);
      fansActive = true;
      Serial.println("Fans turned ON (manual)");
    }
    else if (mode == "ManualOff")
    {
      digitalWrite(FAN1_RELAY, HIGH);
      digitalWrite(FAN2_RELAY, HIGH);
      digitalWrite(FAN3_RELAY, HIGH);
      fansActive = false;
      Serial.println("Fans turned OFF (manual)");
    }
    server.send(200, "text/plain", "OK");
  }
  else
  {
    server.send(400, "text/plain", "invalid mode");
  }
}

void handleMisterControl()
{
  if (!server.hasArg("action"))
  {
    server.send(400, "text/plain", "action query missing");
    return;
  }
  String action = server.arg("action");
  if (action == "mode")
  {
    if (!server.hasArg("mode"))
    {
      server.send(400, "text/plain", "mode query missing");
      return;
    }
    String mode = server.arg("mode");
    if (mode == "Auto" || mode == "ManualOn" || mode == "ManualOff")
    {
      misterMode = mode;
      if (mode == "ManualOff")
      {
        digitalWrite(WATER_MISTER_RELAY, HIGH);
        misterSprayInProgress = false;
        Serial.println("Mister turned OFF (manual)");
      }
      server.send(200, "text/plain", "OK");
    }
    else
    {
      server.send(400, "text/plain", "invalid mode");
    }
  }
  else if (action == "spray")
  {
    // Manual spray for 10 seconds
    misterSprayInProgress = true;
    misterSprayStartTime = millis();
    digitalWrite(WATER_MISTER_RELAY, LOW);
    Serial.println("Mister spray triggered (manual) - 10 seconds");
    server.send(200, "text/plain", "OK");
  }
  else
  {
    server.send(400, "text/plain", "invalid action");
  }
}

void handleGerminationControl()
{
  if (!server.hasArg("action"))
  {
    server.send(400, "text/plain", "action query missing");
    return;
  }
  String action = server.arg("action");
  if (action == "start")
  {
    if (germinationStartTime == 0)
    {
      germinationStartTime = millis();
      saveGerminationTime(germinationStartTime);
      Serial.println("Germination manually started");
      server.send(200, "text/plain", "OK");
    }
    else
    {
      server.send(400, "text/plain", "Germination already started");
    }
  }
  else if (action == "reset")
  {
    germinationStartTime = 0;
    File file = SPIFFS.open(GERMINATION_FILE, "w");
    file.print("{}");
    file.close();
    Serial.println("Germination reset");
    server.send(200, "text/plain", "OK");
  }
  else
  {
    server.send(400, "text/plain", "invalid action");
  }
}

void handleNotFound()
{
  server.send(404, "text/plain", "Not found");
}

String wifiStatusToString(wl_status_t status)
{
  switch (status)
  {
  case WL_IDLE_STATUS:
    return "WL_IDLE_STATUS";
  case WL_NO_SSID_AVAIL:
    return "WL_NO_SSID_AVAIL";
  case WL_SCAN_COMPLETED:
    return "WL_SCAN_COMPLETED";
  case WL_CONNECTED:
    return "WL_CONNECTED";
  case WL_CONNECT_FAILED:
    return "WL_CONNECT_FAILED";
  case WL_CONNECTION_LOST:
    return "WL_CONNECTION_LOST";
  case WL_DISCONNECTED:
    return "WL_DISCONNECTED";
  default:
    return "UNKNOWN";
  }
}

void printWiFiState(const char *label)
{
  String localIp = WiFi.localIP().toString();
  String apIp = WiFi.softAPIP().toString();
  Serial.print(label);
  Serial.print(" | mode=");
  Serial.print(WiFi.getMode());
  Serial.print(" | status=");
  Serial.print(wifiStatusToString(WiFi.status()));
  Serial.print(" | STA IP=");
  Serial.print(localIp);
  Serial.print(" | AP IP=");
  Serial.println(apIp);
}

// ==================== Setup ====================
void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nSmart Microgreen Tray System Starting...");

  // Initialize SPIFFS
  if (!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS Mount Failed");
  }
  else
  {
    Serial.println("SPIFFS Mounted Successfully");
  }

  // Ultrasonic pins
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // Motor pins
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);

  // Relay pin (pump off initially)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // HIGH = pump OFF

  // Flow sensor pin with interrupt
  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), countPulse, RISING);

  // AM2301 sensor (DHT21)
  dht.begin();
  Serial.println("DHT21 sensor initialized");

  // Float switch pin
  pinMode(FLOAT_SWITCH_PIN, INPUT_PULLUP);
  floatSwitchState = digitalRead(FLOAT_SWITCH_PIN);
  lastFloatSwitchState = floatSwitchState;
  Serial.print("Float switch initialized, state: ");
  Serial.println(floatSwitchState ? "LOW (water low)" : "OK (water ok)");

  // Fan relay pins (HIGH = OFF)
  pinMode(FAN1_RELAY, OUTPUT);
  pinMode(FAN2_RELAY, OUTPUT);
  pinMode(FAN3_RELAY, OUTPUT);
  digitalWrite(FAN1_RELAY, HIGH); // OFF
  digitalWrite(FAN2_RELAY, HIGH); // OFF
  digitalWrite(FAN3_RELAY, HIGH); // OFF
  Serial.println("Fan relays initialized (OFF)");

  // Mister relay pin (HIGH = OFF)
  pinMode(WATER_MISTER_RELAY, OUTPUT);
  digitalWrite(WATER_MISTER_RELAY, HIGH); // OFF
  Serial.println("Mister relay initialized (OFF)");

  // Load germination time from SPIFFS
  germinationStartTime = loadGerminationTime();
  if (germinationStartTime > 0)
  {
    Serial.print("Germination time loaded from SPIFFS: ");
    Serial.println(germinationStartTime);
  }
  else
  {
    Serial.println("No germination time found in SPIFFS");
  }

  // Register web server routes (so server handlers are available in STA or AP)
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/control", handleControl);
  server.on("/motor", handleMotorControl);
  server.on("/fan", handleFanControl);
  server.on("/mister", handleMisterControl);
  server.on("/germination", handleGerminationControl);
  server.on("/heightHistory", handleHeightHistory);
  server.on("/waterEvents", handleWaterEvents);
  server.on("/floatSwitchEvents", handleFloatSwitchEvents);
  server.onNotFound(handleNotFound);

  // Try connecting as WiFi station (STA) first, then fall back to AP mode
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.setSleep(false);

  WiFi.mode(WIFI_STA);
  Serial.print("Attempting WiFi STA connect to: ");
  Serial.println(STA_SSID);
  WiFi.begin(STA_SSID, STA_PASSWORD);
  unsigned long staStart = millis();
  const unsigned long STA_TIMEOUT = 15000; // 15s
  while (WiFi.status() != WL_CONNECTED && (millis() - staStart) < STA_TIMEOUT)
  {
    delay(500);
    Serial.print('.');
    if ((millis() - staStart) % 2000 < 500)
    {
      printWiFiState("STA connect");
    }
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println();
    Serial.print("Connected to WiFi (STA): ");
    Serial.println(STA_SSID);
    Serial.print("Local IP: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println();
    Serial.println("STA connect failed, starting AP fallback");

    IPAddress apIP(192, 168, 50, 1);
    IPAddress gateway(192, 168, 50, 1);
    IPAddress subnet(255, 255, 255, 0);

    WiFi.mode(WIFI_AP);
    if (!WiFi.softAPConfig(apIP, gateway, subnet))
    {
      Serial.println("Warning: softAPConfig failed");
    }

    bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD);
    if (!apStarted)
    {
      Serial.println("Error: WiFi.softAP failed to start");
    }

    unsigned long apStart = millis();
    while ((WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) && (millis() - apStart < 5000))
    {
      delay(100);
    }

    Serial.print("WiFi AP started: ");
    Serial.println(AP_SSID);
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
    printWiFiState("AP fallback");
  }

  server.begin();
  Serial.println("Web server started");

  // Initialize flow timer
  lastFlowTime = millis();
}

// ==================== Main Loop ====================
void loop()
{
  // ----- 1. Ultrasonic distance measurement -----
  long distance = readDistanceCM();
  if (distance > 0)
  {
    currentHeight = distance;
    growthStage = calculateGrowthStage(distance);
    lastDistance = distance;

    // Log height data every 5 minutes
    static unsigned long lastHeightLog = 0;
    if (millis() - lastHeightLog >= 300000)
    {
      logHeightData(currentHeight);
      lastHeightLog = millis();
    }
  }
  else
  {
    lastDistance = -1;
  }

  // ----- 2. Motor driver logic -----
  if (!motorManualMode && distance > 0 && distance <= 17 && !motorTriggered && !motorRunning)
  {
    digitalWrite(IN1, HIGH);
    digitalWrite(IN2, LOW);
    motorRunning = true;
    motorStartTime = millis();
    Serial.println("Motor ON for 12 seconds");
  }

  if (motorRunning && (millis() - motorStartTime >= MOTOR_RUN_TIME))
  {
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
    motorRunning = false;
    motorTriggered = true;
    motorManualMode = false;
    Serial.println("Motor OFF");

    // Save germination time on first motor trigger
    if (germinationStartTime == 0)
    {
      germinationStartTime = millis();
      saveGerminationTime(germinationStartTime);
      Serial.println("Germination event detected - mister schedule starts");
    }
  }

  // ----- 3. AM2301 Temperature and Humidity Reading -----
  if (millis() - lastDHTReadTime >= DHT_READ_INTERVAL)
  {
    lastDHTReadTime = millis();
    float temp = dht.readTemperature();
    float hum = dht.readHumidity();

    if (!isnan(temp) && !isnan(hum))
    {
      temperature = temp;
      humidity = hum;
    }
    else
    {
      // Keep last known values on read failure
      Serial.println("DHT read failed, using last known values");
    }
  }

  // ----- 4. Float Switch Reading -----
  floatSwitchState = digitalRead(FLOAT_SWITCH_PIN);
  if (floatSwitchState != lastFloatSwitchState)
  {
    lastFloatSwitchState = floatSwitchState;
    lastFloatSwitchChangeTime = millis();
    logFloatSwitchEvent(floatSwitchState);
    if (floatSwitchState)
    {
      Serial.println("ALERT: Float switch triggered - Water level LOW");
    }
    else
    {
      Serial.println("Float switch reset - Water level OK");
    }
  }

  // ----- 5. Fan Control Logic -----
  bool shouldFansBeOn = (temperature > TEMP_THRESHOLD) && (humidity > HUMIDITY_THRESHOLD);
  if (fanMode == "Auto")
  {
    // Auto mode: turn on if conditions met, off if not
    if (shouldFansBeOn && !fansActive)
    {
      digitalWrite(FAN1_RELAY, LOW); // ON
      digitalWrite(FAN2_RELAY, LOW); // ON
      digitalWrite(FAN3_RELAY, LOW); // ON
      fansActive = true;
      Serial.print("Fans turned ON (auto) - Temp: ");
      Serial.print(temperature);
      Serial.print("°C, Humidity: ");
      Serial.print(humidity);
      Serial.println("%");
    }
    else if (!shouldFansBeOn && fansActive)
    {
      digitalWrite(FAN1_RELAY, HIGH); // OFF
      digitalWrite(FAN2_RELAY, HIGH); // OFF
      digitalWrite(FAN3_RELAY, HIGH); // OFF
      fansActive = false;
      Serial.println("Fans turned OFF (auto) - Temperature or humidity below threshold");
    }
  }
  // ManualOn and ManualOff are handled directly in handleFanControl()

  // ----- 6. Mister Control Logic -----
  unsigned long daysSinceGermination = getDaysSinceGermination();
  bool curtainOpen = motorTriggered; // Curtain opens after first motor trigger
  bool shouldMisterWork = (daysSinceGermination < GERMINATION_PERIOD_DAYS) && !curtainOpen;

  if (misterMode == "ManualOn" && !misterSprayInProgress)
  {
    // Manual ON: keep spray active
    digitalWrite(WATER_MISTER_RELAY, LOW); // ON
    misterSprayInProgress = true;
    Serial.println("Mister ON (manual continuous)");
  }
  else if (misterMode == "ManualOff" && misterSprayInProgress)
  {
    // Manual OFF: stop spray
    digitalWrite(WATER_MISTER_RELAY, HIGH); // OFF
    misterSprayInProgress = false;
    Serial.println("Mister OFF (manual)");
  }
  else if (misterMode == "Auto" && shouldMisterWork)
  {
    // Get current hour (simplified - assumes device time is set correctly)
    // For now, using a simple check based on elapsed time since start
    // TODO: In production, use proper RTC or NTP time
    time_t now = time(nullptr);
    struct tm *timeinfo = localtime(&now);
    int currentHour = timeinfo->tm_hour;

    // Check for 6am and 6pm spray times
    static bool spray6amDone = false;
    static bool spray6pmDone = false;

    if (currentHour == 6 && !spray6amDone)
    {
      // Time for 6am spray
      if (!misterSprayInProgress)
      {
        misterSprayInProgress = true;
        misterSprayStartTime = millis();
        digitalWrite(WATER_MISTER_RELAY, LOW); // ON
        Serial.println("Mister spray START - 6am schedule");
      }
      spray6amDone = true;
      spray6pmDone = false; // reset PM flag
    }
    else if (currentHour == 18 && !spray6pmDone)
    {
      // Time for 6pm spray
      if (!misterSprayInProgress)
      {
        misterSprayInProgress = true;
        misterSprayStartTime = millis();
        digitalWrite(WATER_MISTER_RELAY, LOW); // ON
        Serial.println("Mister spray START - 6pm schedule");
      }
      spray6pmDone = true;
      spray6amDone = false; // reset AM flag
    }
    else if (currentHour != 6 && currentHour != 18)
    {
      spray6amDone = false;
      spray6pmDone = false;
    }

    // Stop spray after 10 seconds
    if (misterSprayInProgress && (millis() - misterSprayStartTime >= MISTER_SPRAY_DURATION))
    {
      digitalWrite(WATER_MISTER_RELAY, HIGH); // OFF
      misterSprayInProgress = false;
      Serial.println("Mister spray STOP - 10 seconds completed");
    }
  }
  else if (misterMode == "Auto")
  {
    // After germination period or when curtain is open
    if (misterSprayInProgress)
    {
      digitalWrite(WATER_MISTER_RELAY, HIGH); // OFF
      misterSprayInProgress = false;
      Serial.println("Mister spray STOP - Germination period ended or curtain open");
    }
  }
  if (millis() - lastFlowTime >= 1000)
  {
    noInterrupts();
    int pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    float flowRate_L_min = (float)pulses / PULSES_PER_LITER * 60.0;
    flow_ml_min = flowRate_L_min * 1000.0;
    lastFlowTime = millis();

    // store for throttled serial output
    lastFlowMlMin = flow_ml_min;
  }

  // ----- 4. Soil moisture reading -----
  int soilValue = analogRead(SOIL_PIN);
  lastSoilValue = soilValue;

  // ----- 5. Pump control logic -----
  bool flowSafety = flow_ml_min > FLOW_THRESHOLD;
  if (flowSafety)
  {
    digitalWrite(RELAY_PIN, HIGH);
  }
  else if (pumpMode == "ManualOn")
  {
    digitalWrite(RELAY_PIN, LOW);
  }
  else if (pumpMode == "ManualOff")
  {
    digitalWrite(RELAY_PIN, HIGH);
  }
  else if (soilValue > SOIL_THRESHOLD)
  {
    digitalWrite(RELAY_PIN, LOW);
  }
  else
  {
    digitalWrite(RELAY_PIN, HIGH);
  }

  // update cached pump state and log water events
  bool pumpIsOn = (digitalRead(RELAY_PIN) == LOW);
  if (pumpIsOn && !lastPumpWasOn)
  {
    logWaterEvent();
    lastPumpWasOn = true;
  }
  else if (!pumpIsOn && lastPumpWasOn)
  {
    lastPumpWasOn = false;
  }

  lastPumpState = getPumpState();

  // ----- throttled serial output -----
  if (millis() - lastSerialTime >= SERIAL_INTERVAL)
  {
    lastSerialTime = millis();
    Serial.print("Flow: ");
    Serial.print(lastFlowMlMin, 1);
    Serial.print(" ml/min | Soil: ");
    Serial.print(lastSoilValue);
    Serial.print(" | Distance: ");
    if (lastDistance > 0)
    {
      Serial.print(lastDistance);
      Serial.print(" cm");
    }
    else
    {
      Serial.print("Out of range");
    }
    Serial.print(" | Stage: ");
    Serial.print(growthStage);
    Serial.print(" | PumpMode: ");
    Serial.print(getPumpModeLabel());
    Serial.print(" | Pump: ");
    Serial.println(lastPumpState);
  }

  server.handleClient();
  delay(50);
}
