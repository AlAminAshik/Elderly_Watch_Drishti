// //server
// #include <BLEDevice.h>
// #include <BLEUtils.h>
// #include <BLE2902.h>
// #include <BLEServer.h>
// #include <Arduino.h>
// #include <WiFi.h>
// #include <esp_wifi.h>
// #include <esp_pm.h>
// #include <esp_cpu.h>

// //BLE Server name (the other ESP32 name running the server sketch)
// #define bleServerName "Elderly Watch V1.1"

// BLECharacteristic *pCharacteristic; // Characteristic to notify the button state
// bool deviceConnected = false;

// #define buttonPin D1
// bool buttonState = 0;

// // Callback class to handle BLE server events
// class MyServerCallbacks: public BLEServerCallbacks {
//   void onConnect(BLEServer* pServer) {
//     deviceConnected = true;
//     //Serial.println("Device connected");
//   };
  
//   void onDisconnect(BLEServer* pServer) {
//     deviceConnected = false;
//     //Serial.println("Device disconnected");
//     BLEDevice::startAdvertising(); // Restart advertising so we can connect again
//   }
// };

// void setup() {
//   //Serial.begin(115200);

//   pinMode(buttonPin, INPUT_PULLUP);

//     //disable wifi to save power and avoid interference with BLE
//     WiFi.mode(WIFI_OFF);
//     esp_wifi_stop();
//     // Set CPU frequency to 80MHz to save power
//     setCpuFrequencyMhz(80);

//     // Initialize BLE and create server
//   BLEDevice::init(bleServerName);
//   BLEServer *pServer = BLEDevice::createServer();
//   pServer->setCallbacks(new MyServerCallbacks());
  
//   BLEService *pService = pServer->createService(BLEUUID((uint16_t)0x182A)); // Environmental Sensing
//   pCharacteristic = pService->createCharacteristic(
//     BLEUUID((uint16_t)0x2A59), // Analog Output
//     BLECharacteristic::PROPERTY_NOTIFY
//   );
//   pCharacteristic->addDescriptor(new BLE2902());
  
//   pService->start();
//   BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
//   pAdvertising->addServiceUUID(pService->getUUID());
//   pAdvertising->setScanResponse(true); // allows the phone to see the service UUID in the scan results
//   pAdvertising->setMinPreferred(0x0);   // functions that help with iPhone connections issue
//   pAdvertising->setMinPreferred(0x1F);  // functions that help with iPhone connections issue
//   pAdvertising->setMinInterval(1600); // advertising interval 1s 1/0.000625
//   pAdvertising->setMaxInterval(3200); // advertising interval 2s
//   BLEDevice::startAdvertising();
// }

// void loop() {
//   if (deviceConnected) {
//     buttonState = digitalRead(buttonPin);
//     pCharacteristic->setValue(buttonState ? "PRESSED" : "RELEASED");
//     pCharacteristic->notify(); //max 20 bytes per second, so we add a delay to avoid congestion
//     delay(100); // bluetooth stack will go into congestion, if too many packets are sent
//   }
// }