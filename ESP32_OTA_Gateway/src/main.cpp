x/*
 * ESP32 WiFi + BLE OTA Gateway - part of the STM32F103RBT6 three-tier secure bootloader.
 * Author: Adham Ehab   Date: 18/08/2026
 *
 * A transparent bridge between a host (over the ESP32's own WiFi access point OR
 * over Bluetooth LE) and the UART that talks to the STM32 bootloader. The PC tool
 * speaks the exact same framed protocol it uses over a COM port - the ESP32 just
 * moves the bytes - so nothing on the STM32 side changes, encrypted images included:
 *
 *   PC --WiFi--> ESP32 (AP "STM32-OTA-Gateway", TCP :3333) --UART--> STM32 USART1
 *   PC --BLE---> ESP32 (Nordic UART Service, "STM32-OTA-BLE") --UART--> STM32 USART1
 *
 *   python bl_host.py tcp:192.168.4.1:3333 flash app.bin
 *   python bl_host.py ble:STM32-OTA-BLE     flash app.bin
 *
 * Wiring (all 3.3 V, common ground):
 *   ESP32 GPIO17 (TX2) --> STM32 PA10 (USART1 RX)
 *   ESP32 GPIO16 (RX2) <-- STM32 PA9  (USART1 TX)
 *   ESP32 GND         <--> STM32 GND
 *
 * USART1 is used (not the ST-Link's USART2), so the PC's COM3 stays free. The
 * STM32 must be in bootloader mode (hold B1, tap reset) to answer.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>

static const char    *AP_SSID    = "STM32-OTA-Gateway";
static const char    *AP_PASS    = "flashme123";   /* >= 8 chars for WPA2 */
static const char    *BLE_NAME   = "STM32-OTA-BLE";
static const uint16_t TCP_PORT   = 3333;
static const uint32_t STM32_BAUD = 115200;
static const int      PIN_RX2    = 16;             /* from STM32 PA9 (TX) */
static const int      PIN_TX2    = 17;             /* to   STM32 PA10 (RX) */

/* Nordic UART Service (the de-facto BLE serial profile) */
#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   /* host -> ESP32 (write)  */
#define NUS_TX      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   /* ESP32 -> host (notify) */

WiFiServer server(TCP_PORT);
WiFiClient tcpClient;

NimBLECharacteristic *bleTx = nullptr;
volatile bool bleConnected = false;

/* Whoever last sent us a byte owns the reply path back from the STM32. */
enum Sink { SINK_NONE, SINK_TCP, SINK_BLE };
volatile Sink activeSink = SINK_NONE;

/* --- BLE callbacks --------------------------------------------------------- */
class RxCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *chr) override {
        std::string v = chr->getValue();
        if (!v.empty()) {
            activeSink = SINK_BLE;
            Serial2.write((const uint8_t *)v.data(), v.size());
        }
    }
};

class ServerCallback : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *) override    { bleConnected = true; }
    void onDisconnect(NimBLEServer *) override { bleConnected = false; NimBLEDevice::startAdvertising(); }
};

static void startBle()
{
    NimBLEDevice::init(BLE_NAME);
    NimBLEDevice::setMTU(247);                     /* bigger PDUs -> fewer round-trips */

    NimBLEServer *srv = NimBLEDevice::createServer();
    srv->setCallbacks(new ServerCallback());

    NimBLEService *svc = srv->createService(NUS_SERVICE);
    NimBLECharacteristic *rx = svc->createCharacteristic(
        NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new RxCallback());
    bleTx = svc->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY);
    svc->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE);
    adv->setScanResponse(true);
    adv->start();
}

/* --- setup / loop ---------------------------------------------------------- */
void setup()
{
    Serial.begin(115200);
    Serial2.begin(STM32_BAUD, SERIAL_8N1, PIN_RX2, PIN_TX2);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    IPAddress ip = WiFi.softAPIP();

    startBle();

    Serial.printf("\nGateway up. Flash the STM32 (in bootloader mode) with either:\n");
    Serial.printf("  python bl_host.py tcp:%s:%u flash app.bin\n", ip.toString().c_str(), TCP_PORT);
    Serial.printf("  python bl_host.py ble:%s flash app.bin\n", BLE_NAME);

    server.begin();
    server.setNoDelay(true);
}

void loop()
{
    /* accept / refresh the TCP client */
    if (!tcpClient || !tcpClient.connected()) {
        WiFiClient c = server.available();
        if (c) {
            tcpClient = c;
            tcpClient.setNoDelay(true);
            while (Serial2.available()) Serial2.read();   /* drop stale UART bytes */
            Serial.println("TCP client connected.");
        }
    }

    /* TCP -> UART */
    if (tcpClient && tcpClient.connected()) {
        uint8_t buf[256];
        int n = tcpClient.available();
        if (n > 0) {
            if (n > (int)sizeof(buf)) n = sizeof(buf);
            int r = tcpClient.read(buf, n);
            if (r > 0) { activeSink = SINK_TCP; Serial2.write(buf, r); }
        }
    }

    /* UART -> whichever host is active (BLE writes go out as one notification) */
    int m = Serial2.available();
    if (m > 0) {
        uint8_t buf[256];
        if (m > (int)sizeof(buf)) m = sizeof(buf);
        int r = Serial2.readBytes(buf, m);
        if (r > 0) {
            if (activeSink == SINK_TCP && tcpClient && tcpClient.connected()) {
                tcpClient.write(buf, r);
            } else if (activeSink == SINK_BLE && bleConnected && bleTx) {
                bleTx->setValue(buf, r);
                bleTx->notify();
            }
        }
    }
}
