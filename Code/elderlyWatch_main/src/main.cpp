#include <Arduino.h>
#include "AudioGeneratorAAC.h"
#include "AudioOutputI2S.h"
#include "AudioFileSourcePROGMEM.h"
#include "sampleaac.h"

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
const float VIB_THRESHOLD = 1.5;  // vibration threshold in g

float readAxis(int pin)
{
  int adc = analogRead(pin);
  float voltage = ((float)adc / ADC_RES) * VREF;
  float g = (voltage - ZERO_G) / SENSITIVITY;
  return g;
}

void setup()
{
  Serial.begin(115200);
  analogReadResolution(12); // Set ADC resolution to 12 bits

  file = new AudioFileSourcePROGMEM(sampleaac, sizeof(sampleaac));
  aac = new AudioGeneratorAAC();
  out = new AudioOutputI2S();
  out -> SetGain(0.3); //maximum is 4
  //out -> SetPinout(26,25,22); ESP32-wroom pinout //blck, lrc, din respectively
  out -> SetPinout(D8,D9,D10); //esp32s3 xiao (D3, D4, D5) //blck, lrc, din respectively
  //aac->begin(in, out); // Start the audio generator with the file source and output
}

void loop()
{
  // Read raw analog values from accelerometer axes and convert to g's
  float x = readAxis(X_PIN);
  float y = readAxis(Y_PIN);
  float z = readAxis(Z_PIN);

  // acceleration magnitude
  float mag = sqrt(x*x + y*y + z*z);
  // detect sudden vibration
  float delta = 0.00;
  delta = abs(mag - prev_mag);
  Serial.print(" | Delta (g): ");
  Serial.println(delta);

  if (delta > VIB_THRESHOLD && !playing) {
    Serial.print("Vibration detected! ");
    Serial.println(delta);

    delete aac; // Clean up previous generator if it exists
    delete file; // Clean up previous file source if it exists
    aac = new AudioGeneratorAAC(); // Create a new generator instance
    file = new AudioFileSourcePROGMEM(sampleaac, sizeof(sampleaac));
    aac->begin(file, out); // Start the audio generator with the file source and output
    playing = true;
  }

  if(playing) {
    if (aac->isRunning()) {
      aac->loop();
    } else {    
      aac -> stop(); 
      playing = false; 
      Serial.printf("Sound finished\n");
    }
  }
  prev_mag = mag;
  delay(10); // Adjust delay as needed for responsiveness
}