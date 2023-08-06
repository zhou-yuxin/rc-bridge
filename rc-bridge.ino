#include "rc-bridge.hpp"

#define IS_SENDER   0

#if IS_SENDER
RCBridge::BasicSender role;
#else
RCBridge::BasicReceiver role;
#endif

void setup() {
    Serial.begin(115200);
    Serial.println();
    if(!LittleFS.begin()) {
        Serial.print("failed to initialize LittleFS...\n");
    }
    role.begin();
}

void loop() {
#if IS_SENDER
    static unsigned long last_time = 0;
    unsigned long now = micros();
    if(now - last_time >= 100000) {
        role.send(6, "hello");
        last_time = now;
    }
#endif
    role.loop();
}
