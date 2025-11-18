#pragma once

#include <stdint.h>
#include "hid_host.h"

// Unified report structure to handle mouse-like data from any device
typedef struct {
    union {
        struct {
            uint8_t button1 : 1;
            uint8_t button2 : 1;
            uint8_t button3 : 1;
            uint8_t reserved : 5;
        };
        uint8_t val;
    } buttons;
    int8_t x_displacement;
    int8_t y_displacement;
    int8_t scroll_wheel;
} __attribute__((packed)) unified_mouseReport_t;

// Callback function pointer for applications to receive mouse reports
typedef void (*mouse_report_callback_t)(unified_mouseReport_t* mouse_report);

void register_mouse_report_callback(mouse_report_callback_t callback);
mouse_report_callback_t * get_registered_mouse_callback();

// Declaration for the shared bit extraction utility
int32_t hid_extract_int(const uint8_t* data, int data_bytes, int bit_offset, int size_bits, bool is_signed);

void hid_print_new_device_report_header(hid_protocol_t proto);

void start_usb_host();

