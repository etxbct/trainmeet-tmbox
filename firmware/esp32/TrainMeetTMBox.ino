/*
 * TrainMeet physical TMBox — ESP32 firmware, 16x2 terminal profile
 *
 * The Raspberry Pi / TrainMeet Server is the traffic authority and draws
 * every screen: the box speaks tmbox/terminal/device/{id}/... (see
 * common/server_terminal.h and the server's docs/protocol/terminal16),
 * shows the frames it is sent and sends back key presses. Before it is
 * connected it shows its own status screens. Since 0.7.4 the old local
 * renderer and its v2 protocol are gone; Server 2.0.0 does not speak it.
 * Wi-Fi and MQTT are deliberately self-healing: a lost connection never
 * leaves the firmware in a dead loop.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoMqttClient.h>
#include <ESPmDNS.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Wire.h>

#include "hardware_profile.h"

// The screens and the state machine live in lib/tmbox_core, built and
// asserted in CI without a board. What this file does is talk to the network
// and the hardware; what a screen says and what a key means is decided there.
#include "model.h"
#include "attention.h"
#include "navigation.h"
#include "renderer.h"
#include "../common/lcd_text.h"
#include "../common/server_terminal.h"
#include "../common/server_discovery_arduino.h"

constexpr char FIRMWARE_VERSION[] = "0.7.7";
constexpr uint16_t DEFAULT_MQTT_PORT = 1883;
constexpr unsigned long SAVED_WIFI_WINDOW_MS = 15000;
constexpr unsigned long LOST_WIFI_PORTAL_DELAY_MS = 30000;
constexpr unsigned long WIFI_RETRY_MS = 5000;
constexpr unsigned long DISCOVERY_RETRY_MS = 4000;
constexpr unsigned long MQTT_RETRY_MIN_MS = 1000;
constexpr unsigned long MQTT_RETRY_MAX_MS = 8000;
constexpr unsigned long DEVICE_CODE_SCREEN_MS = 3000;

const byte ROWS = 4;
const byte COLS = 4;
char keyMap[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'},
};
byte rowPins[ROWS] = {
  TMBOX_ROW_PINS[0], TMBOX_ROW_PINS[1], TMBOX_ROW_PINS[2], TMBOX_ROW_PINS[3]
};
byte colPins[COLS] = {
  TMBOX_COL_PINS[0], TMBOX_COL_PINS[1], TMBOX_COL_PINS[2], TMBOX_COL_PINS[3]
};

LiquidCrystal_I2C lcd(TMBOX_LCD_ADDRESS, TMBOX_LCD_COLUMNS, TMBOX_LCD_ROWS);
Keypad keypad = Keypad(makeKeymap(keyMap), rowPins, colPins, ROWS, COLS);
WiFiManager wifiManager;
WiFiClient networkClient;
MqttClient mqttClient(networkClient);
ServerTerminal terminal;
Preferences preferences;
std::map<std::string, std::string> deviceMessages;
std::string deviceLanguage = "sv";

bool loadDeviceUI(JsonVariantConst ui) {
  if (ui["version"] != 1 || !ui["language"].is<const char*>() || !ui["messages"].is<JsonObjectConst>()) return false;
  const JsonArrayConst options = ui["languages"].as<JsonArrayConst>();
  bool found = false;
  if (options.size() == 0 || options.size() > 5) return false;
  for (JsonObjectConst option : options) {
    const String code = option["code"] | "", name = option["name"] | "";
    if (code.length() != 2 || name.length() == 0 || name.length() > 12) return false;
    found = found || code == ui["language"].as<const char*>();
  }
  if (!found) return false;
  deviceLanguage = ui["language"].as<const char*>();
  deviceMessages.clear();
  for (JsonPairConst item : ui["messages"].as<JsonObjectConst>())
    if (item.value().is<const char*>()) deviceMessages[item.key().c_str()] = item.value().as<const char*>();
  return true;
}

WiFiManagerParameter forgetServer("forgetserver", "Byt TrainMeet Server (behall Wi-Fi)", "1", 1, "type=\"checkbox\"", WFM_LABEL_AFTER);
bool forgetServerRequested = false;
String deviceId;
String deviceCode;
String accessPointName;
String rememberedServerId, discoveredServerId;
String gatewayHost;
uint16_t gatewayPort = DEFAULT_MQTT_PORT;

tmbox::StationConfig stationConfig;
tmbox::Snapshot stationSnapshot;
tmbox::LocalNavigationState navigation;
tmbox::AttentionController attention;

// What this box can physically show. Since 0.7.5 Å, Ä and Ö are drawn in
// CGRAM with the server's glyphs (lcd_text.h), so the renderer keeps them.
const tmbox::Geometry displayGeometry(TMBOX_LCD_ROWS, TMBOX_LCD_COLUMNS, true);

bool portalActive = false;
bool saveParametersRequested = false;
bool resetNetworkRequested = false;
bool mdnsStarted = false;
unsigned long bootAt = 0;
unsigned long wifiAttemptAt = 0;
unsigned long wifiLostAt = 0;
unsigned long nextWifiRetryAt = 0;
unsigned long nextDiscoveryAt = 0;
unsigned long nextMqttAttemptAt = 0;
unsigned long mqttRetryDelay = MQTT_RETRY_MIN_MS;
unsigned int failedMqttAttempts = 0;
// What is on the glass right now, so an unchanged screen is not rewritten.
TrainMeetLcd::Screen drawn(TMBOX_LCD_ROWS, TMBOX_LCD_COLUMNS);
bool drawnValid = false;

void drawScreen();
void drawLocal(const std::vector<std::string>& lines);
void showScreen(tmbox::Screen screen);
void showNetworkState();
void beginSavedWiFiAttempt();
void startSetupPortal();
void stopSetupPortal();
void processWiFi();
void processGateway();
void processSavedParameters();
void resetNetworkConfiguration();
bool discoverGateway();
bool connectMqtt();
void disconnectMqtt();
void onMqttMessage(int messageSize);
void signalAttention(const std::vector<tmbox::AttentionEvent>& events);
const char* attentionName(tmbox::Attention kind);
void keypadEvent(KeypadEvent key);
void buildIdentity();
String codeFromChipId(uint64_t chipId);

void setup() {
  Serial.begin(115200);
  Wire.begin(TMBOX_LCD_SDA, TMBOX_LCD_SCL);
  lcd.init();
  lcd.backlight();
  keypad.setHoldTime(5000);
  keypad.addEventListener(keypadEvent);

  preferences.begin("trainmeet", false);
  { JsonDocument cached; if (!deserializeJson(cached, preferences.getString("device-ui", ""))) loadDeviceUI(cached.as<JsonVariantConst>()); }
  rememberedServerId = preferences.getString("server-id", "");
  if (!TrainMeetNetwork::validServerId(rememberedServerId.c_str())) rememberedServerId = "";
  // Old manual IP settings are deliberately ignored, not migrated into identity.
  buildIdentity();
  bootAt = millis();
  showScreen(tmbox::Screen::Identity);

  wifiManager.setDebugOutput(false);
  wifiManager.addParameter(&forgetServer);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setConnectTimeout(15);
  wifiManager.setSaveConfigCallback([]() { saveParametersRequested = true; });
  wifiManager.setSaveParamsCallback([]() { forgetServerRequested = String(forgetServer.getValue()) == "1"; });
  wifiManager.setAPCallback([](WiFiManager*) { portalActive = true; });

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.setId(deviceId);
  mqttClient.setKeepAliveInterval(10 * 1000UL);
  mqttClient.setConnectionTimeout(4 * 1000UL);
  mqttClient.setCleanSession(true);
  beginSavedWiFiAttempt();
  if (!WiFi.SSID().length()) startSetupPortal();
}

void loop() {
  processSavedParameters();
  if (resetNetworkRequested) {
    resetNetworkConfiguration();
  }

  processWiFi();
  processGateway();
  if (terminal.started) {
    mqttClient.poll();
    if (!terminal.tick()) { disconnectMqtt(); showScreen(tmbox::Screen::SeekingServer); }
    else {
      if (terminal.fresh && String(terminal.frame["station_code"] | "").length() &&
          rememberedServerId != discoveredServerId) {
        preferences.putString("server-id", discoveredServerId);
        rememberedServerId = discoveredServerId;
      }
      const char key = keypad.getKey();
      if (key) terminal.press(key);
      terminal.draw(lcd, TMBOX_LCD_COLUMNS, TMBOX_LCD_ROWS);
    }
    delay(10);
    return;
  }
  // Without a terminal session there is nothing to send (since 0.7.4 there
  // is no v2 protocol): the box shows its own status screens. The keypad is
  // still read, so holding * opens setup.
  keypad.getKey();
  delay(10);
}

void beginSavedWiFiAttempt() {
  stopSetupPortal();
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  wifiAttemptAt = millis();
  wifiLostAt = 0;
  nextWifiRetryAt = 0;
}

void processWiFi() {
  const unsigned long now = millis();
  if (portalActive) {
    wifiManager.process();
    portalActive = wifiManager.getConfigPortalActive();
    processSavedParameters();
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiLostAt = 0;
    return;
  }

  disconnectMqtt();
  if (mdnsStarted) { MDNS.end(); mdnsStarted = false; }
  if (wifiLostAt == 0) {
    wifiLostAt = now;
    showNetworkState();
  }

  if (!portalActive && now - wifiAttemptAt >= SAVED_WIFI_WINDOW_MS &&
      now - wifiLostAt >= LOST_WIFI_PORTAL_DELAY_MS) {
    startSetupPortal();
    return;
  }

  if (!portalActive && now >= nextWifiRetryAt) {
    WiFi.reconnect();
    nextWifiRetryAt = now + WIFI_RETRY_MS;
  }
}

void startSetupPortal() {
  if (portalActive) return;
  disconnectMqtt();
  forgetServer.setValue("1", 1);
  WiFi.mode(WIFI_AP_STA);
  wifiManager.startConfigPortal(accessPointName.c_str());
  portalActive = wifiManager.getConfigPortalActive();
  showScreen(tmbox::Screen::SetupPortal);
}

void stopSetupPortal() {
  if (!portalActive) return;
  TrainMeetNetwork::finishSavedPortal(wifiManager, true);
  portalActive = false;
}

void processGateway() {
  const unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED || portalActive) return;

  if (mqttClient.connected()) {
    return;
  }

  if (now < nextDiscoveryAt || now < nextMqttAttemptAt) return;
  // Always re-resolve the identity before a new MQTT connection. A former IP
  // might now belong to a different server after DHCP reassignment.
  if (!discoverGateway()) {
    nextDiscoveryAt = now + DISCOVERY_RETRY_MS;
    return;
  }
  if (connectMqtt()) {
    mqttRetryDelay = MQTT_RETRY_MIN_MS;
    failedMqttAttempts = 0;
    return;
  }

  signalAttention(attention.observe_link(false));
  failedMqttAttempts++;
  nextMqttAttemptAt = now + mqttRetryDelay;
  mqttRetryDelay = min(mqttRetryDelay * 2UL, MQTT_RETRY_MAX_MS);
  showScreen(tmbox::Screen::ServerGone);
}

bool discoverGateway() {
  if (!mdnsStarted) {
    mdnsStarted = MDNS.begin(deviceId.c_str());
  }
  if (!mdnsStarted) return false;

  const auto servers = TrainMeetNetwork::discoverServers();
  const auto selected = TrainMeetNetwork::selectServer(servers, rememberedServerId.c_str());
  if (selected.index < 0) {
    showScreen(tmbox::Screen::SeekingServer);
    if (selected.status == TrainMeetNetwork::DiscoveryStatus::Ambiguous) {
      drawLocal({"FLERA SERVRAR", "BE ADMIN HJÄLPA"});
    }
    return false;
  }
  const auto& server = servers[selected.index];
  gatewayHost = server.host.c_str(); gatewayPort = server.port; discoveredServerId = server.id.c_str();
  return true;
}

bool connectMqtt() {
  showScreen(tmbox::Screen::SeekingServer);
  if (!mqttClient.connect(gatewayHost.c_str(), gatewayPort)) {
    return false;
  }

  // The cached catalog is keyed on the Swedish folded to ASCII; a message
  // equal to its key is that Swedish, and the box keeps its own spelling.
  const auto text = [](const char* source) {
    char key[48]; TrainMeetLcd::foldText(source, key, sizeof key);
    const auto found = deviceMessages.find(key);
    return String(found == deviceMessages.end() || found->second == key ? source : found->second.c_str());
  };
  terminal.waitingText = text("VÄNTAR PÅ SVAR"); terminal.unansweredText = text("INGET SVAR");
  terminal.begin(mqttClient, deviceId, deviceCode, "ESP32 TMBox 16x2", FIRMWARE_VERSION,
                 deviceId + "-" + String(esp_random(), HEX) + "-" + String(millis()));
  return true;
}

void disconnectMqtt() {
  terminal.reset();
  if (!mqttClient.connected()) return;
  mqttClient.stop();
}

void onMqttMessage(int messageSize) {
  if (messageSize < 1 || messageSize > 8192) {
    while (mqttClient.available()) mqttClient.read();
    return;
  }
  const String topic = mqttClient.messageTopic();
  String payload;
  payload.reserve(messageSize);
  while (mqttClient.available()) {
    payload += (char)mqttClient.read();
  }

  if (terminal.started) {
    terminal.receive(topic, payload, mqttClient.messageRetain());
    terminal.draw(lcd, TMBOX_LCD_COLUMNS, TMBOX_LCD_ROWS);
  }
}

void processSavedParameters() {
  if (forgetServerRequested) {
    forgetServerRequested = false;
    preferences.remove("server-id"); rememberedServerId = ""; discoveredServerId = ""; gatewayHost = "";
  }
  if (!saveParametersRequested) return;
  saveParametersRequested = false;
  disconnectMqtt(); gatewayHost = ""; nextMqttAttemptAt = nextDiscoveryAt = 0;
  if (mdnsStarted) MDNS.end();
  mdnsStarted = false;
  portalActive = TrainMeetNetwork::finishSavedPortal(wifiManager, WiFi.status() == WL_CONNECTED);
}

void keypadEvent(KeypadEvent key) {
  if (key == '*' && keypad.getState() == HOLD) {
    resetNetworkRequested = true;
  }
}

void resetNetworkConfiguration() {
  resetNetworkRequested = false;
  // Holding * opens setup. Only an explicit portal choice forgets the server;
  // neither the working Wi-Fi nor the permanent device identity is erased.
  startSetupPortal();
}

void showNetworkState() {
  if (millis() - bootAt < DEVICE_CODE_SCREEN_MS) return;
  showScreen(tmbox::Screen::NoNetwork);
}

// The box's own screen, with Å, Ä and Ö in CGRAM (lcd_text.h). Only a
// changed screen is written: an I2C display is slow enough that redrawing an
// unchanged one is visible as a flicker.
void drawLocal(const std::vector<std::string>& lines) {
  TrainMeetLcd::Screen screen(TMBOX_LCD_ROWS, TMBOX_LCD_COLUMNS);
  for (uint8_t row = 0; row < screen.rows && row < lines.size(); ++row) screen.line(row, lines[row].c_str());
  if (drawnValid && screen == drawn) return;
  drawn = screen; drawnValid = true;
  screen.draw(lcd);
}

void drawScreen() {
  if (terminal.started && terminal.fresh) {
    terminal.draw(lcd, TMBOX_LCD_COLUMNS, TMBOX_LCD_ROWS);
    // The server's frame and glyphs are on the glass now: the next local
    // screen is written even if it is the one shown before the session.
    drawnValid = false;
    return;
  }
  stationConfig.language = deviceLanguage;
  if (stationConfig.messages != deviceMessages) stationConfig.messages = deviceMessages;
  drawLocal(tmbox::render(displayGeometry, navigation.view(), stationConfig, stationSnapshot));
}

void showScreen(tmbox::Screen screen) {
  navigation.show(screen, millis());
  drawScreen();
}


void buildIdentity() {
  const uint64_t chipId = ESP.getEfuseMac();
  char idBuffer[25];
  snprintf(
    idBuffer,
    sizeof(idBuffer),
    "esp32-%04x%08x",
    (uint16_t)(chipId >> 32),
    (uint32_t)chipId
  );
  deviceId = String(idBuffer);
  deviceCode = codeFromChipId(chipId);
  // "TMBOX-" is 6 characters; the AP name carries just the 6-character code.
  accessPointName = "TrainMeet-" + deviceCode.substring(6);
}

String codeFromChipId(uint64_t chipId) {
  // TMBOX-XXXXXX: six symbols from a 32-character alphabet (5 bits each,
  // 30 bits total) — enough spread that two boxes on the same meeting
  // network collide only by extraordinary coincidence.
  constexpr char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  uint32_t value = ((uint32_t)chipId ^ (uint32_t)(chipId >> 24)) & 0x3FFFFFFF;
  char code[13] = "TMBOX-000000";
  for (int index = 11; index >= 6; --index) {
    code[index] = alphabet[value & 31];
    value >>= 5;
  }
  return String(code);
}

// The attention sink. The controller decides *whether*; this decides *how*,
// and with no buzzer defined the how is "log it and carry on" — the spec's
// graceful degradation. The serial line is not decoration: it is how a bench
// test sees that the box reached the right decision without any hardware to
// hear it with.
void signalAttention(const std::vector<tmbox::AttentionEvent>& events) {
  const tmbox::Attention loudest = tmbox::AttentionController::loudest(events);
  if (loudest == tmbox::Attention::None) return;

  Serial.print("[uppmarksamhet] ");
  Serial.println(attentionName(loudest));

  if (!TMBOX_HAS_BUZZER) return;

  // One buzzer, one sound. Losing the server gets the long note because it is
  // the one event that makes everything else on the display untrustworthy.
  unsigned int frequency = 2000;
  unsigned int duration = 120;
  switch (loudest) {
    case tmbox::Attention::ConnectionLost:     frequency = 700;  duration = 600; break;
    case tmbox::Attention::ConnectionRestored: frequency = 1400; duration = 120; break;
    case tmbox::Attention::IncomingRequest:    frequency = 2200; duration = 250; break;
    case tmbox::Attention::RequestDenied:      frequency = 900;  duration = 250; break;
    case tmbox::Attention::RequestApproved:    frequency = 2600; duration = 150; break;
    case tmbox::Attention::IncomingTrain:      frequency = 1800; duration = 150; break;
    case tmbox::Attention::None:               return;
  }
  tone(TMBOX_BUZZER_PIN, frequency, duration);
}

const char* attentionName(tmbox::Attention kind) {
  switch (kind) {
    case tmbox::Attention::None:               return "inget";
    case tmbox::Attention::ConnectionLost:     return "servern borta";
    case tmbox::Attention::IncomingRequest:    return "begaran hit";
    case tmbox::Attention::RequestDenied:      return "var begaran nekad";
    case tmbox::Attention::RequestApproved:    return "var begaran godkand";
    case tmbox::Attention::IncomingTrain:      return "linjen ledig mot oss";
    case tmbox::Attention::ConnectionRestored: return "servern tillbaka";
  }
  return "?";
}
