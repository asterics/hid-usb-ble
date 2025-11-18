#pragma once

#include <stdint.h>
#include "hid_host.h"

// Unified report structure to handle a selected set of data from supported hid devices
typedef struct {
    union {
        struct {
            uint8_t button1 : 1;
            uint8_t button2 : 1;
            uint8_t button3 : 1;
            uint8_t button4 : 1;
            uint8_t button5 : 1;
            uint8_t button6 : 1;
            uint8_t button7 : 1;
            uint8_t button8 : 1;
        };
        uint8_t val;
    } buttons;
    int16_t x_displacement;
    int16_t y_displacement;
    int8_t scroll_wheel;
} __attribute__((packed)) unified_hidData_t;

// Callback function pointer for applications to receive unified hid data reports
typedef void (*hidData_callback_t)(unified_hidData_t* hidData);

void register_hidData_callback(hidData_callback_t callback);
hidData_callback_t * get_registered_hidData_callback();

// Declaration for the shared bit extraction utility
int32_t hid_extract_int(const uint8_t* data, int data_bytes, int bit_offset, int size_bits, bool is_signed);

void hid_print_new_device_report_header(hid_protocol_t proto);

void start_usb_host();

