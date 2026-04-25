#include <SPI.h>
#include <RF24.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <LoRa.h>

// ================= SPI =================
#define SCK 14
#define MISO 12
#define MOSI 13

// ================= BASE 1 =================
#define CE1 4
#define CSN1 2

// ================= BASE 2 =================
#define CE2 26
#define CSN2 25

// ================= LORA EXTRA =================
#define LORA_DIO0 33

// ================= OBJECTS =================
RF24 radio1(CE1, CSN1);
RF24 radio2(CE2, CSN2);

// ================= MODULE ENUM =================
enum ModuleType {
  MOD_NONE,
  MOD_NRF,
  MOD_CC1101,
  MOD_LORA
};

// ================= SELECT MODULES =================
ModuleType base1 = MOD_NRF;
ModuleType base2 = MOD_CC1101;

// ================= INIT FUNCTIONS =================

void initNRF(RF24 &radio) {
  radio.begin();
  radio.setPALevel(RF24_PA_LOW);
  radio.stopListening();
}

void initCC1101(int cs, int gdo0) {
  pinMode(gdo0, INPUT);

  ELECHOUSE_cc1101.setSpiPin(SCK, MISO, MOSI, cs);
  ELECHOUSE_cc1101.setGDO(gdo0, -1);

  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setMHZ(433.0);
}

void initLoRa(int cs, int rst, int dio0) {
  pinMode(rst, OUTPUT);
  digitalWrite(rst, HIGH);

  // reset pulse
  digitalWrite(rst, LOW);
  delay(10);
  digitalWrite(rst, HIGH);

  LoRa.setPins(cs, rst, dio0);

  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa init failed");
  }
}

void stopAll() {
  Serial.println("Stopping all modules...");

  // ---- NRF ----
  radio1.powerDown();
  radio2.powerDown();

  // ---- CC1101 ----
  ELECHOUSE_cc1101.setSidle();  // idle mode

  // ---- LoRa ----
  LoRa.end();

  // ---- Reset SPI CS pins ----
  pinMode(CSN1, OUTPUT);
  pinMode(CSN2, OUTPUT);

  digitalWrite(CSN1, HIGH);
  digitalWrite(CSN2, HIGH);

  // ---- Reset CE pins ----
  pinMode(CE1, INPUT);
  pinMode(CE2, INPUT);

  delay(200);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  SPI.begin(SCK, MISO, MOSI);

  // -------- BASE 1 --------
  if (base1 == MOD_NRF) {
    initNRF(radio1);
  }
  else if (base1 == MOD_CC1101) {
    initCC1101(CSN1, CE1);
  }
  else if (base1 == MOD_LORA) {
    initLoRa(CSN1, CE1, LORA_DIO0);
  }

  // -------- BASE 2 --------
  if (base2 == MOD_NRF) {
    initNRF(radio2);
  }
  else if (base2 == MOD_CC1101) {
    initCC1101(CSN2, CE2);
  }
  else if (base2 == MOD_LORA) {
    initLoRa(CSN2, CE2, LORA_DIO0);
  }
}

// ================= LOOP FUNCTIONS =================

void loopNRF(RF24 &radio, const char *msg) {
  radio.write(msg, strlen(msg));
}

void loopCC1101(const char *msg) {
  ELECHOUSE_cc1101.SendData(msg);
}

void loopLoRa(const char *msg) {
  LoRa.beginPacket();
  LoRa.print(msg);
  LoRa.endPacket();
}

// ================= MAIN LOOP =================
void loop() {

  // ----- BASE 1 -----
  if (base1 == MOD_NRF) {
    loopNRF(radio1, "B1-NRF");
  }
  else if (base1 == MOD_CC1101) {
    loopCC1101("B1-CC");
  }
  else if (base1 == MOD_LORA) {
    loopLoRa("B1-LORA");
  }

  // ----- BASE 2 -----
  if (base2 == MOD_NRF) {
    loopNRF(radio2, "B2-NRF");
  }
  else if (base2 == MOD_CC1101) {
    loopCC1101("B2-CC");
  }
  else if (base2 == MOD_LORA) {
    loopLoRa("B2-LORA");
  }

  delay(500);
}
