#include <Arduino.h>
#include <BleMouse.h>

BleMouse bleMouse;
/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "usb/usb_host.h"

#include "hid_host.h"
#include "hid_usage_keyboard.h"

/* GPIO Pin number for quit from example logic */
#define APP_QUIT_PIN                GPIO_NUM_0

static const char *TAG = "example";
QueueHandle_t hid_host_event_queue;
bool user_shutdown = false;


typedef struct {
    union {
        struct {
            uint8_t button1:    1;
            uint8_t button2:    1;
            uint8_t button3:    1;
            uint8_t reserved:   5;
        };
        uint8_t val;
    } buttons;
    int8_t x_displacement;
    int8_t y_displacement;
    int8_t scroll_wheel;
} __attribute__((packed)) hid_mouse_input_report_t;



/**
 * @brief HID Host event
 *
 * This event is used for delivering the HID Host event from callback to a task.
 */
typedef struct {
  hid_host_device_handle_t hid_device_handle;
  hid_host_driver_event_t event;
  void *arg;
} hid_host_event_queue_t;

/**
 * @brief HID Protocol string names
 */
static const char *hid_proto_name_str[] = {"NONE", "KEYBOARD", "MOUSE"};

/**
 * @brief Key event
 */
typedef struct {
  enum key_state { KEY_STATE_PRESSED = 0x00, KEY_STATE_RELEASED = 0x01 } state;
  uint8_t modifier;
  uint8_t key_code;
} key_event_t;

/* Main char symbol for ENTER key */
#define KEYBOARD_ENTER_MAIN_CHAR '\r'
/* When set to 1 pressing ENTER will be extending with LineFeed during serial
 * debug output */
#define KEYBOARD_ENTER_LF_EXTEND 1

/**
 * @brief Scancode to ascii table
 */
const uint8_t keycode2ascii[57][2] = {
    {0, 0},     /* HID_KEY_NO_PRESS        */
    {0, 0},     /* HID_KEY_ROLLOVER        */
    {0, 0},     /* HID_KEY_POST_FAIL       */
    {0, 0},     /* HID_KEY_ERROR_UNDEFINED */
    {'a', 'A'}, /* HID_KEY_A               */
    {'b', 'B'}, /* HID_KEY_B               */
    {'c', 'C'}, /* HID_KEY_C               */
    {'d', 'D'}, /* HID_KEY_D               */
    {'e', 'E'}, /* HID_KEY_E               */
    {'f', 'F'}, /* HID_KEY_F               */
    {'g', 'G'}, /* HID_KEY_G               */
    {'h', 'H'}, /* HID_KEY_H               */
    {'i', 'I'}, /* HID_KEY_I               */
    {'j', 'J'}, /* HID_KEY_J               */
    {'k', 'K'}, /* HID_KEY_K               */
    {'l', 'L'}, /* HID_KEY_L               */
    {'m', 'M'}, /* HID_KEY_M               */
    {'n', 'N'}, /* HID_KEY_N               */
    {'o', 'O'}, /* HID_KEY_O               */
    {'p', 'P'}, /* HID_KEY_P               */
    {'q', 'Q'}, /* HID_KEY_Q               */
    {'r', 'R'}, /* HID_KEY_R               */
    {'s', 'S'}, /* HID_KEY_S               */
    {'t', 'T'}, /* HID_KEY_T               */
    {'u', 'U'}, /* HID_KEY_U               */
    {'v', 'V'}, /* HID_KEY_V               */
    {'w', 'W'}, /* HID_KEY_W               */
    {'x', 'X'}, /* HID_KEY_X               */
    {'y', 'Y'}, /* HID_KEY_Y               */
    {'z', 'Z'}, /* HID_KEY_Z               */
    {'1', '!'}, /* HID_KEY_1               */
    {'2', '@'}, /* HID_KEY_2               */
    {'3', '#'}, /* HID_KEY_3               */
    {'4', '$'}, /* HID_KEY_4               */
    {'5', '%'}, /* HID_KEY_5               */
    {'6', '^'}, /* HID_KEY_6               */
    {'7', '&'}, /* HID_KEY_7               */
    {'8', '*'}, /* HID_KEY_8               */
    {'9', '('}, /* HID_KEY_9               */
    {'0', ')'}, /* HID_KEY_0               */
    {KEYBOARD_ENTER_MAIN_CHAR, KEYBOARD_ENTER_MAIN_CHAR}, /* HID_KEY_ENTER */
    {0, 0},      /* HID_KEY_ESC             */
    {'\b', 0},   /* HID_KEY_DEL             */
    {0, 0},      /* HID_KEY_TAB             */
    {' ', ' '},  /* HID_KEY_SPACE           */
    {'-', '_'},  /* HID_KEY_MINUS           */
    {'=', '+'},  /* HID_KEY_EQUAL           */
    {'[', '{'},  /* HID_KEY_OPEN_BRACKET    */
    {']', '}'},  /* HID_KEY_CLOSE_BRACKET   */
    {'\\', '|'}, /* HID_KEY_BACK_SLASH      */
    {'\\', '|'},
    /* HID_KEY_SHARP           */ // HOTFIX: for NonUS Keyboards repeat
                                  // HID_KEY_BACK_SLASH
    {';', ':'},                   /* HID_KEY_COLON           */
    {'\'', '"'},                  /* HID_KEY_QUOTE           */
    {'`', '~'},                   /* HID_KEY_TILDE           */
    {',', '<'},                   /* HID_KEY_LESS            */
    {'.', '>'},                   /* HID_KEY_GREATER         */
    {'/', '?'}                    /* HID_KEY_SLASH           */
};

/**
 * @brief Makes new line depending on report output protocol type
 *
 * @param[in] proto Current protocol to output
 */
static void hid_print_new_device_report_header(hid_protocol_t proto) {
  static hid_protocol_t prev_proto_output = HID_PROTOCOL_MAX;

  if (prev_proto_output != proto) {
    prev_proto_output = proto;
    printf("\r\n");
    if (proto == HID_PROTOCOL_MOUSE) {
      printf("Mouse\r\n");
    } else if (proto == HID_PROTOCOL_KEYBOARD) {
      printf("Keyboard\r\n");
    } else {
      printf("Generic\r\n");
    }
    fflush(stdout);
  }
}

/**
 * @brief HID Keyboard modifier verification for capitalization application
 * (right or left shift)
 *
 * @param[in] modifier
 * @return true  Modifier was pressed (left or right shift)
 * @return false Modifier was not pressed (left or right shift)
 *
 */
static inline bool hid_keyboard_is_modifier_shift(uint8_t modifier) {
  if (((modifier & HID_LEFT_SHIFT) == HID_LEFT_SHIFT) ||
      ((modifier & HID_RIGHT_SHIFT) == HID_RIGHT_SHIFT)) {
    return true;
  }
  return false;
}

/**
 * @brief HID Keyboard get char symbol from key code
 *
 * @param[in] modifier  Keyboard modifier data
 * @param[in] key_code  Keyboard key code
 * @param[in] key_char  Pointer to key char data
 *
 * @return true  Key scancode converted successfully
 * @return false Key scancode unknown
 */
static inline bool hid_keyboard_get_char(uint8_t modifier, uint8_t key_code,
                                         unsigned char *key_char) {
  uint8_t mod = (hid_keyboard_is_modifier_shift(modifier)) ? 1 : 0;

  if ((key_code >= HID_KEY_A) && (key_code <= HID_KEY_SLASH)) {
    *key_char = keycode2ascii[key_code][mod];
  } else {
    // All other key pressed
    return false;
  }

  return true;
}

/**
 * @brief HID Keyboard print char symbol
 *
 * @param[in] key_char  Keyboard char to stdout
 */
static inline void hid_keyboard_print_char(unsigned int key_char) {
  if (!!key_char) {
    putchar(key_char);
#if (KEYBOARD_ENTER_LF_EXTEND)
    if (KEYBOARD_ENTER_MAIN_CHAR == key_char) {
      putchar('\n');
    }
#endif // KEYBOARD_ENTER_LF_EXTEND
    fflush(stdout);
  }
}

/**
 * @brief Key Event. Key event with the key code, state and modifier.
 *
 * @param[in] key_event Pointer to Key Event structure
 *
 */
static void key_event_callback(key_event_t *key_event) {
  unsigned char key_char;

  hid_print_new_device_report_header(HID_PROTOCOL_KEYBOARD);

  if (key_event->KEY_STATE_PRESSED == key_event->state) {
    if (hid_keyboard_get_char(key_event->modifier, key_event->key_code,
                              &key_char)) {

      hid_keyboard_print_char(key_char);
    }
  }
}

/**
 * @brief Key buffer scan code search.
 *
 * @param[in] src       Pointer to source buffer where to search
 * @param[in] key       Key scancode to search
 * @param[in] length    Size of the source buffer
 */
static inline bool key_found(const uint8_t *const src, uint8_t key,
                             unsigned int length) {
  for (unsigned int i = 0; i < length; i++) {
    if (src[i] == key) {
      return true;
    }
  }
  return false;
}

/**
 * @brief USB HID Host Keyboard Interface report callback handler
 *
 * @param[in] data    Pointer to input report data buffer
 * @param[in] length  Length of input report data buffer
 */
static void hid_host_keyboard_report_callback(const uint8_t *const data,
                                              const int length) {
  hid_keyboard_input_report_boot_t *kb_report =
      (hid_keyboard_input_report_boot_t *)data;

  if (length < sizeof(hid_keyboard_input_report_boot_t)) {
    return;
  }

  static uint8_t prev_keys[HID_KEYBOARD_KEY_MAX] = {0};
  key_event_t key_event;

  for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {

    // key has been released verification
    if (prev_keys[i] > HID_KEY_ERROR_UNDEFINED &&
        !key_found(kb_report->key, prev_keys[i], HID_KEYBOARD_KEY_MAX)) {
      key_event.key_code = prev_keys[i];
      key_event.modifier = 0;
      key_event.state = key_event.KEY_STATE_RELEASED;
      key_event_callback(&key_event);
    }

    // key has been pressed verification
    if (kb_report->key[i] > HID_KEY_ERROR_UNDEFINED &&
        !key_found(prev_keys, kb_report->key[i], HID_KEYBOARD_KEY_MAX)) {
      key_event.key_code = kb_report->key[i];
      key_event.modifier = kb_report->modifier.val;
      key_event.state = key_event.KEY_STATE_PRESSED;
      key_event_callback(&key_event);
    }
  }

  memcpy(prev_keys, &kb_report->key, HID_KEYBOARD_KEY_MAX);
}

/**
 * @brief USB HID Host Mouse Interface report callback handler
 *
 * @param[in] data    Pointer to input report data buffer
 * @param[in] length  Length of input report data buffer
 */
static void hid_host_mouse_report_callback(const uint8_t *const data,
                                           const int length) {
  hid_mouse_input_report_t *mouse_report =
      (hid_mouse_input_report_t *)data;

  if (length < sizeof(hid_mouse_input_report_t)) {
    return;
  }

  static int x_pos = 0;
  static int y_pos = 0;

  // Calculate absolute position from displacement
  x_pos += mouse_report->x_displacement;
  y_pos += mouse_report->y_displacement;

  hid_print_new_device_report_header(HID_PROTOCOL_MOUSE);

  printf("X: %06d\tY: %06d\t|%c|%c|%c|\t%d\r", x_pos, y_pos,
         (mouse_report->buttons.button1 ? 'o' : ' '),
         (mouse_report->buttons.button3 ? 'o' : ' '),
         (mouse_report->buttons.button2 ? 'o' : ' '),
         mouse_report->scroll_wheel);
  fflush(stdout);

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

/**
 * @brief USB HID Host Generic Interface report callback handler
 *
 * 'generic' means anything else than mouse or keyboard
 *
 * @param[in] data    Pointer to input report data buffer
 * @param[in] length  Length of input report data buffer
 */
static void hid_host_generic_report_callback(const uint8_t *const data,
                                             const int length) {
  hid_print_new_device_report_header(HID_PROTOCOL_NONE);
  for (int i = 0; i < length; i++) {
    printf("%02X", data[i]);
  }
  putchar('\r');
  putchar('\n');
  fflush(stdout);
}

/**
 * @brief USB HID Host interface callback
 *
 * @param[in] hid_device_handle  HID Device handle
 * @param[in] event              HID Host interface event
 * @param[in] arg                Pointer to arguments, does not used
 */
void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                 const hid_host_interface_event_t event,
                                 void *arg) {
  uint8_t data[64] = {0};
  size_t data_length = 0;
  hid_host_dev_params_t dev_params;
  ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

  switch (event) {
  case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
    ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(
        hid_device_handle, data, 64, &data_length));

    if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
      if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
        hid_host_keyboard_report_callback(data, data_length);
      } else if (HID_PROTOCOL_MOUSE == dev_params.proto) {
        hid_host_mouse_report_callback(data, data_length);
      }
    } else {
      hid_host_generic_report_callback(data, data_length);
    }

    break;
  case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "HID Device, protocol '%s' DISCONNECTED",
             hid_proto_name_str[dev_params.proto]);
    ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
    break;
  case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
    ESP_LOGI(TAG, "HID Device, protocol '%s' TRANSFER_ERROR",
             hid_proto_name_str[dev_params.proto]);
    break;
  default:
    ESP_LOGE(TAG, "HID Device, protocol '%s' Unhandled event",
             hid_proto_name_str[dev_params.proto]);
    break;
  }
}

/**
 * @brief USB HID Host Device event
 *
 * @param[in] hid_device_handle  HID Device handle
 * @param[in] event              HID Host Device event
 * @param[in] arg                Pointer to arguments, does not used
 */
void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                           const hid_host_driver_event_t event, void *arg) {
  hid_host_dev_params_t dev_params;
  ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));
  const hid_host_device_config_t dev_config = {
      .callback = hid_host_interface_callback, .callback_arg = NULL};


  switch (event) {
  case HID_HOST_DRIVER_EVENT_CONNECTED: {
    ESP_LOGI(TAG, "HID Device, protocol '%s' CONNECTED",
             hid_proto_name_str[dev_params.proto]);
    ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));


    if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
      if (HID_PROTOCOL_MOUSE == dev_params.proto) {
        ESP_LOGI(TAG, "Setting mouse to REPORT protocol mode");
        ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle,
                                                        HID_REPORT_PROTOCOL_REPORT));
        digitalWrite(LED_BUILTIN,LOW);
      }
      if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
        ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
        //        ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle,HID_REPORT_PROTOCOL_BOOT));
      }
    }

    ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
      if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
          ESP_LOGI(TAG, "Keyboard connected, turning on numpad LED");
          uint8_t led = 1;  // NumLock ON
          esp_err_t err = hid_class_request_set_report(
              hid_device_handle,
              HID_REPORT_TYPE_OUTPUT,
              0,
              &led,
              sizeof(led)
          );
          digitalWrite(LED_BUILTIN,LOW);
          ESP_LOGI(TAG, "SET_REPORT returned %s", esp_err_to_name(err));
      }
    }
    break;
  default:
    break;
  }
}

/**
 * @brief Start USB Host install and handle common USB host library events while
 * app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_lib_task(void *arg) {
  const usb_host_config_t host_config = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };

  ESP_ERROR_CHECK(usb_host_install(&host_config));
  xTaskNotifyGive((TaskHandle_t)arg);

  while (true) {
    uint32_t event_flags;
    usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

    // Release devices once all clients has deregistered
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
      ESP_LOGI(TAG, "USB Event flags: NO_CLIENTS");
    }
    // All devices were removed
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
      ESP_LOGI(TAG, "USB Event flags: ALL_FREE");
      digitalWrite(LED_BUILTIN,HIGH);
    }
  }
  // Clean up USB Host
  vTaskDelay(10); // Short delay to allow clients clean-up
  ESP_ERROR_CHECK(usb_host_uninstall());
  vTaskDelete(NULL);
}

/**
 * @brief HID Host main task
 *
 * Creates queue and get new event from the queue
 *
 * @param[in] pvParameters Not used
 */
void hid_host_task(void *pvParameters) {
  hid_host_event_queue_t evt_queue;
  // Create queue
  hid_host_event_queue = xQueueCreate(10, sizeof(hid_host_event_queue_t));

  // Wait queue
  while (!user_shutdown) {
    if (xQueueReceive(hid_host_event_queue, &evt_queue, pdMS_TO_TICKS(50))) {
      hid_host_device_event(evt_queue.hid_device_handle, evt_queue.event,
                            evt_queue.arg);
    }
  }

  xQueueReset(hid_host_event_queue);
  vQueueDelete(hid_host_event_queue);
  vTaskDelete(NULL);
}

/**
 * @brief HID Host Device callback
 *
 * Puts new HID Device event to the queue
 *
 * @param[in] hid_device_handle HID Device handle
 * @param[in] event             HID Device event
 * @param[in] arg               Not used
 */
void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
                              const hid_host_driver_event_t event, void *arg) {
  const hid_host_event_queue_t evt_queue = {
      .hid_device_handle = hid_device_handle, .event = event, .arg = arg};
  xQueueSend(hid_host_event_queue, &evt_queue, 0);
}

void app_main(void) {
  BaseType_t task_created;
  ESP_LOGI(TAG, "HID Host example");

  /*
   * Create usb_lib_task to:
   * - initialize USB Host library
   * - Handle USB Host events while APP pin in in HIGH state
   */
  task_created =
      xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096,
                              xTaskGetCurrentTaskHandle(), 2, NULL, 0);
  assert(task_created == pdTRUE);

  // Wait for notification from usb_lib_task to proceed
  ulTaskNotifyTake(false, 1000);

  /*
   * HID host driver configuration
   * - create background task for handling low level event inside the HID driver
   * - provide the device callback to get new HID Device connection event
   */
  const hid_host_driver_config_t hid_host_driver_config = {
      .create_background_task = true,
      .task_priority = 5,
      .stack_size = 4096,
      .core_id = 0,
      .callback = hid_host_device_callback,
      .callback_arg = NULL};

  ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

  // Task is working until the devices are gone (while 'user_shutdown' if false)
  user_shutdown = false;

  /*
   * Create HID Host task process for handle events
   * IMPORTANT: Task is necessary here while there is no possibility to interact
   * with USB device from the callback.
   */
  task_created =
      xTaskCreate(&hid_host_task, "hid_task", 4 * 1024, NULL, 2, NULL);
  assert(task_created == pdTRUE);
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
    bleMouse.begin();
    //use internal button & LED, switch off LED
    pinMode(GPIO_NUM_0,INPUT_PULLUP);
    pinMode(LED_BUILTIN,OUTPUT);
    digitalWrite(LED_BUILTIN,HIGH);
    //start main USB/HID task
    app_main(); 
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

  delay(20);
}
