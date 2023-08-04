#pragma once

#include <sbus.h>
#include <espnow.h>
#include <LittleFS.h>
#include <ESP8266WiFi.h>

class RCBridgeBase {

protected:
    static constexpr char* FPATH_ADDR = "peer.mac";
    static constexpr uint8_t MIN_CHANNEL = 1;
    static constexpr uint8_t MAX_CHANNEL = 13;
    static constexpr uint8_t INIT_CHANNEL = 7;
    static constexpr uint8_t CMD_SEARCH = 1;
    static constexpr uint8_t RPL_SEARCH = 2;
    static constexpr uint8_t CMD_HOP = 3;
    static constexpr uint8_t RPL_HOP = 4;
    static constexpr uint8_t CMD_SBUS = 5;

protected:
    template <typename... T>
    static void debug(const char* format, T... args) {
        Serial.printf(format, args...);
    }

    static char* getReadableMAC(const uint8_t* addr, char* readable) {
        sprintf(readable, "%02x:%02x:%02x:%02x:%02x:%02x",
            addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        return readable;
    }

protected:
    bool matched;
    uint8_t peer_addr[6];

protected:
    RCBridgeBase() {}

    bool begin() {
        static RCBridgeBase* instance = this;
        matched = false;
        WiFi.persistent(false);
        if(!WiFi.mode(WIFI_AP)) {
            debug("failed to switch to AP mode...\n");
            return false;
        }
        if(!wifi_set_channel(INIT_CHANNEL)) {
            debug("failed to set channel to %d...\n", INIT_CHANNEL);
            return false;
        }
        if(esp_now_init() != 0) {
            debug("failed to initialize esp-now...\n");
            return false;
        }
        if(esp_now_set_self_role(ESP_NOW_ROLE_COMBO) != 0) {
            debug("failed to set esp-now role as combo...\n");
            return false;
        }
        int ret = esp_now_register_send_cb([](uint8_t* addr, uint8_t status) {
            instance->onSent(addr, status);
        });
        if(ret != 0) {
            debug("failed to register send callback...\n");
            return false;
        }
        ret = esp_now_register_recv_cb([](uint8_t* addr, uint8_t* data, uint8_t len) {
            instance->onReceived(addr, data, len);
        });
        if(ret != 0) {
            debug("failed to register receive callback...\n");
            return false;
        }
        char readable[32];
        if(LittleFS.exists(FPATH_ADDR)) {
            File file = LittleFS.open(FPATH_ADDR, "r");
            if(!file) {
                debug("failed to open <%s> to read...\n", FPATH_ADDR);
                return false;
            }
            size_t nread = file.readBytes((char*)peer_addr, 6);
            file.close();
            if(nread != 6) {
                debug("failed to read MAC address from <%s>...\n", FPATH_ADDR);
                return false;
            }
            debug("peer address <%s> loaded from <%s>...\n",
                getReadableMAC(peer_addr, readable), FPATH_ADDR);
        }
        else {
            if(!searchForPeer()) {
                debug("failed to search for peer...\n");
                return false;
            }
            File file = LittleFS.open(FPATH_ADDR, "w");
            if(!file) {
                debug("failed to open <%s> to write...\n", FPATH_ADDR);
                return false;
            }
            size_t nwrite = file.write(peer_addr, 6);
            file.close();
            if(nwrite != 6) {
                debug("failed to write MAC address to <%s>...\n", FPATH_ADDR);
                return false;
            }
            debug("peer address <%s> saved to <%s>...\n",
                getReadableMAC(peer_addr, readable), FPATH_ADDR);
        }
        if(esp_now_add_peer(peer_addr, ESP_NOW_ROLE_COMBO, 0, nullptr, 0) != 0) {
            Serial.printf("failed to add <%s> as esp-now combo...\n",
                getReadableMAC(peer_addr, readable));
            return false;
        }
        matched = true;
        return true;
    }

public:
    bool reset() {
        if(LittleFS.exists(FPATH_ADDR)) {
            if(!LittleFS.remove(FPATH_ADDR)) {
                debug("failed to remove <%s>...\n", FPATH_ADDR);
                return false;
            }
        }
        return true;
    }

protected:
    virtual bool searchForPeer() = 0;

    virtual void onSent(uint8_t* addr, uint8_t status) {};

    virtual void onReceived(uint8_t* addr, uint8_t* data, uint8_t len) {};

};

class Sender: public RCBridgeBase {

protected:
    static constexpr float quality_weight = 0.01f;
    static constexpr float hop_threshold = 0.75f;

protected:
    float radio_quality;
    bfs::SbusRx sbus;

public:
    Sender(HardwareSerial* sbus_serial): sbus(sbus_serial) {}

    bool begin() {
        radio_quality = 1.0f;
        sbus.Begin();
        if(!RCBridgeBase::begin()) {
            return false;
        }
        debug("sender initialized...\n");
        return true;
    }

    void loop() {
        if(sbus.Read()) {
            bfs::SbusData frame = sbus.data();
            if(matched) {
                uint8_t command[1 + sizeof(frame)];
                command[0] = CMD_SBUS;
                memcpy(command + 1, &frame, sizeof(frame));
                if(esp_now_send(peer_addr, command, sizeof(command)) != 0) {
                    debug("failed to send sbus frame...\n");
                }
            }
        }
    }

protected:
    virtual bool searchForPeer() override {
        const char* broadcast = "\xff\xff\xff\xff\xff\xff";
        debug("searching for receiver...\n");
        while(!matched) {
            uint8_t command = CMD_SEARCH;
            if(esp_now_send((uint8_t*)broadcast, &command, 1) != 0) {
                debug("failed to broadcast beacom...\n");
                return false;
            }
            delay(500);
        }
        return true;
    }

    virtual void onReceived(uint8_t* addr, uint8_t* data, uint8_t len) override {
        if(!matched) {
            if(len == 1 && data[0] == RPL_SEARCH) {
                memcpy(peer_addr, addr, 6);
                matched = true;
                char readable[32];
                debug("receiver <%s> found...\n", getReadableMAC(addr, readable));
            }
        }
        else {
            if(len == 2 && data[0] == RPL_HOP) {
                uint8_t channel = data[1];
                if(wifi_set_channel(channel)) {
                    debug("channel hopped to %d...\n", channel);
                }
                else {
                    debug("failed to set channel to %d...\n", channel);
                }
            }
        }
    }

    virtual void onSent(uint8_t* addr, uint8_t status) {
        if(!matched) {
            if(status != 0) {
                debug("failed to broadcast beacom...\n");
            }
        }
        else {
            static constexpr float cw = 1 - quality_weight;
#ifndef SIMULATE_LOW_RADIO_QUALITY
            radio_quality = radio_quality * cw + (status == 0) * quality_weight;
#else
            radio_quality = radio_quality * cw + 0.5f * quality_weight;
#endif
            if(radio_quality < hop_threshold) {
                onLowRadioQuality();
                uint8_t command = CMD_HOP;
                if(esp_now_send(peer_addr, &command, 1) == 0) {
                    radio_quality = 1.0f;
                }
                else {
                    debug("failed to send hop command...\n");
                }
            }
        }
    }

protected:
    virtual void onLowRadioQuality() {}

};

class Receiver: public RCBridgeBase {

protected:
    struct ChannelGenerator {
        int8_t channel;
        int8_t direction;

        void begin() {
            channel = INIT_CHANNEL;
            direction = 1;
        }

        uint8_t next(bool commit) {
            int8_t new_channel = channel + direction;
            if(new_channel > MAX_CHANNEL) {
                new_channel = MAX_CHANNEL - 1;
            }
            else if(new_channel < MIN_CHANNEL) {
                new_channel = MIN_CHANNEL + 1;
            }
            if(commit) {
                direction = new_channel - channel;
                channel = new_channel;
            }
            return new_channel;
        }

    } channel;

public:
    Receiver() {}

    bool begin() {
        channel.begin();
        if(!RCBridgeBase::begin()) {
            return false;
        }
        debug("receiver initialized...\n");
        return true;
    }

    void loop() {}

protected:
    virtual bool searchForPeer() override {
        debug("searching for sender...\n");
        while(!matched) {
            delay(100);
        }
        return true;
    }

    virtual void onReceived(uint8_t* addr, uint8_t* data, uint8_t len) override {
        if(!matched) {
            if(len == 1 && data[0] == CMD_SEARCH) {
                char readable[32];
                debug("received beacon from %s\n", getReadableMAC(addr, readable));
                uint8_t reply = RPL_SEARCH;
                if(esp_now_send(addr, &reply, 1) != 0) {
                    debug("failed to reply beacon...\n");
                }
                else {
                    memcpy(peer_addr, addr, 6);
                    matched = true;
                }
            }
        }
        else {
            if(len == 1 && data[0] == CMD_HOP) {
                debug("received hop command...\n");
                uint8_t reply[2] = {RPL_HOP, channel.next(false)};
                if(esp_now_send(peer_addr, reply, 2) != 0) {
                    debug("failed to reply hop...\n");
                }
            }
            else if(len == 1 + sizeof(bfs::SbusData) && data[0] == CMD_SBUS) {
                bfs::SbusData frame;
                memcpy(&frame, data + 1, sizeof(bfs::SbusData));
                onSbusFrame(frame);
            }
        }
    }

    virtual void onSent(uint8_t* addr, uint8_t status) {
        if(matched) {
            if(status == 0) {
                uint8_t next_channel = channel.next(false);
                if(wifi_set_channel(next_channel)) {
                    channel.next(true);
                    debug("channel set to %d...\n", next_channel);
                }
                else {
                    debug("failed to set channel to %d...\n", next_channel);
                }
            }
        }
    }

protected:
    virtual void onSbusFrame(bfs::SbusData& frame) {}

};
