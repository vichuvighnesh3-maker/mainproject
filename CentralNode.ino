/*
  CentralNode.ino - ESP32 + SX1278 emergency message receiver
  Required library: "LoRa" by Sandeep Mistry
*/

#include <LoRa.h>

#define CENTRAL_NODE_ID 0  // Reserved for later routing logic
#define LORA_FREQUENCY 433E6

// ESP32 default VSPI is already used by the wiring; no SPI pin remapping is needed.
#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 2

struct __attribute__((packed)) Packet {
  uint8_t senderId;
  uint8_t destId;
  uint8_t msgId;
  char senderName[20];
  char message[64];
};

// Packet text fields may use every byte, so convert them to safe printable C strings first.
String packetText(const char *bytes, size_t length) {
  String text;
  text.reserve(length);
  for (size_t i = 0; i < length && bytes[i] != '\0'; ++i) {
    uint8_t c = (uint8_t)bytes[i];
    if (c >= 0x20 && c != 0x7F) text += (char)c;
  }
  text.trim();
  return text;
}

// Keep presentation separate from radio reception so OLED display, storage, or mesh forwarding
// can be added later without changing the low-level receive code.
void handleIncomingPacket(const Packet &packet) {
  String name = packetText(packet.senderName, sizeof(packet.senderName));
  String message = packetText(packet.message, sizeof(packet.message));
  if (name.length() == 0) name = "Anonymous";

  Serial.print("[Node ");
  Serial.print(packet.senderId);
  Serial.print("] ");
  Serial.print(name);
  Serial.print(": ");
  Serial.print(message);
  Serial.print(" (msg #");
  Serial.print(packet.msgId);
  Serial.println(")");
}

void receiveLoRaPacket() {
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;

  if (packetSize != sizeof(Packet)) {
    Serial.print("Ignored packet with unexpected length: ");
    Serial.println(packetSize);
    while (LoRa.available()) LoRa.read();
    return;
  }

  Packet packet;
  uint8_t *raw = (uint8_t *)&packet;
  for (size_t i = 0; i < sizeof(Packet); ++i) {
    if (!LoRa.available()) {
      Serial.println("Ignored incomplete LoRa packet.");
      return;
    }
    raw[i] = LoRa.read();
  }
  handleIncomingPacket(packet);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("LoRa init failed. Check SX1278 wiring/power.");
    while (true) delay(1000);
  }
  LoRa.receive();
  Serial.println("Central LoRa receiver ready.");
}

void loop() {
  // Polling is reliable here and keeps the receive path short; parsePacket() is non-blocking.
  receiveLoRaPacket();
}
