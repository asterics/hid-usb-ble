#include <Arduino.h>
#include <esp_log.h>
#include "usb_hid_joystick.h"

#include "usb_hid_host.h"

static const char* TAG = "usb-hid-joystick";
joystick_report_format_t joystick_format = {0};

joystick_report_format_t* get_joystick_format() { return &joystick_format; }

// Helper to manually sign-extend a value of 'bits' width to 32-bit int
static int32_t sign_extend(int32_t val, int bits) {
    if (bits <= 0 || bits >= 32) return val;
    int32_t m = 1U << (bits - 1);
    return (val ^ m) - m;
}

bool parse_joystick_report_descriptor(const uint8_t* desc, size_t desc_len,joystick_report_format_t* fmt) {
    memset(fmt, 0, sizeof(*fmt));

    int bit_offset = 0;
    int report_size = 0;
    int report_count = 0;
    uint16_t usage_page = 0;
    bool report_id_found = false;
    int32_t logical_min = 0; // Track Logical Minimum to determine signedness

    uint16_t usages[16];
    int usage_count = 0;
    uint16_t usage_min = 0;
    uint16_t usage_max = 0;
    bool have_usage_range = false;

    bool found_x = false, found_y = false, found_buttons = false, found_hat = false;

    ESP_LOGI(TAG, "Parsing HID joystick report descriptor (%u bytes)",
             (unsigned)desc_len);
    ESP_LOGI(TAG, "Raw descriptor:");
    for (size_t i = 0; i < desc_len; i += 16) {
        char line[80];
        int pos = 0;
        for (size_t j = i; j < desc_len && j < i + 16; ++j) {
            pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", desc[j]);
        }
        ESP_LOGI(TAG, "%04u: %s", (unsigned)i, line);
    }

    for (size_t i = 0; i < desc_len;) {
        uint8_t b = desc[i++];

        if ((b & 0xF0) == 0xF0) {
            if (i + 1 >= desc_len) break;
            uint8_t data_len = desc[i++];
            uint8_t long_tag = desc[i++];
            (void)long_tag;
            i += data_len;
            continue;
        }

        uint8_t size = b & 0x03;
        uint8_t type = (b >> 2) & 0x03;
        uint8_t tag = (b >> 4) & 0x0F;

        if (size == 3) size = 4;

        uint32_t data = 0;
        for (uint8_t n = 0; n < size && i < desc_len; ++n) {
            data |= (uint32_t)desc[i++] << (8 * n);
        }

        switch (type) {
            case 0: // Main
                if (tag == 0x08) { // Input
                    ESP_LOGI(TAG, "Processing INPUT: usage_page=0x%02X, usage_count=%d, bit_offset=%d, report_size=%d, report_count=%d",
                             usage_page, usage_count, bit_offset, report_size, report_count);
                    
                    // Debug: print all collected usages
                    for (int dbg = 0; dbg < usage_count; ++dbg) {
                        ESP_LOGI(TAG, "  Collected usage[%d] = 0x%02X", dbg, usages[dbg]);
                    }
                    
                    if (usage_page == 0x01) { // Generic Desktop Page (Axes, Hat)
                        // Start from the CURRENT bit_offset for this Input item
                        int field_bit = bit_offset;
                        ESP_LOGI(TAG, "  Starting field_bit at %d", field_bit);
                        
                        // Process each usage in the order they were declared
                        for (int u_idx = 0; u_idx < usage_count; ++u_idx) {
                            uint16_t uval = usages[u_idx];
                            ESP_LOGI(TAG, "    Processing usage[%d]=0x%02X at field_bit=%d", u_idx, uval, field_bit);
                            
                            // Assign offsets to X and Y on their first occurrence
                            if (!found_x && uval == 0x30) { // X-Axis 
                                fmt->x_bit_offset = field_bit;
                                fmt->x_bits = report_size;
                                fmt->x_signed = (logical_min < 0); // Determine sign based on Logical Min
                                found_x = true;
                                ESP_LOGI(TAG, "Joystick X: bit_offset=%d bits=%d signed=%d", fmt->x_bit_offset, fmt->x_bits, fmt->x_signed);
                            } else if (!found_y && uval == 0x31) { // Y-Axis
                                fmt->y_bit_offset = field_bit;
                                fmt->y_bits = report_size;
                                fmt->y_signed = (logical_min < 0); // Determine sign based on Logical Min
                                found_y = true;
                                ESP_LOGI(TAG, "Joystick Y: bit_offset=%d bits=%d signed=%d", fmt->y_bit_offset, fmt->y_bits, fmt->y_signed);
                            } else if (!found_hat && uval == 0x39) { // Hat Switch
                                fmt->hat_bit_offset = field_bit;
                                fmt->hat_bits = report_size;
                                fmt->hat_logical_min = logical_min; // Capture Logical Min for Hat
                                fmt->has_hat = true;
                                found_hat = true;
                                ESP_LOGI(TAG, "Joystick Hat: bit_offset=%d bits=%d min=%d", fmt->hat_bit_offset, fmt->hat_bits, fmt->hat_logical_min);
                            }
                            // For each usage, advance by report_size bits
                            field_bit += report_size;
                        }
                    } else if (usage_page == 0x09) { // Button Page
                        if (!found_buttons && report_count > 0 && report_size == 1) {
                            fmt->buttons_bit_offset = bit_offset;
                            fmt->buttons_bits = report_count;
                            fmt->button_count = report_count;
                            found_buttons = true;
                            ESP_LOGI(TAG, "Joystick buttons: bit_offset=%d bits=%d", fmt->buttons_bit_offset, fmt->buttons_bits);
                        }
                    }
                    
                    // After processing all usages in this Input item, advance the main bit offset
                    bit_offset += report_size * report_count;
                    
                    // Clear local items for the next Input item
                    usage_count = 0;
                    usage_min = 0;
                    usage_max = 0;
                    have_usage_range = false;
                } else if (tag == 0x0A || tag == 0x0C) { // Collection or End Collection
                    // Clear usages when entering/exiting collections
                    // This prevents collection-level usages from being mixed with field usages
                    usage_count = 0;
                    ESP_LOGD(TAG, "Collection boundary, clearing usage list");
                }
                break;

            case 1: // Global
                switch (tag) {
                    case 0x0: // Usage Page
                        usage_page = (uint16_t)data;
                        break;
                    case 0x1: // Logical Min
                        // Handle sign extension based on size
                        if (size == 1) logical_min = (int8_t)data;
                        else if (size == 2) logical_min = (int16_t)data;
                        else logical_min = (int32_t)data;
                        break;
                    case 0x7: // Report Size
                        report_size = (int)data;
                        break;
                    case 0x8: // Report ID
                        if (!report_id_found) {
                            bit_offset = 8;
                            report_id_found = true;
                            ESP_LOGI(TAG, "Report ID found (%u), setting initial bit_offset to 8", (unsigned)data);
                        }
                        break;
                    case 0x9: // Report Count
                        report_count = (int)data;
                        break;

                    default:
                        break;
                }
                break;

            case 2: // Local
                switch (tag) {
                    case 0x0: // Usage
                        if (usage_count < (int)(sizeof(usages) / sizeof(usages[0]))) {
                            usages[usage_count++] = (uint16_t)data;
                        }
                        break;
                    case 0x1: // Usage Min
                        usage_min = (uint16_t)data;
                        have_usage_range = true;
                        break;
                    case 0x2: // Usage Max
                        usage_max = (uint16_t)data;
                        have_usage_range = true;
                        break;
                    default:
                        break;
                }
                break;
        }
    }

    fmt->is_valid = found_x && found_y && found_buttons;
    ESP_LOGI(TAG,
             "Parsed joystick format: valid=%d, btn_off=%d bits, x_off=%d bits, "
             "y_off=%d bits, hat=%d",
             fmt->is_valid, fmt->buttons_bit_offset, fmt->x_bit_offset,
             fmt->y_bit_offset, fmt->has_hat);
    return fmt->is_valid;
}


/**
 * @brief Parse joystick/gamepad input report into unified hidData report:
 *  first axis maps to x/y displacements, 
 *  first 3 buttons map to buttons 1-3
 *  hat switch up/down maps to scroll wheel
 */
bool parse_joystick_report(const uint8_t* data, int length,
                                  unified_hidData_t* out) {
    if (!joystick_format.is_valid) return false;

    memset(out, 0, sizeof(*out));

    int32_t btns =
        hid_extract_int(data, length, joystick_format.buttons_bit_offset,
                        joystick_format.buttons_bits, false);
    out->buttons.button1 = (btns & 0x01) != 0; // Button 1
    out->buttons.button2 = (btns & 0x02) != 0; // Button 2
    out->buttons.button3 = (btns & 0x04) != 0; // Button 3

    // Extract X and Y axis values as UNSIGNED raw bits first
    // We pass 'false' for is_signed to prevent hid_extract_int from doing sign extension
    int32_t x = hid_extract_int(
        data, length, joystick_format.x_bit_offset, joystick_format.x_bits,
        false);
    int32_t y = hid_extract_int(
        data, length, joystick_format.y_bit_offset, joystick_format.y_bits,
        false);

    // Fix: Ensure correct sign extension for signed values
    if (joystick_format.x_signed) {
        x = sign_extend(x, joystick_format.x_bits);
    } else if (joystick_format.x_bits > 0) {
        // If unsigned, center the value (e.g., 0..1023 -> -512..511)
        x -= (1 << (joystick_format.x_bits - 1));
    }

    if (joystick_format.y_signed) {
        y = sign_extend(y, joystick_format.y_bits);
    } else if (joystick_format.y_bits > 0) {
        y -= (1 << (joystick_format.y_bits - 1));
    }

    // --- Convert Joystick Axis to Mouse Displacement (Dynamic Scaling) ---
    int16_t mouse_x = 0;
    int16_t mouse_y = 0;
    const int MOUSE_MAX_SPEED = 10; // Max mouse displacement per report

    // --- X-Axis Scaling ---
    if (joystick_format.x_bits > 0) {
        int32_t x_max_val = (1 << (joystick_format.x_bits - 1)) - 1;
        if (x_max_val > 0) {
            int32_t x_deadzone = x_max_val / 8; // ~12.5% deadzone
            if (abs(x) > x_deadzone) {
                mouse_x = (int16_t)((float)x / x_max_val * MOUSE_MAX_SPEED);
            }
        }
    }

    // --- Y-Axis Scaling ---
    if (joystick_format.y_bits > 0) {
        int32_t y_max_val = (1 << (joystick_format.y_bits - 1)) - 1;
        if (y_max_val > 0) {
            int32_t y_deadzone = y_max_val / 8; // ~12.5% deadzone
            if (abs(y) > y_deadzone) {
                mouse_y = (int16_t)((float)y / y_max_val * MOUSE_MAX_SPEED);
            }
        }
    }

    out->x_displacement = mouse_x;
    out->y_displacement = mouse_y;
    out->scroll_wheel = 0;

    // --- Hat Switch to Scroll Wheel ---
    if (joystick_format.has_hat) {
        int32_t hat = hid_extract_int(data, length, joystick_format.hat_bit_offset,
                                      joystick_format.hat_bits, false);
        
        // Normalize Hat value to 0..7 range (0=Up, 1=NE, ... 7=NW)
        // Joystick A: Min=0 -> 0..7
        // Joystick B: Min=1 -> 1..8 -> (hat - 1) -> 0..7
        int normalized_hat = hat - joystick_format.hat_logical_min;

        // Check if value is within valid 8-direction range
        if (normalized_hat >= 0 && normalized_hat <= 7) {
            if (normalized_hat == 7 || normalized_hat == 0 || normalized_hat == 1) {
                // Up or up-diagonal
                out->scroll_wheel = 1;
            } else if (normalized_hat == 3 || normalized_hat == 4 || normalized_hat == 5) {
                // Down or down-diagonal
                out->scroll_wheel = -1;
            }
        }
    }

    ESP_LOGD(TAG, "Joystick->Mouse: btns=0x%X X=%d Y=%d Wheel=%d", (unsigned)btns,
             out->x_displacement, out->y_displacement, out->scroll_wheel);
    return true;
}

/**
 * @brief USB HID Host Joystick Interface report callback handler
 *
 * note that this is also called on 'generic' input reports,
 * (anything else than mouse or keyboard) so we need to check if it's 
 * really a joystick. If not, return false to allow other handlers to try.
 *
 * @param[in] data    Pointer to input report data buffer
 * @param[in] length  Length of input report data buffer
 */
bool hid_host_joystick_report_callback(const uint8_t* const data,
                                             const int length) {
    // try to interpret HID report as joystick
    if (joystick_format.is_valid) {
        unified_hidData_t unified_hidData;
        if (parse_joystick_report(data, length, &unified_hidData)) {
            if (get_registered_hidData_callback() != NULL) {
                (*get_registered_hidData_callback())(&unified_hidData);
            }
            return true;  // joystick report handled
        }
    }
    // if no joystick detected, return false
    return(false);
}