// J1939 Simulator (bench testing)
// Pretends to be a vehicle: broadcasts odometer and battery voltage on a J1939 bus
// so the OdoLogger can be tested at a desk without plugging into a real vehicle.
//
// Hardware: any Raspberry Pi Pico / Pico 2 (W not needed) + SN65HVD230 CAN transceiver
// Wire it to the OdoLogger's transceiver: CANH-CANH, CANL-CANL, GND-GND.
// On the bench, KEEP the 120 ohm resistor on both transceiver boards (one at each end).
//
// Sends every second (priority 6, source address 0x00):
//   PGN 65217 High Resolution Vehicle Distance   (5 m/bit)
//   PGN 65248 Vehicle Distance                   (125 m/bit)
//   PGN 65271 Vehicle Electrical Power (SPN 168) (0.05 V/bit)
//
// Serial monitor commands (115200):
//   h  = toggle sending the high-res distance (tests the low-res fallback)
//   p  = pause / resume everything        (tests the STALE / NO_DATA errors)

#include <ACAN2040.h>

#define CAN_RX_PIN 4
#define CAN_TX_PIN 5
#define J1939_BITRATE 250000

#define SOURCE_ADDR 0x00
#define PRIORITY 6

ACAN2040 *can;

uint64_t odometerMeters = 80467200ULL;   // start at 80,467.2 km (50,000 mi)
const uint32_t METERS_PER_SECOND = 11;   // about 40 km/h
float batteryVolts = 27.6;               // 24 V transit bus system

bool sendHighRes = true;
bool paused = false;
unsigned long lastTick = 0;

void canCallback(struct can2040 *cd, uint32_t notify, struct can2040_msg *msg) {
  // The simulator only transmits; nothing to do on receive.
}

uint32_t j1939Id(uint32_t pgn) {
  return ((uint32_t)PRIORITY << 26) | (pgn << 8) | SOURCE_ADDR;
}

void putLe32(uint8_t *d, uint32_t v) {
  d[0] = v & 0xFF; d[1] = (v >> 8) & 0xFF; d[2] = (v >> 16) & 0xFF; d[3] = (v >> 24) & 0xFF;
}

bool sendFrame(uint32_t pgn, const uint8_t *data) {
  struct can2040_msg msg;
  msg.id = j1939Id(pgn) | CAN2040_ID_EFF;
  msg.dlc = 8;
  memcpy(msg.data, data, 8);
  // wait briefly for room in the transmit queue
  unsigned long start = millis();
  while (!can->ok_to_send()) {
    if (millis() - start > 50) return false;
  }
  return can->send_message(&msg);
}

void sendAll() {
  uint8_t d[8];

  if (sendHighRes) {
    memset(d, 0xFF, 8);                                   // unused bytes = not available
    putLe32(&d[0], (uint32_t)(odometerMeters / 5));       // total distance, 5 m/bit
    putLe32(&d[4], 0xFFFFFFFF);                           // trip distance: not available
    sendFrame(65217, d);
  }

  memset(d, 0xFF, 8);
  putLe32(&d[0], 0xFFFFFFFF);                             // trip distance: not available
  putLe32(&d[4], (uint32_t)(odometerMeters / 125));       // total distance, 125 m/bit
  sendFrame(65248, d);

  memset(d, 0xFF, 8);
  uint16_t v = (uint16_t)(batteryVolts / 0.05 + 0.5);
  d[4] = v & 0xFF;
  d[5] = v >> 8;                                          // SPN 168, bytes 5-6
  sendFrame(65271, d);
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("J1939 simulator starting. h = toggle high-res, p = pause");

  can = new ACAN2040(0, CAN_TX_PIN, CAN_RX_PIN, J1939_BITRATE, F_CPU, canCallback);
  can->begin();
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'h') {
      sendHighRes = !sendHighRes;
      Serial.println(sendHighRes ? "high-res ON" : "high-res OFF (low-res only)");
    } else if (c == 'p') {
      paused = !paused;
      Serial.println(paused ? "PAUSED" : "running");
    }
  }

  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    if (!paused) {
      odometerMeters += METERS_PER_SECOND;
      sendAll();
      Serial.print("odometer km: ");
      Serial.println(odometerMeters / 1000.0, 3);
    }
  }
}
