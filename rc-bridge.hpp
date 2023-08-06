#pragma once

#include <espnow.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

namespace RCBridge {

template <typename... T>
void debug(const char* format, T... args) {
    Serial.printf(format, args...);
}

class RCBridgeBase {

protected:
    // 默认名称（SSID）前缀
    static constexpr char* DEFAULT_NAME_PREFIX = "RCBridge-";
    // IP地址
    static constexpr char* IP_ADDR = "192.168.1.1";
    // 首页文件名、 配置文件名
    static constexpr char* FNAME_HTML = "index.html";
    static constexpr char* FNAME_JSON = "config.json";
    // 该文件存放6字节MAC地址+16字节随机密钥的对端信息
    static constexpr char* FPATH_PEER = "peer.info";
    // 最小、最大以及初始化信道
    static constexpr uint8_t MIN_CHANNEL = 1;
    static constexpr uint8_t MAX_CHANNEL = 13;
    static constexpr uint8_t INIT_CHANNEL = 7;
    // 发送端未配对时广播发送1字节的搜索命令
    static constexpr uint8_t CMD_SEARCH = 1;
    // 接收端收到搜索命令时的回复，格式：{RPL_SEARCH, <16字节的密钥>}，共1+16字节
    static constexpr uint8_t RPL_SEARCH = 2;
    // 发送端感到信号质量差，发送1字节的跳频命令
    static constexpr uint8_t CMD_HOP = 3;
    // 接收端回复跳频命令，格式：{RPL_HOP, <1字节的新信道>}，2字节
    static constexpr uint8_t RPL_HOP = 4;
    // 发送端单向推送数据帧，格式：{CMD_DATA，<字节数>，<数据>...}，共1+1+n字节
    static constexpr uint8_t CMD_DATA = 5;

protected:
    // HTML页面文件
    String fpath_html;
    // 存放配置的json文件
    String fpath_json;
    // 存放配置的json对象
    DynamicJsonDocument json;
    // Web服务以供配置
    ESP8266WebServer web;
    // 是否已配对
    bool matched;
    // 对端信息
    struct {
        // 对端MAC地址
        uint8_t addr[6];
        // 通信密钥
        uint8_t key[16];

        // 转化为<MAC = aa:bb:cc:..., key = aabbcc...>的人类可读形式
        String toString(bool only_addr = false) const {
            char buffer[64];
            char* dst = buffer;
            memcpy(dst, "MAC = ", 6);
            dst += 6;
            for(int i = 0; i < 6; i++) {
                if(i != 0) {
                    *(dst++) = ':';
                }
                sprintf(dst, "%02x", addr[i]);
                dst += 2;
            }
            if(!only_addr) {
                memcpy(dst, ", key = ", 8);
                dst += 8;
                for(int i = 0; i < 16; i++) {
                    sprintf(dst, "%02x", key[i]);
                    dst += 2;
                }
            }
            *dst = 0;
            return String(buffer);
        }
    } peer;

protected:
    RCBridgeBase(): json(8) {}

    bool begin(const char* dir) {
        fpath_html = dir;
        fpath_html.concat(FNAME_HTML);
        fpath_json = dir;
        fpath_json.concat(FNAME_JSON);
        File file = LittleFS.open(fpath_json, "r");
        if(!file) {
            debug("failed to open <%s> to read......\n", fpath_json.c_str());
            return false;
        }
        size_t fsize = file.size();
        json = DynamicJsonDocument(fsize * 2);
        auto err = deserializeJson(json, file);
        file.close();
        if(err) {
            debug("failed to parse <%s> as json...\n", fpath_json.c_str());
            return false;
        }
        debug("configuration loaded from <%s>...\n", fpath_json.c_str());
        // 配置文件中需要有name和password字段，否则使用默认值
        String name = json["name"];
        const char* password = json["password"];
        if(name.isEmpty()) {
            name = DEFAULT_NAME_PREFIX;
            name.concat(WiFi.softAPmacAddress());
        }
        if(!WiFi.mode(WIFI_AP)) {
            debug("failed to switch to AP mode...\n");
            return false;
        }
        if(!WiFi.softAP(name, password)) {
            debug("failed to setup WiFi access point...\n");
            return false;
        }
        debug("WiFi access point <%s> setup...\n", name.c_str());
        IPAddress ip;
        ip.fromString(IP_ADDR);
        if(!WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0))) {
            debug("failed to set IP to <%s>...\n", IP_ADDR);
            return false;
        }
        web.begin();
        web.onNotFound([&]() {
            String message("找不到页面（");
            message.concat(web.uri());
            message.concat(")");
            sendMessage(message.c_str());
        });
        web.on("/", [&]() {
            sendWebPage(fpath_html, json);
        });
        web.on("/reset", [&]() {
            if(reset()) {
                sendMessage("配对信息已删除，重启以重新配对...");
            }
            else {
                sendMessage("删除配对信息出错！");
            }
        });
        web.on("/update", [&]() {
            size_t pswd_len = web.arg("password").length();
            if(!(pswd_len == 0 || (8 <= pswd_len && pswd_len <= 16))) {
                sendMessage("密码要么为空，要么介于8-16位！");
                return;
            }
            if(!onConfigUpdating()) {
                return;
            }
            debug("configuration updated as:\n>>>\n");
            int narg = web.args();
            for(int i = 0; i < narg; i++) {
                const String& key = web.argName(i);
                const String& value = web.arg(i);
                debug("\t<%s> = <%s>\n", key.c_str(), value.c_str());
                json[key] = value;
            }
            debug("<<<\n");
            File file = LittleFS.open(fpath_json, "w");
            if(file) {
                serializeJson(json, file);
                file.close();
                sendMessage("配置已更新，重启以应用新配置...");
            }
            else {
                debug("failed to open <%s> to write...\n", fpath_json.c_str());
                sendMessage("保存配置出错！");
            }
        });
        debug("web service started on <%s:80>...\n", IP_ADDR);
        matched = false;
        json["peer.addr"] = "N/A";
        // espnow本质就是802.11的帧，所以设置wifi信道就是设置espnow的信道
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
        // espnow的回调函数无法带user defined argument，只能通过全局变量转入成员方法中，
        // 不过好在不管是发送端还是接收端都是全局单例的
        static RCBridgeBase* instance = this;
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
        // 如果有配对文件，直接读取
        if(LittleFS.exists(FPATH_PEER)) {
            File file = LittleFS.open(FPATH_PEER, "r");
            if(!file) {
                debug("failed to open <%s> to read...\n", FPATH_PEER);
                return false;
            }
            size_t nread = file.readBytes((char*)&peer, sizeof(peer));
            file.close();
            if(nread != sizeof(peer)) {
                debug("failed to read from <%s>...\n", FPATH_PEER);
                return false;
            }
            debug("peer <%s> loaded from <%s>...\n", peer.toString().c_str(), FPATH_PEER);
        }
        // 否则现场搜索对端，并将MAC地址保存入文件
        else {
            if(!searchForPeer()) {
                debug("failed to search for peer...\n");
                return false;
            }
            File file = LittleFS.open(FPATH_PEER, "w");
            if(!file) {
                debug("failed to open <%s> to write...\n", FPATH_PEER);
                return false;
            }
            size_t nwrite = file.write((uint8_t*)&peer, sizeof(peer));
            file.close();
            if(nwrite != sizeof(peer)) {
                debug("failed to write to <%s>...\n", FPATH_PEER);
                return false;
            }
            debug("peer <%s> saved to <%s>...\n", peer.toString().c_str(), FPATH_PEER);
        }
        if(esp_now_add_peer(peer.addr, ESP_NOW_ROLE_COMBO, 0, peer.key, sizeof(peer.key)) != 0) {
            debug("failed to add <%s> as esp-now combo...\n", peer.toString().c_str());
            return false;
        }
        matched = true;
        json["peer.addr"] = peer.toString(true);
        return true;
    }

public:
    // 删除已配对的信息，使得下次begin()会重新搜索配对
    bool reset() {
        if(LittleFS.exists(FPATH_PEER)) {
            if(!LittleFS.remove(FPATH_PEER)) {
                debug("failed to remove <%s>...\n", FPATH_PEER);
                return false;
            }
        }
        return true;
    }

protected:
    // 发送<fpath>指向的html文件，用json中的字段填充html中的${xxx}字段
    bool sendWebPage(const String& fpath, JsonDocument& json) {
        File file = LittleFS.open(fpath, "r");
        if(!file) {
            debug("failed to open <%s> to read...", fpath.c_str());
            web.send(500, "text/plain", "server internal error...");
            return false;
        }
        String content = file.readString();
        file.close();
        String key;
        for(auto kv: json.as<JsonObject>()) {
            key = "${";
            key.concat(kv.key().c_str());
            key.concat('}');
            const char* value = kv.value().as<const char*>();
            content.replace(key, value);
        }
        debug("page <%s> rendered:\n>>>\n%s\n<<<\n", fpath.c_str(), content.c_str());
        web.send(200, "text/html", content);
        return true;
    }

    // 套用message.html，显示简单消息
    bool sendMessage(const char* message) {
        size_t capacity = (7 + strlen(message)) * 2;
        if(capacity < 256) {
            StaticJsonDocument<256> json;
            json["message"] = message;
            return sendWebPage("message.html", json);
        }
        else {
            DynamicJsonDocument json(capacity);
            json["message"] = message;
            return sendWebPage("message.html", json);
        }
    }

protected:
    virtual bool searchForPeer() = 0;

    virtual void onSent(uint8_t* addr, uint8_t status) = 0;

    virtual void onReceived(uint8_t* addr, uint8_t* data, uint8_t len) = 0;

    // 用户可重载该方法以监听访问/update的事件，比如可以检查web传来的参数，
    // 返回false可以中断配置生效
    virtual bool onConfigUpdating() {
        return true;
    }

};

// 最小发送端，支持Web配置、发现设备、加密发送数据、自动跳频
class BasicSender: public RCBridgeBase {

protected:
    // 指数平滑移动均值中新值的权重
    static constexpr float quality_weight = 0.01f;
    // 信号质量降至此阈值触发跳频命令
    static constexpr float hop_threshold = 0.75f;

protected:
    // 当前信号质量
    float radio_quality;

public:
    bool begin() {
        radio_quality = 1.0f;
        if(!RCBridgeBase::begin("sender/")) {
            return false;
        }
        debug("basic sender initialized...\n");
        return true;
    }

    bool send(uint8_t len, const void* data) {
        // espnow一次最多发送250字节，去除开头一字节的CMD_DATA，用户数据最大249字节
        if(len > 249) {
            debug("data more than 249 bytes...\n");
            return false;
        }
        uint8_t command[250];
        command[0] = CMD_DATA;
        memcpy(command + 1, data, len);
        if(esp_now_send(peer.addr, command, len + 1) != 0) {
            debug("failed to send data...\n");
            return false;
        }
        return true;
    }

    void loop() {
        web.handleClient();
    }

protected:
    virtual bool searchForPeer() override {
        const char* broadcast = "\xff\xff\xff\xff\xff\xff";
        uint8_t command = CMD_SEARCH;
        unsigned long last_time = 0;
        // 广播发送直到配对
        while(!matched) {
            unsigned long now = micros();
            // 每500ms发送一次
            if(now - last_time >= 500000) {
                debug("searching for receiver...\n");
                if(esp_now_send((uint8_t*)broadcast, &command, 1) != 0) {
                    debug("failed to broadcast beacon...\n");
                    return false;
                }
                last_time = now;
            }
            // 不要让web服务停止响应
            web.handleClient();
        }
        return true;
    }

    virtual void onReceived(uint8_t* addr, uint8_t* data, uint8_t len) override {
        if(!matched) {
            // 收到搜索回复，格式：{RPL_SEARCH, <密钥>}
            if(len == 1 + sizeof(peer.key) && data[0] == RPL_SEARCH) {
                memcpy(peer.addr, addr, 6);
                memcpy(peer.key, data + 1, sizeof(peer.key));
                matched = true;
                debug("receiver <%s> matched...\n", peer.toString().c_str());
            }
        }
        else {
            // 收到跳频回复，格式：{RPL_HOP, <新信道>}
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
            // 未配对时发送的是广播，广播包的onSent()仅告知是否发送成功，
            // 而不反馈是否有设备收到且确认（因为广播没有明确的接收者）
            if(status != 0) {
                debug("failed to broadcast beacon...\n");
            }
        }
        else {
            // 用指数平滑均值法计算当前加权的信号质量，即avg=(1-w)*avg + w*X
            static constexpr float cw = 1.0f - quality_weight;
#ifndef SIMULATE_LOW_RADIO_QUALITY
            // status == 0代表帧被对端接收，记为1，否则记为0
            radio_quality = radio_quality * cw + (status == 0) * quality_weight;
#else
            // 在调试阶段，可使用此代码模拟信号质量只有50%，以触发跳频逻辑
            radio_quality = radio_quality * cw + 0.5f * quality_weight;
#endif
            if(radio_quality < hop_threshold) {
                debug("channel hopping triggered...\n");
                // 用户可继承后实现hook
                onLowRadioQuality();
                uint8_t command = CMD_HOP;
                if(esp_now_send(peer.addr, &command, 1) == 0) {
                    // 如果不重置radio_quality，那么很可能连续发送多个跳频命令
                    radio_quality = 1.0f;
                }
                else {
                    debug("failed to send hop command...\n");
                }
            }
        }
    }

protected:
    // 用户可重载该方法以监听信号差的事件，比如拉响蜂鸣器让用户注意遥控距离
    virtual void onLowRadioQuality() {}

};

// 最小发送端，支持Web配置、发现设备、加密接收数据、自动跳频
class BasicReceiver: public RCBridgeBase {

protected:
    // 当前信道
    uint8_t channel;
    // 跳频方向（1表示每次信道+1，-1表示每次信道-1）
    int8_t channel_direction;
    // 即将跳到的信道
    uint8_t new_channel;

public:
    bool begin() {
        channel = INIT_CHANNEL;
        channel_direction = 1;
        if(!RCBridgeBase::begin("receiver/")) {
            return false;
        }
        debug("basic receiver initialized...\n");
        return true;
    }

    void loop() {
        web.handleClient();
    }

protected:
    virtual bool searchForPeer() override {
        debug("waiting for sender...\n");
        while(!matched) {
            // 接收端被动监听广播直到配对，无需做事
            // 不要让web服务停止响应
            web.handleClient();
        }
        return true;
    }

    virtual void onReceived(uint8_t* addr, uint8_t* data, uint8_t len) override {
        if(!matched) {
            // 收到配对广播
            if(len == 1 && data[0] == CMD_SEARCH) {
                debug("received beacon from %s...\n", peer.toString(true).c_str());
                memcpy(peer.addr, addr, 6);
                // 产生随机密钥
                randomSeed(micros());
                for(size_t i = 0; i < sizeof(peer.key); i++) {
                    peer.key[i] = (uint8_t)random(0, 256);
                }
                uint8_t reply[1 + sizeof(peer.key)];
                reply[0] = RPL_SEARCH;
                memcpy(reply + 1, peer.key, sizeof(peer.key));
                if(esp_now_send(addr, reply, sizeof(reply)) != 0) {
                    debug("failed to reply beacon...\n");
                }
            }
        }
        else {
            // 收到跳频命令
            if(len == 1 && data[0] == CMD_HOP) {
                debug("received hop command...\n");
                new_channel = channel + channel_direction;
                // 如果超出MAX_CHANNEL（即本来已经是MAX_CHANNEL），则调头降一级
                if(new_channel > MAX_CHANNEL) {
                    new_channel = MAX_CHANNEL - 1;
                }
                // 如果低于MIN_CHANNEL（即本来已经是MIN_CHANNEL），则调头升一级
                else if(new_channel < MIN_CHANNEL) {
                    new_channel = MIN_CHANNEL + 1;
                }
                uint8_t reply[2] = {RPL_HOP, new_channel};
                if(esp_now_send(peer.addr, reply, 2) != 0) {
                    debug("failed to reply hop...\n");
                }
            }
            // 收到数据帧
            else if(len >= 1 && data[0] == CMD_DATA) {
                onData(len - 1, data + 1);
            }
        }
    }

    virtual void onSent(uint8_t* addr, uint8_t status) {
        if(!matched) {
            if(status == 0) {
                // 发送的搜索回复被接收，配对成功
                matched = true;
            }
        }
        else {
            if(status == 0) {
                // 发送的跳频回复被接收，执行跳频
                if(wifi_set_channel(new_channel)) {
                    debug("channel set to %d...\n", new_channel);
                    channel_direction = new_channel - channel;
                    channel = new_channel;
                }
                else {
                    debug("failed to set channel to %d...\n", new_channel);
                }
            }
        }
    }

protected:
    // 用户可重载以接收数据
    virtual void onData(uint8_t len, void* data) {
        debug("data received, len = %d, data = [", len);
        for(uint8_t i = 0; i < len; i++) {
            debug("%02x", ((uint8_t*)data)[i]);
        }
        debug("]...\n");
    }

};

}
