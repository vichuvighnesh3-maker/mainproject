#pragma once

#include <Arduino.h>

// Include this header from radio.cpp. Calling takePendingMessage() transfers
// ownership of the oldest queued submission to the caller.
bool hasPendingMessage();
uint8_t pendingMessageCount();
bool takePendingMessage(String &sender, String &message);
