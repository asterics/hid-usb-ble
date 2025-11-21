/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "usb/usb_host.h"

#include <Arduino.h>
#include <esp_log.h>

#include "hid_usage_keyboard.h"
#include "hid_usage_mouse.h"

#include "usb_hid_host.h"
#include "usb_hid_keyboard.h"
#include "usb_hid_mouse.h"
#include "usb_hid_joystick.h"

static const char* TAG = "usb-hid-host";
QueueHandle_t hid_host_event_queue;
bool user_shutdown = false;
bool addDelayDuringEnumeration = true;     // TBD: this is a workaround for some devices that need delay during enumeration
                                           // see issue: https://github.com/espressif/esp-idf/issues/

// Global callback function pointer
static hidData_callback_t registered_hidData_callback = NULL;

hidData_callback_t * get_registered_hidData_callback() {
    return &registered_hidData_callback;
}


/**
 * Extract integer value of size_bits starting at bit_offset from a HID report 
 * (LSB = bit 0 of data[0])
 */
int32_t hid_extract_int(const uint8_t* data, int data_bytes,
                               int bit_offset, int size_bits, bool is_signed) {
    if (size_bits <= 0 || size_bits > 32) return 0;
    if (bit_offset < 0 || bit_offset + size_bits > data_bytes * 8) return 0;

    int start_byte = bit_offset / 8;
    int start_bit = bit_offset % 8;

    // Read up to 5 bytes into a 64-bit temp (enough for 32 bits at any bit
    // offset)
    uint64_t tmp = 0;
    int needed_bytes = (start_bit + size_bits + 7) / 8;
    for (int i = 0; i < needed_bytes; ++i) {
        tmp |= (uint64_t)data[start_byte + i] << (8 * i);
    }

    tmp >>= start_bit;
    uint32_t val =
        (uint32_t)(tmp &
                   ((size_bits == 32) ? 0xFFFFFFFFu : ((1u << size_bits) - 1)));

    if (is_signed && size_bits < 32 && (val & (1u << (size_bits - 1)))) {
        val |= ~((1u << size_bits) - 1);  // sign extend
    }

    return (int32_t)val;
}



/**
 * @brief Makes new line depending on report output protocol type
 *
 * @param[in] proto Current protocol to output
 */
void hid_print_new_device_report_header(hid_protocol_t proto) {
    static hid_protocol_t prev_proto_output = HID_PROTOCOL_MAX;

    if (prev_proto_output != proto) {
        prev_proto_output = proto;
        printf("\r\n");
        if (proto == HID_PROTOCOL_MOUSE) {
            printf("Mouse\r\n");
        } else if (proto == HID_PROTOCOL_KEYBOARD) {
            printf("Keyboard\r\n");
        } else if (proto == HID_PROTOCOL_JOYSTICK) {
            printf("Joystick/Gamepad\r\n");
        } else {
            printf("Generic\r\n");
        }
        fflush(stdout);
    }
}


/**
 * @brief Register a callback function to be called when hid data is updated
 *
 * @param[in] callback Pointer to callback function that accepts unified_hidData_t*
 */
void register_hidData_callback(hidData_callback_t callback) {
    *get_registered_hidData_callback() = callback;
    ESP_LOGI(TAG, "HidData callback %s",callback ? "registered" : "unregistered");
}

/**
 * @brief HID Host event
 *
 * This event is used for delivering the HID Host event from callback to a task.
 */
typedef struct {
    hid_host_device_handle_t hid_device_handle;
    hid_host_driver_event_t event;
    void* arg;
} hid_host_event_queue_t;

/**
 * @brief HID Protocol string names
 */
static const char* hid_proto_name_str[] = {"NONE", "KEYBOARD", "MOUSE"};


/**
 * @brief USB HID Host interface callback
 *
 * @param[in] hid_device_handle  HID Device handle
 * @param[in] event              HID Host interface event
 * @param[in] arg                Pointer to arguments, does not used
 */
void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                 const hid_host_interface_event_t event,
                                 void* arg) {
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
                    hid_print_new_device_report_header(HID_PROTOCOL_KEYBOARD);
                    hid_host_keyboard_report_callback(data, data_length);
                } else if (HID_PROTOCOL_MOUSE == dev_params.proto) {
                    hid_print_new_device_report_header(HID_PROTOCOL_MOUSE);
                    hid_host_mouse_report_callback(data, data_length);
                }
            } else {
                // try joystick report callback first
                if (hid_host_joystick_report_callback(data, data_length)){
                    hid_print_new_device_report_header((hid_protocol_t)HID_PROTOCOL_JOYSTICK);
                } else {
                    // Fallback: if no joystick report handled, just hex-dump the generic report
                    hid_print_new_device_report_header(HID_PROTOCOL_NONE);
                    for (int i = 0; i < data_length; i++) {
                        printf("%02X", data[i]);
                    }
                    putchar('\n');
                    fflush(stdout);
                }
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
 * @brief HID Host Device event
 *
 * @param[in] hid_device_handle  HID Device handle
 * @param[in] event              HID Host Device event
 * @param[in] arg                Pointer to arguments, does not used
 */
void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                           const hid_host_driver_event_t event, void* arg) {
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));
    const hid_host_device_config_t dev_config = {
        .callback = hid_host_interface_callback, .callback_arg = NULL};

    switch (event) {
        case HID_HOST_DRIVER_EVENT_CONNECTED: {
            addDelayDuringEnumeration = false; // disable delay after first device connected
            ESP_LOGI(TAG, "HID Device, protocol '%s' CONNECTED",
                     hid_proto_name_str[dev_params.proto]);
            ESP_ERROR_CHECK(
                hid_host_device_open(hid_device_handle, &dev_config));

            if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
                if (HID_PROTOCOL_MOUSE == dev_params.proto) {
                    ESP_LOGI(TAG,"Mouse device detected, parsing report descriptor...");

                    // Try to get and parse the HID report descriptor
                    size_t report_desc_len = 0;
                    uint8_t* report_desc = hid_host_get_report_descriptor(
                        hid_device_handle, &report_desc_len);

                    bool use_boot_protocol = true;

                    if (report_desc != NULL && report_desc_len > 0) {
                        ESP_LOGI(TAG, "Got report descriptor, length: %zu",
                                 report_desc_len);

                        if (parse_mouse_report_descriptor(
                                report_desc, report_desc_len, get_mouse_format())) {
                            ESP_LOGI(TAG, "Successfully parsed mouse report descriptor, using report protocol");
                            ESP_ERROR_CHECK(hid_class_request_set_protocol(
                                hid_device_handle, HID_REPORT_PROTOCOL_REPORT));
                            use_boot_protocol = false;
                        } else {
                            ESP_LOGW(TAG, "Failed to parse mouse report descriptor");
                        }

                        // free(report_desc); // Uncomment if needed
                    } else {
                        ESP_LOGW(TAG, "Could not get report descriptor (NULL or length=0)");
                    }

                    if (use_boot_protocol) {
                        ESP_LOGI(TAG, "Falling back to boot protocol for mouse");
                        ESP_ERROR_CHECK(hid_class_request_set_protocol(
                            hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
                        get_mouse_format()->is_valid =
                            false;  // Use boot protocol parsing
                    }
                }

                if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                    ESP_ERROR_CHECK(
                        hid_class_request_set_idle(hid_device_handle, 0, 0));
                    //        ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle,HID_REPORT_PROTOCOL_BOOT));
                }
            } else {
                // Non-boot HID: attempt to treat as joystick/gamepad
                ESP_LOGI(TAG, "Non-boot HID device, checking for joystick/gamepad");
                size_t report_desc_len = 0;
                uint8_t* report_desc = hid_host_get_report_descriptor(
                    hid_device_handle, &report_desc_len);

                if (report_desc != NULL && report_desc_len > 0) {
                    if (parse_joystick_report_descriptor(
                            report_desc, report_desc_len, get_joystick_format())) {
                        ESP_LOGI(TAG, "Joystick/Gamepad descriptor parsed; generic reports will be mapped to mouse");
                        // Joystick usually uses report protocol by default
                        // If needed:
                        // ESP_ERROR_CHECK(hid_class_request_set_protocol(
                        //     hid_device_handle, HID_REPORT_PROTOCOL_REPORT));
                    } else {
                        ESP_LOGI(TAG, "Non-boot HID is not recognized as joystick/gamepad");
                        get_joystick_format()->is_valid = false;
                    }
                    // free(report_desc); // Uncomment if API requires
                } else {
                    ESP_LOGW(TAG, "Could not get report descriptor for non-boot HID");
                }
            }

            ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
            if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
                ESP_LOGI(TAG, "Keyboard connected, turning on numpad LED");
                uint8_t led = 1;  // NumLock ON
                esp_err_t err = hid_class_request_set_report(
                    hid_device_handle, HID_REPORT_TYPE_OUTPUT, 0, &led,
                    sizeof(led));
                digitalWrite(LED_BUILTIN, LOW);
                ESP_LOGI(TAG, "SET_REPORT returned %s", esp_err_to_name(err));
            }
        } break;
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
static void usb_lib_task(void* arg) {
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (addDelayDuringEnumeration)
            vTaskDelay(pdMS_TO_TICKS(10)); 

        // Release devices once all clients has deregistered
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
            ESP_LOGI(TAG, "USB Event flags: NO_CLIENTS");
        }
        // All devices were removed
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB Event flags: ALL_FREE");
            addDelayDuringEnumeration = true; // re-enable delay for next enumeration
        }
    }
    // Clean up USB Host
    vTaskDelay(10);  // Short delay to allow clients clean-up
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
void hid_host_task(void* pvParameters) {
    hid_host_event_queue_t evt_queue;
    // Create queue
    hid_host_event_queue = xQueueCreate(10, sizeof(hid_host_event_queue_t));

    // Wait queue
    while (!user_shutdown) {
        if (xQueueReceive(hid_host_event_queue, &evt_queue,
                          pdMS_TO_TICKS(50))) {
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
                              const hid_host_driver_event_t event, void* arg) {
    const hid_host_event_queue_t evt_queue = {
        .hid_device_handle = hid_device_handle, .event = event, .arg = arg};
    xQueueSend(hid_host_event_queue, &evt_queue, 0);
}

void start_usb_host(void) {
    BaseType_t task_created;
    ESP_LOGI(TAG, "USB HID Host starting ...");

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
     * - create background task for handling low level event inside the HID
     * driver
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

    // Task is working until the devices are gone (while 'user_shutdown' if
    // false)
    user_shutdown = false;

    /*
     * Create HID Host task process for handle events
     * IMPORTANT: Task is necessary here while there is no possibility to
     * interact with USB device from the callback.
     */
    task_created =
        xTaskCreate(&hid_host_task, "hid_task", 4 * 1024, NULL, 2, NULL);
    assert(task_created == pdTRUE);
}