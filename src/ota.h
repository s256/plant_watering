// ####################################################
// OTA
// ####################################################
#ifndef OTA_H
#define OTA_H

#include <Arduino.h>
#include <string>
#include <WiFi.h>


class OTA {
    private:
        WiFiClient _Client;
        String _MAC;
        String _host;
        int _port;
        long long _FW_VERSION;
    public:
        OTA(String host, int port, long long firmware);
        void execOTA();
};


#endif