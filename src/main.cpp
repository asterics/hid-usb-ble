
#include <Arduino.h>
#include <BleMouse.h>
#include <BLEDevice.h>
#include "usb_host.h"

BleMouse bleMouse("Assistronik USB Adapter","Assistronik");


void update_mouseState(unified_mouseReport_t *mouse_report) {

  static int x_pos = 0;
  static int y_pos = 0;

  // Calculate absolute position from displacement
  x_pos += mouse_report->x_displacement;
  y_pos += mouse_report->y_displacement;

  if(bleMouse.isConnected()) {
    static bool leftButtonPressed = false;
    static bool rightButtonPressed = false;
    static bool middleButtonPressed = false;

    bleMouse.move(mouse_report->x_displacement, mouse_report->y_displacement);

    // Track and update left button state
    if (mouse_report->buttons.button1 != leftButtonPressed) {
      leftButtonPressed = mouse_report->buttons.button1;
      if (leftButtonPressed) {
        bleMouse.press(MOUSE_LEFT);
      } else {
        bleMouse.release(MOUSE_LEFT);
      }
    }
    
    // Track and update right button state
    if (mouse_report->buttons.button2 != rightButtonPressed) {
      rightButtonPressed = mouse_report->buttons.button2;
      if (rightButtonPressed) {
        bleMouse.press(MOUSE_RIGHT);
      } else {
        bleMouse.release(MOUSE_RIGHT);
      }
    }

    // Track and update middle button state
    if (mouse_report->buttons.button3 != middleButtonPressed) {
      middleButtonPressed = mouse_report->buttons.button3;
      if (middleButtonPressed) {
        bleMouse.press(MOUSE_MIDDLE);
      } else {
        bleMouse.release(MOUSE_MIDDLE);
      }
    }

    // Handle scroll wheel
    if (mouse_report->scroll_wheel != 0) {
      bleMouse.move(0, 0, mouse_report->scroll_wheel);
    }
  }
}


void unbond_all_devices() {
    int dev_num = 0;  // To store the number of bonded devices
    esp_ble_bond_dev_t *dev_list = NULL;

    // Get the number of bonded devices
    dev_num = esp_ble_get_bond_device_num();
    if (dev_num == 0) {
        ESP_LOGI("UNBOND", "No bonded devices found");
        return;
    }

    // Allocate memory to hold the list of bonded devices
    dev_list = (esp_ble_bond_dev_t*)malloc(dev_num * sizeof(esp_ble_bond_dev_t));
    if (dev_list == NULL) {
        ESP_LOGE("UNBOND", "Failed to allocate memory for device list");
        return;
    }

    // Get the list of bonded devices
    esp_err_t err = esp_ble_get_bond_device_list(&dev_num, dev_list);
    if (err != ESP_OK) {
        ESP_LOGE("UNBOND", "Failed to get bonded device list");
        free(dev_list);
        return;
    }

    // Iterate over each bonded device and unbond it
    for (int i = 0; i < dev_num; i++) {
        esp_ble_bond_dev_t dev = dev_list[i];
        esp_bd_addr_t bd_addr;
        memcpy(&bd_addr,&dev.bd_addr, sizeof(esp_bd_addr_t)); // Get the Bluetooth address of the device
        ESP_LOGI("UNBOND", "Removing bond for device with BD_ADDR: %02x:%02x:%02x:%02x:%02x:%02x",
                 bd_addr[0], bd_addr[1], bd_addr[2], bd_addr[3], bd_addr[4], bd_addr[5]);

        err = esp_ble_remove_bond_device(bd_addr);
        if (err == ESP_OK) {
            ESP_LOGI("UNBOND", "Successfully removed bond for device: %02x:%02x:%02x:%02x:%02x:%02x",
                     bd_addr[0], bd_addr[1], bd_addr[2], bd_addr[3], bd_addr[4], bd_addr[5]);
        } else {
            ESP_LOGE("UNBOND", "Failed to remove bond for device: %02x:%02x:%02x:%02x:%02x:%02x",
                     bd_addr[0], bd_addr[1], bd_addr[2], bd_addr[3], bd_addr[4], bd_addr[5]);
        }
    }

    // Free the allocated memory
    free(dev_list);
}


void setup() { 
    Serial.begin(115200);
    Serial.setDebugOutput(true);

    //use internal button & LED, switch off LED
    pinMode(GPIO_NUM_0,INPUT_PULLUP);
    pinMode(LED_BUILTIN,OUTPUT);
    digitalWrite(LED_BUILTIN,HIGH);

    // start BLE mouse
    bleMouse.begin();

    // register mouse report callback handler
    register_mouse_report_callback(update_mouseState);

    //start main USB/HID task
    start_usb_host(); 
}

void loop() {
  static long lastbuttonPress = 0;
  if(bleMouse.isConnected()) {
    //Serial.println("Moving mouse");
    //Move mouse in a circle
    //bleMouse.move(10, 0);   // Move right
    //delay(100);
    //bleMouse.move(0, 10);   // Move down
    //delay(100);
    //bleMouse.move(-10, 0);  // Move left
    //delay(100);
    //bleMouse.move(0, -10);  // Move up
    //delay(100);
  }

  //button press
  if(digitalRead(GPIO_NUM_0) == LOW && lastbuttonPress == 0) {
    lastbuttonPress = millis();
    digitalWrite(LED_BUILTIN,LOW);
  }
  //button release
  if(digitalRead(GPIO_NUM_0) == HIGH) {
    lastbuttonPress = 0;
  }

  if(lastbuttonPress && (millis() - lastbuttonPress > 1000)) {
    Serial.println("Reset pairings");
    unbond_all_devices();
    lastbuttonPress = 0;
    //blink a few times
    for(int i = 0; i<5; i++) {
      digitalWrite(LED_BUILTIN,LOW);
      delay(250);
      digitalWrite(LED_BUILTIN,HIGH);
      delay(250);
    }
    //restart
    esp_restart();
  }

  //TODO: stop advertising after x seconds?
  //BLEDevice::getAdvertising()->stop();

  delay(20);
}
