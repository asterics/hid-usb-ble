#pragma once

#include "hid_host.h"


// Structure to store parsed HID mouse report format
typedef struct {
    bool is_valid;

    // report ID (0 if not used)
    int reportid;

    // All offsets are in *bits*.
    int buttons_bit_offset;
    int buttons_bits;  // usually = button_count
    int button_count;

    int x_bit_offset;
    int x_bits;  // size in bits
    bool x_signed;

    int y_bit_offset;
    int y_bits;
    bool y_signed;

    int wheel_bit_offset;
    int wheel_bits;
    bool wheel_signed;
} mouse_report_format_t;


mouse_report_format_t* get_mouse_format();

void hid_host_mouse_report_callback(const uint8_t* const data, const int length);
bool parse_mouse_report_descriptor(const uint8_t* desc, size_t desc_len, mouse_report_format_t* fmt);