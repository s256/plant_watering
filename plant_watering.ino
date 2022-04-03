const int AirValue = 3700;   // replace the value from calibration in air
const int WaterValue = 1350; // replace the value from calibration in water
// default settings
const int daylightOffset_sec = 3600;
const long gmtOffset_sec = 3600;
const char *ntpServer = "pool.ntp.org";
// Firmware Version used for OTA
const long long FW_VERSION = 202204031337; // YEARmonthDAYhourMINUTE
// Sleep and Watering durations
const int uS_TO_S_FACTOR = 1000000; // Conversion factor for micro seconds to seconds
const int TIME_TO_SLEEP = 300;      // Time ESP32 will go to sleep (in seconds) (300 default)

const int WATERING_DURATION = 1000 * 60 * 1; // defaults to 1000 * 60 * 1 -> 1 minute

// Soil Moisture Limit
const int MOIST_LIMIT = 50;
// Data Batch size
const int DATA_TRANSFER_BATCH_SIZE = 2; // transfer after this number of items have been collected

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
#include "src/ota.h"

// ##############################
// Init Smart Home Stuff
// ##############################

// WiFi Client & FW
WiFiClient espClient;
String wifiMacString = String(WiFi.macAddress());

// InfluxDB Client Stuff
struct Measurement // Data structure
{
    float moisture; // Percentage of Moisture compared to calibration
    int pump;       // State of Pump (on / off)
    int waterlevel; // Water Level Sensor
    char location[20] = LOCATION;
    time_t time;
};

RTC_DATA_ATTR QList<Measurement> queue; // Queue
HTTPClient http;                        // HTTPClient user for transfering data to InfluxDB

// OTA ota;
OTA ota(OTA_SERVER, 80, FW_VERSION);

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
    ota.execOTA();
    Serial.println("OTA done...");
    WiFi.mode(WIFI_OFF);
}

void loop()
{
    Serial.println("Resuming...");
    digitalWrite(relay_pin, HIGH);
    long now = esp_log_timestamp();
    Serial.println("Watering System");
    // obtain measurements from the sensors
    soilMoistureValue = get_moisture(moisture_sensor);
    Serial.print("Soil Sensor: ");
    Serial.println(soilMoistureValue);
    Serial.println("");
    Serial.print("Water Level Sensor: ");
    water_level = digitalRead(water_sensor_pin_in);
    Serial.println(water_sensor_pin_in);
    soilmoisturepercent = map(soilMoistureValue, AirValue, WaterValue, 0, 100);
    struct Measurement m;
    m.moisture = soilmoisturepercent;
    m.waterlevel = water_level;
    time(&m.time);
    Serial.print("Soil Moisture: ");
    Serial.println(String(soilmoisturepercent) + " %");
    if ((soilmoisturepercent < MOIST_LIMIT) && (water_level == 1)) // if Soil is too dry, pump water for some duration
    {
        Serial.println("Soil is too dry, start watering");
        m.pump = 1;
        queue.push_back(m); // save the current values
        m.pump = 0;
        digitalWrite(relay_pin, LOW);
        delay(WATERING_DURATION);
    }
    m.pump = 0;
    soilMoistureValue = get_moisture(moisture_sensor);
    soilmoisturepercent = map(soilMoistureValue, AirValue, WaterValue, 0, 100);
    m.moisture = soilmoisturepercent;
    water_level = digitalRead(water_sensor_pin_in);
    m.waterlevel = water_level;
    time(&m.time);
    queue.push_back(m); // if the pump was not on, this is our only data point, if the pump was one, we can check for how long the pump was running
    Serial.print("Queue Size: ");
    Serial.println(queue.size());
    if (queue.size() % DATA_TRANSFER_BATCH_SIZE == 0)
    {
        transferData();
    }

    digitalWrite(relay_pin, HIGH);  // make sure pump is turned off, befor next cycle
    Serial.println("Sleeping...."); // next cycle should be some time in future, to give earth some time to consume water
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    delay(500);
    esp_deep_sleep_start();
}

// #################################
// Read Sensors
// #################################
int get_moisture(int sensor_pin)
{
    int analog_value = 0;
    analog_value = analogRead(sensor_pin);
    delay(200);
    analog_value = analogRead(sensor_pin); // read twice, test for better accuracy
    return analog_value;
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