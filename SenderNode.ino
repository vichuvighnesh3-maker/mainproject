/*
  SenderNode.ino - ESP32 + SX1278 emergency message sender
  Required library: "LoRa" by Sandeep Mistry
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LoRa.h>

// ---- Values to change for each physical sender node ----
#define AP_SSID       "EmergencyNode_1"
#define AP_PASSWORD   ""              // Empty = open network. Set a password to secure it later.
#define SENDER_NODE_ID 1
#define LORA_FREQUENCY 433E6

// These are the ESP32 default VSPI pins; do not remap SPI in software.
#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 2

#define DNS_PORT 53
#define MAX_NAME_CHARS 20
#define MAX_MESSAGE_CHARS 64

// Packed makes the over-the-air representation explicit (87 bytes).
struct __attribute__((packed)) Packet {
  uint8_t senderId;
  uint8_t destId;       // 0 means broadcast / central node
  uint8_t msgId;
  char senderName[20]; // May contain 20 characters; not necessarily NUL-terminated.
  char message[64];    // May contain 64 characters; not necessarily NUL-terminated.
};

WebServer server(80);
DNSServer dnsServer;
uint8_t nextMessageId = 0;

const char PORTAL_PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Emergency Message</title><style>
body{margin:0;background:#101820;color:#fff;font:18px Arial,sans-serif}.wrap{max-width:560px;margin:auto;padding:24px}
h1{font-size:30px;margin:0 0 8px;color:#ffda44}p{line-height:1.4}label{display:block;font-weight:bold;margin-top:22px}
input,textarea,button{box-sizing:border-box;width:100%;font:20px Arial,sans-serif;border-radius:8px}input,textarea{padding:14px;margin-top:7px;border:2px solid #b9c6cf;background:#fff;color:#111}
textarea{min-height:130px;resize:vertical}#count{text-align:right;color:#c9d5dc;font-size:15px;margin-top:5px}button{margin-top:26px;padding:17px;border:0;background:#ffcf33;color:#111;font-weight:bold;cursor:pointer}
</style></head><body><main class="wrap"><h1>Emergency Message</h1><p>Enter your message below. It will be sent by radio to the emergency central node.</p>
<form method="POST" action="/send"><label for="name">Name (optional)</label><input id="name" name="name" maxlength="20" autocomplete="name">
<label for="message">Message</label><textarea id="message" name="message" required maxlength="64" aria-describedby="count"></textarea><div id="count">0/64</div>
<button type="submit">SEND MESSAGE</button></form></main><script>const m=document.getElementById('message'),c=document.getElementById('count');function u(){c.textContent=m.value.length+'/64'}m.addEventListener('input',u);u();</script></body></html>
)HTML";

String cleanInput(const String &input, size_t limit) {
  String result;
  result.reserve(limit);
  for (size_t i = 0; i < input.length() && result.length() < limit; ++i) {
    char c = input[i];
    // Drop NUL and ASCII control characters. Keep normal printable UTF-8 bytes.
    if (c != '\0' && (uint8_t)c >= 0x20 && c != 0x7F) result += c;
  }
  result.trim();
  // trim() can remove characters, never add them; length remains <= limit.
  return result;
}

void copyPacketText(char *destination, size_t destinationSize, const String &text) {
  memset(destination, 0, destinationSize);
  // Do not reserve a byte for NUL: fixed fields intentionally carry their full 20/64-byte capacity.
  memcpy(destination, text.c_str(), min(destinationSize, text.length()));
}

void sendPacket(const String &name, const String &message) {
  Packet packet = {};
  packet.senderId = SENDER_NODE_ID;
  packet.destId = 0;
  packet.msgId = nextMessageId++;
  copyPacketText(packet.senderName, sizeof(packet.senderName), name);
  copyPacketText(packet.message, sizeof(packet.message), message);

  LoRa.beginPacket();
  LoRa.write((const uint8_t *)&packet, sizeof(packet));
  int status = LoRa.endPacket();

  Serial.print(status ? "Sent: " : "LoRa send failed: ");
  Serial.print(name.length() ? name : "Anonymous");
  Serial.print(" - ");
  Serial.println(message);
}

void servePortal() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html", PORTAL_PAGE);
}

void sendConfirmation(const String &name, const String &message) {
  String page = F("<!doctype html><html><meta name=viewport content='width=device-width,initial-scale=1'><body style='margin:0;background:#101820;color:white;font:22px Arial;text-align:center;padding:48px'><h1 style='color:#ffda44'>Message sent</h1><p>Your emergency message was transmitted.</p><p style='font-size:16px'>You may now close this page or send another message.</p><p><a style='color:#ffda44' href='/'>Send another message</a></p></body></html>");
  server.send(200, "text/html", page);
}

void handleSend() {
  String name = cleanInput(server.arg("name"), MAX_NAME_CHARS);
  String message = cleanInput(server.arg("message"), MAX_MESSAGE_CHARS);
  if (message.length() == 0) {
    server.send(400, "text/html", "<h1>Message required</h1><p><a href='/'>Go back</a></p>");
    return;
  }
  sendPacket(name, message);
  sendConfirmation(name, message);
}

void redirectToPortal() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "Captive portal");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  // Calling softAP with one argument starts an open network; the define allows easy future security.
  if (strlen(AP_PASSWORD) == 0) WiFi.softAP(AP_SSID);
  else WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("Connect to "); Serial.print(AP_SSID);
  Serial.print(" then browse to http://"); Serial.println(WiFi.softAPIP());

  // Wildcard DNS makes every hostname (including OS probe hostnames) resolve to this ESP32.
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  // Each platform normally expects a successful Internet-test response. Redirecting instead
  // makes it recognize this AP as captive and opens its small portal browser.
  server.on("/generate_204", HTTP_ANY, redirectToPortal);       // Android
  server.on("/hotspot-detect.html", HTTP_ANY, redirectToPortal); // iOS / macOS
  server.on("/connecttest.txt", HTTP_ANY, redirectToPortal);     // Windows
  server.on("/", HTTP_GET, servePortal);
  server.on("/send", HTTP_POST, handleSend);
  // Other probe URLs and normal unknown URLs receive the form directly.
  server.onNotFound(servePortal);
  server.begin();

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("LoRa init failed. Check SX1278 wiring/power.");
    while (true) delay(1000);
  }
  Serial.println("LoRa sender ready.");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
}
