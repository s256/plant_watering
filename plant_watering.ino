const int AirValue = 3700;   // replace the value from calibration in air
const int WaterValue = 1350; // replace the value from calibration in water
// default settings
const int daylightOffset_sec = 3600;
const long gmtOffset_sec = 3600;
const char *ntpServer = "pool.ntp.org";
// Firmware Version used for OTA
const long long FW_VERSION = 202203081838; // YEARmonthDAYhourMINUTE
// Sleep and Watering durations
const int uS_TO_S_FACTOR = 1000000; // Conversion factor for micro seconds to seconds
const int TIME_TO_SLEEP = 30;       // Time ESP32 will go to sleep (in seconds) (300 default)

const int WATERING_DURATION = 1000 * 10; // defaults to 1000 * 60 * 1 -> 1 minute

// Data Batch size
const int DATA_TRANSFER_BATCH_SIZE = 1; // transfer after this number of items have been collected

RTC_DATA_ATTR int bootCount = 0;

int relay_pin = 16;
int water_sensor_pin_in = 27;
int moisture_sensor = 35;
int soilMoistureValue = 0;
int soilmoisturepercent = 0;
int water_level = 0;

#include <QList.h>
#include "custom.h"
#include <WiFi.h>
#include "time.h"
#include <HTTPClient.h>
#include <esp_sleep.h>
#include <esp_log.h> // support for timestamps
#include <string>
#include <Update.h>
using namespace std;
#include <string>
#include <sstream>

// ##############################
// Init Smart Home Stuff
// ##############################

// WiFi Client & FW
WiFiClient espClient;
String wifiMacString = String(WiFi.macAddress());

// InfluxDB Client Stuff
RTC_DATA_ATTR struct Measurement // Data structure
{
    float moisture; // Percentage of Moisture compared to calibration
    int pump;       // State of Pump (on / off)
    int waterlevel; // Water Level Sensor
    char location[20] = LOCATION;
    time_t time;
};

RTC_DATA_ATTR QList<Measurement> queue; // Queue
HTTPClient http;                        // HTTPClient user for transfering data to InfluxDB

// OTA Stuff

long contentLength = 0;
long long firmware = 0;
bool isValidContentType = false;
bool isNewFirmware = false;

String host = OTA_SERVER; // Host => OTA Server Path
int port = 80;            // Non https. For HTTPS 443. As of today, HTTPS doesn't work.

// Utility to extract header value from headers
String getHeaderValue(String header, String headerName)
{
    return header.substring(strlen(headerName.c_str()));
}

void setup()
{
    Serial.begin(115200);
    ++bootCount;
    Serial.println("Boot number: " + String(bootCount));

    // Print the wakeup reason for ESP32
    print_wakeup_reason();

    // disable bluetooth to save power
    btStop();

    pinMode(relay_pin, OUTPUT);
    pinMode(water_sensor_pin_in, INPUT);
    pinMode(moisture_sensor, INPUT);

    digitalWrite(relay_pin, HIGH);
    // Connecting WiFi and syncing Time

    Serial.println("Connecting to Wifi...");
    while (!connectWifi())
        ;
    Serial.println("Obtaining time from time server...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    while (!getLocalTime(&timeinfo))
    {
        Serial.println("ERROR: Failed to obtain time!");
        delay(500);
    }
    Serial.println("Done...");
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");

    // Execute OTA Update
    execOTA();
    Serial.println("OTA done...");
    WiFi.mode(WIFI_OFF);
}

void loop()
{
    Serial.println("Resuming...");
    digitalWrite(relay_pin, HIGH);
    long now = esp_log_timestamp();
    Serial.println("Watering System");
    soilMoistureValue = analogRead(moisture_sensor);
    delay(500);
    soilMoistureValue = analogRead(moisture_sensor); // read twice, test for better accuracy
    Serial.print("Soil Sensor: ");
    Serial.println(soilMoistureValue);
    Serial.println("");
    Serial.print("Water Level Sensor: ");
    Serial.println(digitalRead(water_sensor_pin_in));
    soilmoisturepercent = map(soilMoistureValue, AirValue, WaterValue, 0, 100);
    water_level = digitalRead(water_sensor_pin_in);
    struct Measurement m;
    m.moisture = soilmoisturepercent;
    m.waterlevel = water_level;
    time(&m.time);
    Serial.print("Soil Moisture: ");
    Serial.println(soilmoisturepercent + " %");
    if ((soilmoisturepercent < 35) && (water_level == 1)) // if Soil is too dry, pump water for some duration
    {
        Serial.println("Soil is too dry, start watering");
        digitalWrite(relay_pin, LOW);
        delay(WATERING_DURATION);
        m.pump = 1;
    }
    else
    {
        m.pump = 0;
    }
    // obtain measurements from the sensor

    queue.push_back(m);
    Serial.print("Queue Size: ");
    Serial.println(queue.size());
    if (queue.size() % DATA_TRANSFER_BATCH_SIZE == 0)
    {
        transferData();
    }

    digitalWrite(relay_pin, HIGH);  // make sure pump is turned off, befor next cycle
    Serial.println("Sleeping...."); // next cycle should be some time in future, to give earth some time to consume water
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
}

/**
 * Connects to Wifi and returns true if a connection has
 * been successfully established.
 *
 * Returns:
 *   `true` if the connection succeeds `false` otherwise.
 */
bool connectWifi()
{
    int count = 1;
    WiFi.mode(WIFI_STA);
    Serial.println("WiFi Station Mode");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED && count < WIFI_RECONNECT_TRY_MAX)
    {
        delay(WIFI_RECONNECT_DELAY_MS);
        count++;
    }
    Serial.println("");
    Serial.println("WiFi connected..!");
    Serial.print("Got IP: ");
    Serial.println(WiFi.localIP());
    return WiFi.status() == WL_CONNECTED;
}

/*
Method to print the reason by which ESP32
has been awaken from sleep
*/
void print_wakeup_reason()
{
    esp_sleep_wakeup_cause_t wakeup_reason;

    wakeup_reason = esp_sleep_get_wakeup_cause();

    switch (wakeup_reason)
    {
    case ESP_SLEEP_WAKEUP_EXT0:
        Serial.println("Wakeup caused by external signal using RTC_IO");
        break;
    case ESP_SLEEP_WAKEUP_EXT1:
        Serial.println("Wakeup caused by external signal using RTC_CNTL");
        break;
    case ESP_SLEEP_WAKEUP_TIMER:
        Serial.println("Wakeup caused by timer");
        break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
        Serial.println("Wakeup caused by touchpad");
        break;
    case ESP_SLEEP_WAKEUP_ULP:
        Serial.println("Wakeup caused by ULP program");
        break;
    default:
        Serial.printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason);
        break;
    }
}

// ##############################
// OTA Logic
// ##############################

void execOTA()
{
    Serial.println("Connecting to: " + String(host));
    wifiMacString.replace(":", "");
    String bin = "/ota/esp32_" + wifiMacString + ".ino.bin"; // bin file name with a slash in front.
    // Connect to S3
    if (espClient.connect(host.c_str(), port))
    {
        // Connection Succeed.
        // Fecthing the bin
        Serial.println("Fetching Bin: " + String(bin));

        // Get the contents of the bin file
        espClient.print(String("GET ") + bin + " HTTP/1.1\r\n" +
                        "Host: " + host + "\r\n" +
                        "Cache-Control: no-cache\r\n" +
                        "Connection: close\r\n\r\n");

        // Check what is being sent
        //    Serial.print(String("GET ") + bin + " HTTP/1.1\r\n" +
        //                 "Host: " + host + "\r\n" +
        //                 "Cache-Control: no-cache\r\n" +
        //                 "Connection: close\r\n\r\n");

        unsigned long timeout = millis();
        while (espClient.available() == 0)
        {
            if (millis() - timeout > 5000)
            {
                Serial.println("Client Timeout !");
                espClient.stop();
                return;
            }
        }
        // Once the response is available,
        // check stuff

        /*
           Response Structure
            HTTP/1.1 200 OK
            x-amz-id-2: NVKxnU1aIQMmpGKhSwpCBh8y2JPbak18QLIfE+OiUDOos+7UftZKjtCFqrwsGOZRN5Zee0jpTd0=
            x-amz-request-id: 2D56B47560B764EC
            Date: Wed, 14 Jun 2017 03:33:59 GMT
            Last-Modified: Fri, 02 Jun 2017 14:50:11 GMT
            ETag: "d2afebbaaebc38cd669ce36727152af9"
            Accept-Ranges: bytes
            Content-Type: application/octet-stream
            Content-Length: 357280
            Server: AmazonS3

            {{BIN FILE CONTENTS}}

        */
        while (espClient.available())
        {
            // read line till /n
            String line = espClient.readStringUntil('\n');
            // remove space, to check if the line is end of headers
            line.trim();

            // if the the line is empty,
            // this is end of headers
            // break the while and feed the
            // remaining `client` to the
            // Update.writeStream();
            if (!line.length())
            {
                // headers ended
                break; // and get the OTA started
            }

            // Check if the HTTP Response is 200
            // else break and Exit Update
            if (line.startsWith("HTTP/1.1"))
            {
                if (line.indexOf("200") < 0)
                {
                    Serial.println("Got a non 200 status code from server. Exiting OTA Update.");
                    break;
                }
            }

            // extract headers here
            // Start with content length
            if (line.startsWith("Content-Length: "))
            {
                contentLength = atol((getHeaderValue(line, "Content-Length: ")).c_str());
                Serial.println("Got " + String(contentLength) + " bytes from server");
            }

            // Next, the content type
            if (line.startsWith("Content-Type: "))
            {
                String contentType = getHeaderValue(line, "Content-Type: ");
                Serial.println("Got " + contentType + " payload.");
                if (contentType == "application/octet-stream")
                {
                    isValidContentType = true;
                }
            }

            // Next, the firmware version
            if (line.startsWith("X-Firmware: "))
            {
                firmware = atoll((getHeaderValue(line, "X-Firmware: ")).c_str());
                Serial.print("Got ");
                Serial.print(firmware);
                Serial.print(" version. Currently running: ");
                Serial.print(FW_VERSION);
                Serial.println();
                if (firmware > FW_VERSION)
                {
                    Serial.println("Firmware is new. Starting Update...");
                    isNewFirmware = true;
                }
                else
                {
                    Serial.println("Firmware is not new, skipping Update.");
                }
            }
        }
    }
    else
    {
        // Connect to S3 failed
        // May be try?
        // Probably a choppy network?
        Serial.println("Connection to " + String(host) + " failed. Please check your setup");
        // retry??
        // execOTA();
    }

    // Check what is the contentLength and if content type is `application/octet-stream`
    Serial.println("contentLength : " + String(contentLength) + ", isValidContentType : " + String(isValidContentType) + ", isNewFirmware : " + String(isNewFirmware));

    // check contentLength and content type
    if (contentLength && isValidContentType && isNewFirmware)
    {
        // Check if there is enough to OTA Update
        bool canBegin = Update.begin(contentLength);

        // If yes, begin
        if (canBegin)
        {
            Serial.println("Begin OTA. This may take 2 - 5 mins to complete. Things might be quite for a while.. Patience!");
            // No activity would appear on the Serial monitor
            // So be patient. This may take 2 - 5mins to complete
            size_t written = Update.writeStream(espClient);

            if (written == contentLength)
            {
                Serial.println("Written : " + String(written) + " successfully");
            }
            else
            {
                Serial.println("Written only : " + String(written) + "/" + String(contentLength) + ". Retry?");
                // retry??
                // execOTA();
            }

            if (Update.end())
            {
                Serial.println("OTA done!");
                if (Update.isFinished())
                {
                    Serial.println("Update successfully completed. Rebooting.");
                    ESP.restart();
                }
                else
                {
                    Serial.println("Update not finished? Something went wrong!");
                }
            }
            else
            {
                Serial.println("Error Occurred. Error #: " + String(Update.getError()));
            }
        }
        else
        {
            // not enough space to begin OTA
            // Understand the partitions and
            // space availability
            Serial.println("Not enough space to begin OTA");
            espClient.flush();
        }
    }
    else
    {
        Serial.println("There was no content in the response or Firmware is up to date");
        espClient.flush();
    }
}

/**
 * Transfers the data to the influx db server.
 */
void transferData()
{
    // Connect to Wifi
    if (!connectWifi())
    {
        return;
    }

    // Transfer data
    Serial.println("Transfering data to InfluxDB");
    while (queue.size() > 0 && transferBatch())
    {
        Serial.print(".");
    }
    Serial.println("\nCompleted :)");
    // disable Wifi

    delay(500);
    WiFi.mode(WIFI_OFF);
    delay(500);
}

/**
 * Transfer a batch of size DATA_TRANSFER_BATCH_SIZE to InfluxDB.
 * This is necessary since the httpClient does not allow large posts.
 *
 * Returns:
 *  `true` if the transfer succeeds `else` otherwise.
 */
boolean transferBatch()
{
    int endIndex = queue.size() - 1;
    if (endIndex < 0)
    {
        return false;
    }
    int startIndex = endIndex - DATA_TRANSFER_BATCH_SIZE;
    if (startIndex < 0)
    {
        startIndex = 0;
    }

    http.begin(INFLUXDB_REST_SERVICE_URL);
    http.addHeader("Content-type", "text/plain");
    std::ostringstream oss;
    for (int i = endIndex; i >= startIndex; i--)
    {
        struct Measurement m = queue[i];
        oss << "plants,sensor=moisture,host=" << WiFi.macAddress().c_str() << ",location=" << m.location << " moisture=" << m.moisture << ",pump=" << m.pump
            << ",water_level=" << m.waterlevel << " " << m.time << "000000000\n"; // we need to add 9 zeros to the time, since InfluxDB expects ns.
    }
    int httpResponseCode = http.POST(oss.str().c_str());
    http.end();
    if (httpResponseCode != 204)
    {
        Serial.println("Error on sending POST");
        Serial.print(httpResponseCode);
        return false;
    }

    // clear the transferred items from the queue
    for (int i = endIndex; i >= startIndex; i--)
    {
        queue.clear(i);
    }
    return true;
}