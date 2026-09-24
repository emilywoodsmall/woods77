// OdoLogger J1939 - simple version
// reads the odometer from a heavy truck (J1939) and sends it over bluetooth
//
// hardware: raspberry pi pico 2 w + SN65HVD230 CAN transceiver
// arduino IDE setup:
//  - board package "Raspberry Pi Pico/RP2040/RP2350" (by Earle Philhower)
//  - library "ACAN2040" (library manager)
//  - Tools > IP/Bluetooth Stack > "IPv4 + Bluetooth"

#include <ACAN2040.h>
#include <SerialBT.h>

// ===== CHANGE FOR EACH TRUCK =====
String truckName = "TRUCK_01";
String bluetoothName = "OdoLogger_TRUCK_01";
// =================================

#define CAN_RX_PIN 4
#define CAN_TX_PIN 5
#define J1939_SPEED 250000   // almost all trucks use 250k

int howOften = 60000;  // send every 60 seconds

ACAN2040 *can;

// the truck puts the odometer in these messages
volatile float odometerKm = 0;
volatile bool gotOdometer = false;

unsigned long lastSent = 0;

// runs every time a CAN message comes in
void canCallback(struct can2040 *cd, uint32_t notify, struct can2040_msg *msg) {
  if (notify != CAN2040_NOTIFY_RX) {
    return;
  }

  // J1939 messages use the long 29 bit ID
  if ((msg->id & CAN2040_ID_EFF) == 0) {
    return;
  }

  // the PGN (what kind of message it is) is in the middle of the ID
  unsigned long id = msg->id & 0x1FFFFFFF;
  unsigned long pgn = (id >> 8) & 0xFFFF;

  // PGN 65248 = Vehicle Distance
  // bytes 4,5,6,7 = total distance, 0.125 km per bit, lowest byte first
  if (pgn == 65248) {
    unsigned long raw = msg->data[4] + (msg->data[5] * 256UL) + (msg->data[6] * 65536UL) + (msg->data[7] * 16777216UL);
    if (raw < 0xFB000000) {  // FFFFFFFF means "not available"
      odometerKm = raw * 0.125;
      gotOdometer = true;
    }
  }

  // PGN 65217 = High Resolution Vehicle Distance (some trucks only send this one)
  // bytes 0,1,2,3 = total distance, 5 meters (0.005 km) per bit
  if (pgn == 65217) {
    unsigned long raw = msg->data[0] + (msg->data[1] * 256UL) + (msg->data[2] * 65536UL) + (msg->data[3] * 16777216UL);
    if (raw < 0xFB000000) {
      odometerKm = raw * 0.005;
      gotOdometer = true;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("OdoLogger J1939 starting");

  SerialBT.setName(bluetoothName.c_str());
  SerialBT.begin();
  Serial.println("bluetooth on: " + bluetoothName);

  can = new ACAN2040(0, CAN_TX_PIN, CAN_RX_PIN, J1939_SPEED, F_CPU, canCallback);
  can->begin();
  Serial.println("listening to J1939 at 250k");

  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  if (millis() - lastSent > howOften || lastSent == 0) {
    lastSent = millis();

    if (gotOdometer == true) {
      float km = odometerKm;
      float miles = km * 0.621371;

      Serial.print("odometer: ");
      Serial.print(miles, 1);
      Serial.println(" miles");

      // send over bluetooth like:  ODO,TRUCK_01,123456.1
      if (SerialBT) {
        SerialBT.print("ODO,");
        SerialBT.print(truckName);
        SerialBT.print(",");
        SerialBT.println(km, 1);
      }

      digitalWrite(LED_BUILTIN, HIGH);
      delay(200);
      digitalWrite(LED_BUILTIN, LOW);
    } else {
      Serial.println("no odometer yet (is the key on?)");
      if (SerialBT) {
        SerialBT.println("ERR," + truckName + ",NO_ANSWER");
      }
    }
  }

  delay(50);
}
