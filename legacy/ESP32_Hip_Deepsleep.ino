#include "BluetoothSerial.h"

const int buttonPin = 4;
const int outputPins[] = { 14, 27, 26, 25 };
const int LED = 32;
int onoff = 1;

float receivedValues[4];

#define USE_NAME  // Comment this to use MAC address instead of a slaveName

// Check if Bluetooth is available
#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to and enable it
#endif

// Check Serial Port Profile
#if !defined(CONFIG_BT_SPP_ENABLED)
#error Serial Port Profile for Bluetooth is not available or not enabled. It is only available for the ESP32 chip.
#endif
BluetoothSerial SerialBT;

#ifdef USE_NAME
String slaveName = "ESP32-BT-Foot";  // Change this to reflect the real name of your slave BT device
#else
String MACadd = "AA:BB:CC:11:22:33";                          // This only for printing
uint8_t address[6] = { 0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33 };  // Change this to reflect real MAC address of your slave BT device
#endif

String myName = "ESP32-BT-Stumpf";

void setup() {
  pinMode(buttonPin, INPUT_PULLDOWN);
  pinMode(LED, OUTPUT);
  pinMode(35, INPUT);  // You should set the input pin
  pinMode(14, OUTPUT);
  pinMode(27, OUTPUT);
  pinMode(26, OUTPUT);
  pinMode(25, OUTPUT);
  bool connected;
  Serial.begin(115200);
  motor_null();
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, HIGH);  //Configure GPIO4 as a wake-up source when the voltage is 3.3V
  SerialBT.begin(myName, true);
  //SerialBT.deleteAllBondedDevices(); // Uncomment this to delete paired devices; Must be called after begin
  //Serial.printf("The device \"%s\" started in master mode, make sure slave BT device is on!\n", myName.c_str());
  blink_led();

#ifndef USE_NAME
  SerialBT.setPin(5645);
  //Serial.println("Using PIN");
#endif

// connect(address) is fast (up to 10 secs max), connect(slaveName) is slow (up to 30 secs max) as it needs
// to resolve slaveName to address first, but it allows to connect to different devices with the same name.
// Set CoreDebugLevel to Info to view devices Bluetooth address and device names
#ifdef USE_NAME
  connected = SerialBT.connect(slaveName);
  //Serial.printf("Connecting to slave BT device named \"%s\"\n", slaveName.c_str());
  blink_led();
#else
  connected = SerialBT.connect(address);
  //Serial.print("Connecting to slave BT device with MAC ");
  //Serial.println(MACadd);
  blink_led();
#endif

  float v_mes = analogRead(35);
  float v_dc = 1.435 * (v_mes / 4095) * 3.3;
  //  Serial.println(v_dc); //remove all Serial.Print for smaller code, but usefull for debugging
  if (v_dc <= 3.0) {
    battery_blink_led();
    motor_blink();
    digitalWrite(LED, LOW);
    esp_deep_sleep_start();
  }

  if (connected) {
    Serial.println("Connected Successfully!");
    digitalWrite(LED, HIGH);
  } else {
    while (!SerialBT.connected(10000)) {
      //Serial.println("Failed to connect. Make sure remote device is available and in range, then restart app.");
      digitalWrite(LED, LOW);
      esp_deep_sleep_start();
    }
  }
  // Disconnect() may take up to 10 secs max
  if (SerialBT.disconnect()) {
    //Serial.println("Disconnected Successfully!");
    digitalWrite(LED, LOW);
  }
  // This would reconnect to the slaveName(will use address, if resolved) or address used with connect(slaveName/address).
  SerialBT.connect();
  if (connected) {
    //Serial.println("Reconnected Successfully!");
    digitalWrite(LED, HIGH);
  } else {
    while (!SerialBT.connected(10000)) {
      //Serial.println("Failed to reconnect. Make sure remote device is available and in range, restart.");
      digitalWrite(LED, LOW);
      esp_deep_sleep_start();
    }
  }
  //setCpuFrequencyMhz(240);
}

void loop() {

  if (SerialBT.available()) {
    int buttonState = digitalRead(buttonPin);
    if (buttonState == HIGH) {
      // Button pressed
      delay(50);  // debounce button
      if (digitalRead(buttonPin) == HIGH) {
        //Serial.println("1,5 Sek wait");
        delay(1500);
        if (digitalRead(buttonPin) == HIGH) {
          blink_led();   //Time to release Button
          delay(1500);   //together thats 3sek. to toggle deepsleep
          motor_null();  //settings motors to null
          //Serial.println("Good Night");
          //Serial.println("----------------------");
          esp_deep_sleep_start();

        } else {
          // BUtton was pressed less than 3 sec, just vibration turned off
          onoff = 1 - onoff;  // Switching state (0 to 1 or 1 to 0)
          //Serial.print("Short press. Now it is: ");
          //Serial.println(onoff);
          delay(500);  // Optional: delay to reduce debounce
        }
      }
    }

    String received = SerialBT.readStringUntil('\n');
    int i = 0;
    for (int i = 0; i < 4; i++) {
      int idx = received.indexOf(',');
      if (idx != -1) {
        receivedValues[i] = received.substring(0, idx).toFloat();
        received.remove(0, idx + 1);
      }
    }
    for (int i = 0; i < 4; i++) {
      // Convert the voltage value back into a value that can be written to the digital output
      int outputValue = (int)(receivedValues[i] / 3.0 * 255.0);

      // Calibrating the pressure sensors for a similar output representation on the vibration
      // These values needs to be set via App
      if (outputPins[i] == 26) {
        if (outputValue >= 200) {
        } else {
          outputValue = outputValue - 50;
        }
      }
      if (outputPins[i] == 14) {
        if (outputValue >= 5) {
          outputValue = outputValue + 70;
        }
      }
      if (outputPins[i] == 25) {
        if (outputValue >= 5) {
          outputValue = outputValue + 100;
        }
      }
      if (outputPins[i] == 27) {
        if (outputValue >= 5) {
          outputValue = outputValue + 50;
        }
      }

      if (onoff == 1) {
      // These values needs to be set via App
        if (outputValue >= 200) {
          analogWrite(outputPins[i], 255);
        } else if (outputValue >= 125) {
          analogWrite(outputPins[i], 200);
        } else if (outputValue >= 40) {
          analogWrite(outputPins[i], 150);
        } else if (outputValue >= 30) {
          analogWrite(outputPins[i], 70);
        } else {
          analogWrite(outputPins[i], 0);
        }
//        Serial.println(i);
//        Serial.println(outputPins[i]);
//        Serial.println(outputValue);
      } else {
        motor_null();
      }
    }
  }
  delay(20);
}

void blink_led() {
  for (int i = 0; i < 10; i++) {
    digitalWrite(LED, HIGH);
    delay(150);
    digitalWrite(LED, LOW);
    delay(150);
  }
}

void battery_blink_led() {
  for (int i = 0; i < 10; i++) {
    digitalWrite(LED, HIGH);
    delay(1300);
    digitalWrite(LED, LOW);
    delay(1300);
  }
}

void motor_null() {
  for (int i = 0; i < 4; i++) {
    analogWrite(outputPins[i], 0);
  }
}

void motor_blink() {
  for (int i = 0; i < 4; i++) {
    analogWrite(outputPins[i], 255);
    delay(500);
    motor_null();
  }
}
