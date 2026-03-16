#include <Arduino.h>
#include "AudioGeneratorAAC.h"
#include "AudioOutputI2S.h"
#include "AudioFileSourcePROGMEM.h"
#include "sampleaac.h"
#include <Wire.h>
#include <MAX30105.h>
#include <heartRate.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_pm.h>
#include <esp_cpu.h>

TaskHandle_t BLE_handler = NULL;
TaskHandle_t Audio_handler = NULL;
TaskHandle_t BPM_handler = NULL;
TaskHandle_t accln_handler = NULL;

// Mutexes for shared variable protection
SemaphoreHandle_t xMutex_audio = NULL;  // protects: playing, aac, file
SemaphoreHandle_t xMutex_data  = NULL;  // protects: delta, irValue, beatAvg

//BLE Server name (the other ESP32 name running the server sketch)
#define bleServerName "Elderly Watch V1.1"
// Characteristic to notify the button state
BLECharacteristic *pCharacteristic; 
bool deviceConnected = false;
// Callback class to handle BLE server events
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    //Serial.println("Device connected");
  };
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    //Serial.println("Device disconnected");
    BLEDevice::startAdvertising(); // Restart advertising so we can connect again
  }
};

//Heart rate sensor object
MAX30105 particleSensor;
const byte RATE_SIZE = 4; //Increase this for more averaging. 4 is good.
byte rates[RATE_SIZE]; //Array of heart rates
byte rateSpot = 0;
long lastBeat = 0; //Time at which the last beat occurred
float beatsPerMinute;
int beatAvg;
long irValue;

//Audio objects
AudioFileSourcePROGMEM *file;
AudioGeneratorAAC *aac;
AudioOutputI2S *out;
bool playing = false;

// ADXL335 Vibration Detection
const int X_PIN = D0;
const int Y_PIN = D1;
const int Z_PIN = D2;
const float VREF = 3.3;           // ESP32 ADC reference
const int ADC_RES = 4095;         // 12-bit ADC
const float ZERO_G = 1.65;        // ~1.65V at 0g
const float SENSITIVITY = 0.300;  // 300mV/g = 0.300V/g
float prev_mag = 0;
const float VIB_THRESHOLD = 1.1;  // vibration threshold in g
float delta = 0.00;

float readAxis(int pin)
{
  int adc = analogRead(pin);
  float voltage = ((float)adc / ADC_RES) * VREF;
  float g = (voltage - ZERO_G) / SENSITIVITY;
  return g;
}

//set BLE task
void BLE_TASK(void *parameter){
  while(1){
    // Take snapshot of shared variables under mutex
    float snap_delta;
    bool snap_playing;
    long snap_irValue;
    int snap_beatAvg;

    if (xSemaphoreTake(xMutex_data, portMAX_DELAY)) {
      snap_delta   = delta;
      snap_irValue = irValue;
      snap_beatAvg = beatAvg;
      xSemaphoreGive(xMutex_data);
    }
    if (xSemaphoreTake(xMutex_audio, portMAX_DELAY)) {
      snap_playing = playing;
      xSemaphoreGive(xMutex_audio);
    }

    String mesString;
    mesString += "M" + String(snap_delta);
    mesString += snap_playing ? "-Vib-" : "-Safe-";
    mesString += snap_irValue<50000 ? "PF" : String(snap_beatAvg);

    //send data over bluetooth
    if (deviceConnected) {
      pCharacteristic->setValue(mesString);
      pCharacteristic->notify(); //max 20 bytes per second, so we add a delay to avoid congestion
    }
    vTaskDelay(10/portTICK_PERIOD_MS); // Always delay regardless of connection state
  }
}

//set audio tasks
void AUDIO_TASK(void *parameter){
  while(1){
    //keep playing until finished
    if (xSemaphoreTake(xMutex_audio, portMAX_DELAY)) {
      if(playing) {
        if (aac->isRunning()) {
          aac->loop();
        } else {    
          aac -> stop(); 
          playing = false; 
          Serial.printf("Sound finished\n");
        }
      }
      xSemaphoreGive(xMutex_audio);
    }
    vTaskDelay(1/portTICK_PERIOD_MS); // Always yield every iteration to avoid starving other tasks
  }
}

//set BPM tasks
void BPM_TASK(void *parameter){
  while(1){
    // Heart rate measurement
  long snap_irValue = particleSensor.getIR();

  if (checkForBeat(snap_irValue) == true) {
    // We sensed a beat!
    long beatDelta = millis() - lastBeat; // renamed from 'delta' to avoid shadowing global delta
    lastBeat = millis();
    beatsPerMinute = 60 / (beatDelta / 1000.0);
    if (beatsPerMinute < 255 && beatsPerMinute > 20) {
      rates[rateSpot++] = (byte)beatsPerMinute; //Store this reading in
      rateSpot %= RATE_SIZE; //Wrap variable
      //Take average of readings
      int localAvg = 0;
      for (byte x = 0; x < RATE_SIZE; x++)
        localAvg += rates[x];
      localAvg /= RATE_SIZE;

      // Write shared variables under mutex
      if (xSemaphoreTake(xMutex_data, portMAX_DELAY)) {
        beatAvg  = localAvg;
        irValue  = snap_irValue;
        xSemaphoreGive(xMutex_data);
      }
    }
    Serial.print("IR=");
    Serial.print(snap_irValue);
    Serial.print(" BPM=");
    Serial.print(beatsPerMinute);
    Serial.print(" Avg BPM=");
    Serial.println(beatAvg);
  }

  // Update irValue even when no beat detected so BLE task stays current
  if (xSemaphoreTake(xMutex_data, portMAX_DELAY)) {
    irValue = snap_irValue;
    xSemaphoreGive(xMutex_data);
  }

  if (snap_irValue < 50000) {
    Serial.println("No finger detected");
  }
  vTaskDelay(10/portTICK_PERIOD_MS);
}
}

//void accln tasks
void ACCLN_TASK(void *parameter){
  while(1){
  // Read raw analog values from accelerometer axes and convert to g's
  float x = readAxis(X_PIN);
  float y = readAxis(Y_PIN);
  float z = readAxis(Z_PIN);

  // acceleration magnitude
  float mag = sqrt(x*x + y*y + z*z);

  // detect sudden vibration
  float local_delta = abs(mag - prev_mag);

  // Write delta under mutex
  if (xSemaphoreTake(xMutex_data, portMAX_DELAY)) {
    delta = local_delta;
    xSemaphoreGive(xMutex_data);
  }

  Serial.println(local_delta);

  // Check playing state and trigger audio under mutex to prevent use-after-free
  if (xSemaphoreTake(xMutex_audio, portMAX_DELAY)) {
    if (local_delta > VIB_THRESHOLD && !playing) {
      Serial.print("Vibration detected! ");
      Serial.println(local_delta);
      //play sound
      delete aac; // Clean up previous generator if it exists
      delete file; // Clean up previous file source if it exists
      aac = new AudioGeneratorAAC(); // Create a new generator instance
      file = new AudioFileSourcePROGMEM(sampleaac, sizeof(sampleaac));
      aac->begin(file, out); // Start the audio generator with the file source and output
      playing = true;
    }
    xSemaphoreGive(xMutex_audio);
  }

  prev_mag = mag; //store current magnitude of g value to previous magnitude value
  vTaskDelay(10/portTICK_PERIOD_MS);
  //delay(5); // Adjust delay as needed for responsiveness
}
}


void setup()
{
  Serial.begin(115200);
  analogReadResolution(12); // Set ADC resolution to 12 bits

  // Create mutexes before starting any tasks
  xMutex_audio = xSemaphoreCreateMutex();
  xMutex_data  = xSemaphoreCreateMutex();

  //disable wifi to save power and avoid interference with BLE
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  // Set CPU frequency to 80MHz to save power
  setCpuFrequencyMhz(80);

  // Initialize BLE and create server
  BLEDevice::init(bleServerName);
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  //UUID setup
  BLEService *pService = pServer->createService(BLEUUID((uint16_t)0x182A)); // Environmental Sensing
  pCharacteristic = pService->createCharacteristic(
    BLEUUID((uint16_t)0x2A59), // Analog Output
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->addDescriptor(new BLE2902());
  //setup params and start BLE advertising
  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(pService->getUUID());
  pAdvertising->setScanResponse(true); // allows the phone to see the service UUID in the scan results
  pAdvertising->setMinPreferred(0x0);   // functions that help with iPhone connections issue
  pAdvertising->setMinPreferred(0x1F);  // functions that help with iPhone connections issue
  pAdvertising->setMinInterval(1600); // advertising interval 1s 1/0.000625
  pAdvertising->setMaxInterval(3200); // advertising interval 2s
  BLEDevice::startAdvertising();

  //initialize audio in, out, pins
  file = new AudioFileSourcePROGMEM(sampleaac, sizeof(sampleaac));
  aac = new AudioGeneratorAAC();
  out = new AudioOutputI2S();
  out -> SetGain(0.1); //maximum is 4
  //out -> SetPinout(26,25,22); ESP32-wroom pinout //blck, lrc, din respectively
  out -> SetPinout(D8,D9,D10); //esp32s3 xiao (D8, D9, D10) //blck, lrc, din respectively
  //aac->begin(in, out); // Start the audio generator with the file source and output

  // Initialize heart rate sensor
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 was not found. Please check wiring/power. ");
    while (1);
  }
  //heart rate sensor params
  particleSensor.setup(); //Configure sensor with default settings
  particleSensor.setPulseAmplitudeRed(0x0A); //Turn Red LED to low to indicate sensor is running
  particleSensor.setPulseAmplitudeGreen(0); //Turn off Green LED

  xTaskCreatePinnedToCore(
    BLE_TASK,
    "Bluetooth low energy tasks",
    4096,
    NULL,
    2,
    &BLE_handler,
    1);

  xTaskCreatePinnedToCore(
    AUDIO_TASK,
    "Audio playback",
    8192,
    NULL,
    1,    // Lowered from 5 → 1 so it doesn't starve BPM and ACCLN tasks
    &Audio_handler,
    1);

  xTaskCreatePinnedToCore(
    BPM_TASK,
    "heart rate monitoring",
    4096,
    NULL,
    3,
    &BPM_handler,
    1);

  xTaskCreatePinnedToCore(
    ACCLN_TASK,
    "accelerometer monitoring",
    4096,  // Increased from 2048 → 4096 for float math + Serial headroom
    NULL,
    2,
    &accln_handler,
    1);
}

void loop()
{
vTaskDelay(portMAX_DELAY);
}