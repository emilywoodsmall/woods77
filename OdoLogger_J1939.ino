// OdoLogger J1939
// Reads the odometer from a heavy vehicle's J1939 CAN bus and sends it over Bluetooth.
//
// Hardware: Raspberry Pi Pico 2 W + SN65HVD230 CAN transceiver
// Arduino IDE setup:
//  - Board package "Raspberry Pi Pico/RP2040/RP2350" (Earle Philhower)
//  - Library "ACAN2040" (Library Manager)
//  - Tools > IP/Bluetooth Stack > "IPv4 + Bluetooth"
//
// Bluetooth output (one line per reading):
//   ODO,<vehicle>,<km>,<source>,<battery volts or blank>,<source address hex>
//   ODO,BUS_01,123456.125,J1939-HR,27.60,00
// Errors:
//   ERR,<vehicle>,NO_DATA   odometer never seen since power-up (key off? wrong bitrate?)
//   ERR,<vehicle>,STALE     odometer was seen, but not recently
// Send "READ" over Bluetooth to get a reading right away.

#include <ACAN2040.h>
#include <SerialBT.h>

// ===== CHANGE FOR EACH VEHICLE =====
const char *VEHICLE_NAME = "BUS_01";
// ===================================

#define CAN_RX_PIN 4
#define CAN_TX_PIN 5
#define CAN_PIO 0                 // CYW43 (WiFi/BT) prefers PIO1, so PIO0 is free for CAN
#define J1939_BITRATE 250000      // most vehicles; some newer ones use 500000 (J1939-14)

// Only trust the odometer from this source address. 0xFF = accept any sender.
// Several modules (engine 0x00, instrument cluster 0x17, ...) can broadcast distance
// with slightly different values; lock this to one once you know your vehicle.
#define ODO_SOURCE_ADDR 0xFF

const unsigned long SEND_INTERVAL_MS = 60000;  // send every 60 s
const unsigned long STALE_MS = 5000;           // odometer older than this is "stale"
const unsigned long HR_PREFERRED_MS = 5000;    // ignore low-res if high-res seen this recently

// J1939 PGNs used
#define PGN_HR_VEHICLE_DISTANCE 65217   // 0xFEC1, 5 m/bit, 4 bytes at data[0..3]
#define PGN_VEHICLE_DISTANCE    65248   // 0xFEE0, 125 m/bit, 4 bytes at data[4..7]
#define PGN_VEHICLE_ELEC_POWER  65271   // 0xFEF7, SPN 168 battery potential, 0.05 V/bit, data[4..5]

ACAN2040 *can;

// ---- shared with the CAN interrupt: only touch inside noInterrupts() in loop() ----
struct OdoState {
  uint32_t raw;            // raw counts, kept as an integer so no precision is lost
  bool highRes;            // true = 5 m/bit (65217), false = 125 m/bit (65248)
  uint8_t sourceAddr;
  unsigned long lastRxMs;  // millis() when the reading arrived
  unsigned long lastHrMs;  // millis() when a high-res reading last arrived
  bool valid;
  uint16_t batteryRaw;
  unsigned long batteryRxMs;
  bool batteryValid;
};
volatile OdoState state = {};
// ------------------------------------------------------------------------------------

unsigned long lastSent = 0;
bool sentOnce = false;
String btLine = "";

// Little-endian helpers (J1939 sends the lowest byte first)
uint32_t le32(const uint8_t *d) {
  return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}
uint16_t le16(const uint8_t *d) {
  return (uint16_t)d[0] | ((uint16_t)d[1] << 8);
}

// Decode the PGN from a 29-bit J1939 identifier.
//   bits 28-26 priority | 25 EDP | 24 DP | 23-16 PDU format (PF) | 15-8 PDU specific (PS) | 7-0 source addr
// If PF < 240 (PDU1) the PS byte is a destination address and is NOT part of the PGN.
uint32_t j1939Pgn(uint32_t id) {
  uint32_t pf = (id >> 16) & 0xFF;
  uint32_t pgn = (id >> 8) & 0x3FFFF;   // EDP + DP + PF + PS
  if (pf < 240) {
    pgn &= 0x3FF00;                     // PDU1: drop destination address
  }
  return pgn;
}

// Runs in interrupt context every time a CAN frame arrives. Keep it short.
void canCallback(struct can2040 *cd, uint32_t notify, struct can2040_msg *msg) {
  if (notify != CAN2040_NOTIFY_RX) return;
  if ((msg->id & CAN2040_ID_EFF) == 0) return;   // J1939 uses 29-bit extended IDs
  if (msg->id & CAN2040_ID_RTR) return;
  if (msg->dlc < 8) return;

  uint32_t id = msg->id & 0x1FFFFFFF;
  uint32_t pgn = j1939Pgn(id);
  uint8_t sa = id & 0xFF;
  unsigned long now = millis();

  if (pgn == PGN_HR_VEHICLE_DISTANCE || pgn == PGN_VEHICLE_DISTANCE) {
    if (ODO_SOURCE_ADDR != 0xFF && sa != ODO_SOURCE_ADDR) return;

    bool hr = (pgn == PGN_HR_VEHICLE_DISTANCE);
    uint32_t raw = hr ? le32(&msg->data[0]) : le32(&msg->data[4]);
    if (raw > 0xFAFFFFFF) return;   // 0xFB.. - 0xFF.. = error / not available

    if (hr) {
      state.lastHrMs = now;
    } else if (state.valid && state.highRes && (now - state.lastHrMs) < HR_PREFERRED_MS) {
      return;   // we have fresh high-res data; don't let the low-res value overwrite it
    }

    state.raw = raw;
    state.highRes = hr;
    state.sourceAddr = sa;
    state.lastRxMs = now;
    state.valid = true;
  } else if (pgn == PGN_VEHICLE_ELEC_POWER) {
    uint16_t raw = le16(&msg->data[4]);
    if (raw > 0xFAFF) return;
    state.batteryRaw = raw;
    state.batteryRxMs = now;
    state.batteryValid = true;
  }
}

void sendReading() {
  // Copy the shared state atomically so the interrupt can't change it half-way through
  OdoState s;
  noInterrupts();
  s.raw = state.raw;
  s.highRes = state.highRes;
  s.sourceAddr = state.sourceAddr;
  s.lastRxMs = state.lastRxMs;
  s.valid = state.valid;
  s.batteryRaw = state.batteryRaw;
  s.batteryRxMs = state.batteryRxMs;
  s.batteryValid = state.batteryValid;
  interrupts();

  unsigned long now = millis();
  String line;

  if (!s.valid) {
    line = String("ERR,") + VEHICLE_NAME + ",NO_DATA";
  } else if (now - s.lastRxMs > STALE_MS) {
    line = String("ERR,") + VEHICLE_NAME + ",STALE";
  } else {
    // Exact decimal math: 125 m/bit or 5 m/bit -> meters, then km with 3 decimals
    uint64_t meters = (uint64_t)s.raw * (s.highRes ? 5 : 125);
    char km[24];
    snprintf(km, sizeof(km), "%lu.%03lu", (unsigned long)(meters / 1000), (unsigned long)(meters % 1000));

    char volts[12] = "";
    if (s.batteryValid && now - s.batteryRxMs <= STALE_MS) {
      unsigned long centivolts = (unsigned long)s.batteryRaw * 5;   // 0.05 V/bit
      snprintf(volts, sizeof(volts), "%lu.%02lu", centivolts / 100, centivolts % 100);
    }

    char sa[4];
    snprintf(sa, sizeof(sa), "%02X", s.sourceAddr);

    line = String("ODO,") + VEHICLE_NAME + "," + km + "," +
           (s.highRes ? "J1939-HR" : "J1939") + "," + volts + "," + sa;

    digitalWrite(LED_BUILTIN, HIGH);
    delay(100);
    digitalWrite(LED_BUILTIN, LOW);
  }

  Serial.println(line);
  if (SerialBT) SerialBT.println(line);
}

// Read commands from the laptop ("READ" = send a reading now)
void checkBluetoothCommands() {
  while (SerialBT && SerialBT.available()) {
    char c = SerialBT.read();
    if (c == '\n' || c == '\r') {
      btLine.trim();
      if (btLine.equalsIgnoreCase("READ")) {
        sendReading();
      }
      btLine = "";
    } else if (btLine.length() < 32) {
      btLine += c;
    }
  }
}

void printCanStats() {
  struct can2040_stats st;
  can->get_statistics(&st);
  Serial.print("CAN rx_total=");
  Serial.print(st.rx_total);
  Serial.print(" parse_error=");
  Serial.println(st.parse_error);
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("OdoLogger J1939 starting");

  pinMode(LED_BUILTIN, OUTPUT);

  String btName = String("OdoLogger_") + VEHICLE_NAME;
  SerialBT.setName(btName.c_str());
  SerialBT.begin();
  Serial.println("bluetooth on: " + btName);

  can = new ACAN2040(CAN_PIO, CAN_TX_PIN, CAN_RX_PIN, J1939_BITRATE, F_CPU, canCallback);
  can->begin();
  Serial.print("listening to J1939 at ");
  Serial.println(J1939_BITRATE);
}

void loop() {
  checkBluetoothCommands();

  if (!sentOnce || millis() - lastSent >= SEND_INTERVAL_MS) {
    sentOnce = true;
    lastSent = millis();
    sendReading();
    printCanStats();   // lots of parse errors usually means wrong bitrate or bad wiring
  }

  delay(20);
}
