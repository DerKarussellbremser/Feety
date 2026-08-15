#include "BluetoothSerial.h"

String device_name = "ESP32-BT-Foot";
String master_name = "ESP32-BT-Stumpf";

// Check if Bluetooth is available
#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to and enable it
#endif

// Check Serial Port Profile
#if !defined(CONFIG_BT_SPP_ENABLED)
#error Serial Port Profile for Bluetooth is not available or not enabled. It is only available for the ESP32 chip.
#endif

BluetoothSerial SerialBT;

const int switchPin = 21;
const int LED = 26;
bool sleepSignalReceived = false;
const int sensorPins[] = { 15, 4, 27, 32, 32 };  //dunno why, but with 4 Variables the hip recivies 3, something with the /n but fi
float sensorValues[5];

void setup() {
  pinMode(switchPin, INPUT_PULLDOWN);
  analogReadResolution(12);
  pinMode(LED, OUTPUT);
  for (int i = 0; i < 5; i++) {
//    for (int i = 0; i < 4; i++) {
    pinMode(sensorPins[i], INPUT_PULLDOWN);
  }
  Serial.begin(115200);
  SerialBT.begin(device_name);  //Bluetooth device name
  //SerialBT.deleteAllBondedDevices(); // Uncomment this to delete paired devices; Must be called after begin
  Serial.printf("The device with name \"%s\" is started.\nNow you can pair it with Bluetooth!\n", device_name.c_str());
}

void loop() {
  for (int i = 0; i < 5; i++) {
//  for (int i = 0; i < 4; i++) {
    sensorValues[i] = analogRead(sensorPins[i]) * (3.0 / 4095.0);    // converting to voltage
  SerialBT.print(String(sensorValues[i]) + (i < 4 ? "," : "\n"));  // sending data to ESP32 "Stumpf"
    Serial.println(i);
    Serial.println(String(sensorValues[i]));
  }
  delay(20);
}
