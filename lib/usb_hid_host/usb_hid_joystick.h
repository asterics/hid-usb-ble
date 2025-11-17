#pragma once

#include "hid_host.h"

// Structure to store parsed HID joystick/gamepad report format
typedef struct {
    bool is_valid;

    int x_bit_offset;
    int x_bits;
    bool x_signed;

    int y_bit_offset;
    int y_bits;
    bool y_signed;

    int hat_bit_offset;
    int hat_bits;
    bool has_hat;

    int buttons_bit_offset;
    int buttons_bits;
    int button_count;
} joystick_report_format_t;

joystick_report_format_t* get_joystick_format();
bool hid_host_joystick_report_callback(const uint8_t* const data,const int length);
bool parse_joystick_report_descriptor(const uint8_t* desc, size_t desc_len,joystick_report_format_t* fmt);