#include "BTManager.h"
#include <BLEServer.h>
#include <BLE2902.h>

BLEScan* pBLEScan;
std::vector<String> foundBTDevices;
static volatile bool btScanning = false;

BLEServer *pServer = NULL;
BLECharacteristic * pTxCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      String rxValue = pCharacteristic->getValue().c_str();
      if (rxValue.length() > 0) {
        Serial.println("*********");
        Serial.print("Received Value: ");
        for (int i = 0; i < rxValue.length(); i++) {
          Serial.print(rxValue[i]);
        }
        Serial.println();
        Serial.println("*********");
      }
    }
};

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        String name = advertisedDevice.getName().c_str();
        if (name.length() > 0) {
            foundBTDevices.push_back(name);
        }
    }
};

void setupBT() {
    // Watch port: null-guard the createServer call. If BLE init failed
    // (e.g. WiFi coex refused, heap fragmented, controller busy), then
    // pServer stays NULL and `setCallbacks` on it panics with
    // StoreProhibited — confirmed via serial log during the boot loop.
    BLEDevice::init(bluetoothName.c_str());

    pServer = BLEDevice::createServer();
    if (!pServer) {
        Serial.println("[BT] createServer() returned NULL — aborting setupBT");
        BLEDevice::deinit();
        return;
    }
    pServer->setCallbacks(new MyServerCallbacks());

    // Create the BLE Service
    BLEService *pService = pServer->createService(SERVICE_UUID);
    if (!pService) {
        Serial.println("[BT] createService() failed — aborting setupBT");
        BLEDevice::deinit();
        pServer = nullptr;
        return;
    }

    // Create a BLE Characteristic for TX
    pTxCharacteristic = pService->createCharacteristic(
                    CHARACTERISTIC_UUID_TX,
                    BLECharacteristic::PROPERTY_NOTIFY
                  );
                      
    pTxCharacteristic->addDescriptor(new BLE2902());

    // Create a BLE Characteristic for RX
    BLECharacteristic * pRxCharacteristic = pService->createCharacteristic(
                       CHARACTERISTIC_UUID_RX,
                      BLECharacteristic::PROPERTY_WRITE
                    );

    pRxCharacteristic->setCallbacks(new MyCallbacks());

    // Start the service
    pService->start();

    // Start advertising
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);  
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();

    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
}

void loopBT() {
    if (!deviceConnected && oldDeviceConnected) {
        delay(500); // give the bluetooth stack the chance to get things ready
        pServer->startAdvertising(); // restart advertising
        Serial.println("Start advertising");
        oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }
}

void stopBT() {
    BLEDevice::deinit();
}

void scanBT() {
    // v4.4: null-guard. pBLEScan is only set after setupBT() runs; callers
    // (Ducky IF_BT_PRESENT, processAutomation with bluetoothToggleEnabled=false)
    // could reach scanBT before setupBT and crash on the null deref.
    if (!bluetoothToggleEnabled || !pBLEScan) {
        Serial.println("[BT] scanBT: BT not initialised — skipped");
        return;
    }
    foundBTDevices.clear();
    Serial.println("Scanning for BT devices...");
    btScanning = true;
    BLEScanResults* foundDevices = pBLEScan->start(5, false);
    btScanning = false;
    if (foundDevices) {
        Serial.print("BT Devices found: ");
        Serial.println(foundDevices->getCount());
    }
    pBLEScan->clearResults();
}

bool btScanInProgress() {
    return btScanning;
}

bool isBTDevicePresent(String name) {
    for (String device : foundBTDevices) {
        if (device.indexOf(name) != -1) return true;
    }
    return false;
}

int getBTClientCount() {
    return deviceConnected ? 1 : 0;
}

void stopBTAdvertising() {
    if (pServer) {
        BLEDevice::getAdvertising()->stop();
        Serial.println("[BT] Advertising stopped");
    }
}

void startBTAdvertising() {
    if (pServer) {
        BLEDevice::getAdvertising()->start();
        Serial.println("[BT] Advertising started");
    }
}
