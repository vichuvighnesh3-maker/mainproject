#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include "PortalInbox.h"

// Change this before deployment if a different network name is needed.
static const char *AP_SSID = "Emergency-Mesh";
static const byte DNS_PORT = 53;
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_GATEWAY(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);
static const uint8_t MAX_NAME_LENGTH = 20;
static const uint8_t MAX_MESSAGE_LENGTH = 64;
static const uint8_t INBOX_CAPACITY = 4;

DNSServer dnsServer;
WebServer webServer(80);

struct PendingMessage {
  String sender;
  String message;
};

static PendingMessage inbox[INBOX_CAPACITY];
static uint8_t inboxHead = 0;
static uint8_t inboxTail = 0;
static uint8_t inboxCount = 0;

// This page is intentionally self-contained: captive-portal browsers have no
// internet access and may support only a conservative subset of JavaScript.
static const char PORTAL_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Emergency Message</title><style>
body{margin:0;background:#10212d;color:#fff;font-family:Arial,sans-serif;font-size:18px}.card{max-width:520px;margin:0 auto;padding:24px 18px 36px}h1{font-size:30px;margin:0 0 8px}.note{line-height:1.45;margin:0 0 24px;color:#d8e7ef}label{display:block;font-weight:bold;margin:18px 0 7px}input,textarea,button{box-sizing:border-box;width:100%;font:inherit;border-radius:8px}input,textarea{padding:14px;border:2px solid #d8e7ef;background:#fff;color:#101820}textarea{min-height:132px;resize:vertical}.count{text-align:right;margin:6px 0 20px;color:#d8e7ef;font-size:15px}button{padding:16px;border:0;background:#ffca28;color:#171717;font-weight:bold;font-size:21px;min-height:56px}button:active{background:#ffb300}.small{font-size:14px;color:#d8e7ef;margin-top:18px;line-height:1.35}</style></head><body><main class="card"><h1>Send an emergency message</h1><p class="note">Your message will be sent through the local emergency network.</p><form action="/send" method="post" accept-charset="US-ASCII"><label for="sender">Your name (optional)</label><input id="sender" name="sender" type="text" maxlength="20" autocomplete="name" autocapitalize="words"><label for="message">Message</label><textarea id="message" name="message" maxlength="64" required aria-describedby="counter"></textarea><div class="count" id="counter">0/64</div><button type="submit">Send message</button></form><p class="small">No internet connection is required.</p></main><script>(function(){var m=document.getElementById('message'),c=document.getElementById('counter');function u(){c.innerHTML=m.value.length+'/64';}m.oninput=u;m.onkeyup=u;u();}());</script></body></html>
)rawliteral";

static String sanitizePrintableAscii(const String &input, size_t maxLength) {
  String cleaned;
  cleaned.reserve(maxLength);
  for (size_t i = 0; i < input.length() && cleaned.length() < maxLength; ++i) {
    const uint8_t character = static_cast<uint8_t>(input[i]);
    if (character >= 32 && character <= 126) {
      cleaned += static_cast<char>(character);
    }
  }
  cleaned.trim();
  return cleaned;
}

static bool enqueueMessage(const String &sender, const String &message) {
  if (inboxCount >= INBOX_CAPACITY) return false;
  inbox[inboxTail].sender = sender;
  inbox[inboxTail].message = message;
  inboxTail = (inboxTail + 1) % INBOX_CAPACITY;
  ++inboxCount;
  return true;
}

bool hasPendingMessage() {
  return inboxCount != 0;
}

uint8_t pendingMessageCount() {
  return inboxCount;
}

bool takePendingMessage(String &sender, String &message) {
  if (inboxCount == 0) return false;
  sender = inbox[inboxHead].sender;
  message = inbox[inboxHead].message;
  inbox[inboxHead].sender = "";
  inbox[inboxHead].message = "";
  inboxHead = (inboxHead + 1) % INBOX_CAPACITY;
  --inboxCount;
  return true;
}

static void sendPortal() {
  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  webServer.send_P(200, "text/html", PORTAL_PAGE);
}

static void redirectToPortal() {
  // A local redirect is deliberately different from each OS's expected online
  // probe response, causing its captive-portal UI to open.
  webServer.sendHeader("Location", "http://192.168.4.1/");
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(302, "text/plain", "Captive portal");
}

static void handleSend() {
  const String rawSender = webServer.hasArg("sender") ? webServer.arg("sender") : "";
  const String rawMessage = webServer.hasArg("message") ? webServer.arg("message") : "";
  const String sender = sanitizePrintableAscii(rawSender, MAX_NAME_LENGTH);
  const String message = sanitizePrintableAscii(rawMessage, MAX_MESSAGE_LENGTH);

  webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  if (message.length() == 0) {
    webServer.send(400, "text/html", "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><body style='font-family:Arial;padding:24px'><h1>Message not sent</h1><p>Please enter a message using printable characters.</p><p><a href='/'>Return to the form</a></p></body>");
    return;
  }
  if (!enqueueMessage(sender, message)) {
    webServer.send(503, "text/html", "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><body style='font-family:Arial;padding:24px'><h1>Message not sent</h1><p>The node is busy. Please retry shortly.</p><p><a href='/'>Return to the form</a></p></body>");
    return;
  }
  webServer.send(200, "text/html", "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><body style='font-family:Arial;padding:24px'><h1>Message sent</h1><p>Your message has been queued for the emergency network.</p><p><a href='/'>Send another message</a></p></body>");
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(AP_SSID);  // Open AP by design for emergency accessibility.

  dnsServer.start(DNS_PORT, "*", AP_IP);  // Resolve every hostname to this ESP32.

  webServer.on("/", HTTP_GET, sendPortal);
  webServer.on("/send", HTTP_POST, handleSend);

  // Known captive-network probe paths. Redirecting instead of returning their
  // expected healthy-internet responses requests the platform portal UI.
  webServer.on("/generate_204", HTTP_ANY, redirectToPortal);       // Android
  webServer.on("/hotspot-detect.html", HTTP_ANY, redirectToPortal); // iOS/macOS
  webServer.on("/connecttest.txt", HTTP_ANY, redirectToPortal);     // Windows
  webServer.onNotFound(redirectToPortal);
  webServer.begin();

  Serial.print("Captive portal IP: ");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();

  // Example radio-side use (place this logic in your radio module):
  // String sender, message;
  // if (takePendingMessage(sender, message)) radioSend(sender, message);
}
