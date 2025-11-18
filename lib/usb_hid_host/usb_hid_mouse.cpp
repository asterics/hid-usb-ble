#include <Arduino.h>
#include <esp_log.h>
#include "usb_hid_mouse.h"

#include "hid_usage_mouse.h"
#include "usb_hid_host.h"


static const char* TAG = "usb-hid-mouse";
mouse_report_format_t mouse_format = {0};
mouse_report_format_t* get_mouse_format() { return &mouse_format; }


bool parse_mouse_report_descriptor(const uint8_t* desc, size_t desc_len,
                                          mouse_report_format_t* fmt) {
    memset(fmt, 0, sizeof(*fmt));

    int bit_offset = 0;
    int report_size = 0;   // bits
    int report_count = 0;  // fields
    int report_id = 0;  // report id
    uint16_t usage_page = 0;

    // local state
    uint16_t usages[16];
    int usage_count = 0;
    uint16_t usage_min = 0;
    uint16_t usage_max = 0;
    bool have_usage_range = false;

    bool found_x = false, found_y = false, found_buttons = false,
         found_wheel = false;

    ESP_LOGI(TAG, "Parsing HID Mouse report descriptor (%u bytes)",
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
            // Long item – not used in typical mouse descriptors, skip safely
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

        if (size == 3) size = 4;  // HID quirk

        uint32_t data = 0;
        for (uint8_t n = 0; n < size && i < desc_len; ++n) {
            data |= (uint32_t)desc[i++] << (8 * n);
        }

        switch (type) {
            case 0:                 // Main
                if (tag == 0x08) {  // Input
                    ESP_LOGD(TAG,
                             "Input: usage_page=0x%X, bit_offset=%d, "
                             "report_size=%d, report_count=%d, usage_count=%d",
                             usage_page, bit_offset, report_size, report_count,
                             usage_count);

                    // BUTTONS
                    if (usage_page == 0x09) {  // Button page
                        if (!found_buttons && report_count > 0 &&
                            report_size == 1) {
                            fmt->buttons_bit_offset = bit_offset;
                            fmt->buttons_bits =
                                report_count;  // could be > physical buttons
                            fmt->button_count = report_count;
                            found_buttons = true;
                            ESP_LOGI(TAG, "Buttons: bit_offset=%d, bits=%d",
                                     fmt->buttons_bit_offset,
                                     fmt->buttons_bits);
                        }
                    }
                    // AXES / WHEELS
                    else if (usage_page == 0x01) {  // Generic Desktop
                        int field_bit = bit_offset;

                        // 1) explicit usages
                        for (int u = 0; u < usage_count; ++u) {
                            uint16_t uval = usages[u];

                            if (!found_x && uval == 0x30) {  // X
                                fmt->x_bit_offset = field_bit;
                                fmt->x_bits = report_size;
                                fmt->x_signed =
                                    (data & 0x02) !=
                                    0;  // Variable bit => signed for rel axes
                                found_x = true;
                                ESP_LOGI(TAG,
                                         "X: bit_offset=%d, bits=%d, signed=%d",
                                         fmt->x_bit_offset, fmt->x_bits,
                                         fmt->x_signed);
                            } else if (!found_y && uval == 0x31) {  // Y
                                fmt->y_bit_offset = field_bit;
                                fmt->y_bits = report_size;
                                fmt->y_signed = (data & 0x02) != 0;
                                found_y = true;
                                ESP_LOGI(TAG,
                                         "Y: bit_offset=%d, bits=%d, signed=%d",
                                         fmt->y_bit_offset, fmt->y_bits,
                                         fmt->y_signed);
                            } else if (!found_wheel && uval == 0x38) {  // Wheel
                                fmt->wheel_bit_offset = field_bit;
                                fmt->wheel_bits = report_size;
                                fmt->wheel_signed = (data & 0x02) != 0;
                                found_wheel = true;
                                ESP_LOGI(
                                    TAG,
                                    "Wheel: bit_offset=%d, bits=%d, signed=%d",
                                    fmt->wheel_bit_offset, fmt->wheel_bits,
                                    fmt->wheel_signed);
                            }

                            field_bit += report_size;
                        }

                        // 2) usage range (X..Y etc.)
                        if (usage_count == 0 && have_usage_range) {
                            uint16_t u = usage_min;
                            for (int idx = 0; idx < report_count; ++idx, ++u) {
                                int obit = bit_offset + idx * report_size;
                                if (!found_x && u == 0x30) {
                                    fmt->x_bit_offset = obit;
                                    fmt->x_bits = report_size;
                                    fmt->x_signed = (data & 0x02) != 0;
                                    found_x = true;
                                    ESP_LOGI(TAG,
                                             "X(range): bit_offset=%d, "
                                             "bits=%d, signed=%d",
                                             fmt->x_bit_offset, fmt->x_bits,
                                             fmt->x_signed);
                                } else if (!found_y && u == 0x31) {
                                    fmt->y_bit_offset = obit;
                                    fmt->y_bits = report_size;
                                    fmt->y_signed = (data & 0x02) != 0;
                                    found_y = true;
                                    ESP_LOGI(TAG,
                                             "Y(range): bit_offset=%d, "
                                             "bits=%d, signed=%d",
                                             fmt->y_bit_offset, fmt->y_bits,
                                             fmt->y_signed);
                                } else if (!found_wheel && u == 0x38) {
                                    fmt->wheel_bit_offset = obit;
                                    fmt->wheel_bits = report_size;
                                    fmt->wheel_signed = (data & 0x02) != 0;
                                    found_wheel = true;
                                    ESP_LOGI(TAG,
                                             "Wheel(range): bit_offset=%d, "
                                             "bits=%d, signed=%d",
                                             fmt->wheel_bit_offset,
                                             fmt->wheel_bits,
                                             fmt->wheel_signed);
                                }
                            }
                        }
                    }

                    bit_offset += report_size * report_count;

                    // reset locals
                    usage_count = 0;
                    usage_min = 0;
                    usage_max = 0;
                    have_usage_range = false;
                    break;
                }
                // other main items (Output, Feature, Collection, End
                // Collection) are ignored for format
                break;

            case 1:  // Global
                switch (tag) {
                    case 0x0:  // Usage Page
                        usage_page = (uint16_t)data;
                        break;
                    case 0x7:  // Report Size
                        report_size = (int)data;
                        break;
                    case 0x9:  // Report Count
                        report_count = (int)data;
                        break;
                    case 0x8:  // Report ID
                        //because we want to save only the reportID of the mouse usage page:
                        //if we already found a valid set of buttons/x/y/wheel, we can assume
                        //we don't want any following report ids saved
                        if(!(found_x && found_y && found_buttons)) {
                            report_id = (int)data;
                        }
                        break;
                    default:
                        break;
                }
                break;

            case 2:  // Local
                switch (tag) {
                    case 0x0:  // Usage
                        if (usage_count <
                            (int)(sizeof(usages) / sizeof(usages[0]))) {
                            usages[usage_count++] = (uint16_t)data;
                        }
                        break;
                    case 0x1:  // Usage Min
                        usage_min = (uint16_t)data;
                        have_usage_range = true;
                        break;
                    case 0x2:  // Usage Max
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
    fmt->reportid = report_id;
    //offset all fields by 1Byte if report id is found:
    if(fmt->reportid > 0) {
        fmt->buttons_bit_offset += 8;
        fmt->x_bit_offset += 8;
        fmt->y_bit_offset += 8;
        fmt->wheel_bit_offset += 8;
    }
    ESP_LOGI(TAG,
             "Parsed mouse format: valid=%d, reportid=%d, btn_off=%d bits, x_off=%d bits, "
             "y_off=%d bits, wheel_off=%d bits",
             fmt->is_valid, fmt->reportid, fmt->buttons_bit_offset, fmt->x_bit_offset,
             fmt->y_bit_offset, fmt->wheel_bit_offset);
    return fmt->is_valid;
}


bool parse_custom_mouse_report(const uint8_t* data, int length,
                                      unified_mouseReport_t* out) {
    if (!mouse_format.is_valid) return false;

    //check if report id matches in mouse_format
    uint8_t reportid = hid_extract_int(data,1,0,8,false);

    if(mouse_format.reportid != 0 && mouse_format.reportid != reportid) {
        ESP_LOGE(TAG,"Wrong report ID, expected %d, got %d",mouse_format.reportid,reportid);
        return false;
    }

    memset(out, 0, sizeof(*out));

    // Buttons: just look at first 8 bits starting at buttons_bit_offset.
    int32_t btns =
        hid_extract_int(data, length, mouse_format.buttons_bit_offset,
                        mouse_format.buttons_bits, false);
    out->buttons.button1 = (btns & 0x01) != 0;
    out->buttons.button2 = (btns & 0x02) != 0;
    out->buttons.button3 = (btns & 0x04) != 0;

    // X, Y, Wheel
    out->x_displacement =
        (int16_t)hid_extract_int(data, length, mouse_format.x_bit_offset,
                                 mouse_format.x_bits, mouse_format.x_signed);
    out->y_displacement =
        (int16_t)hid_extract_int(data, length, mouse_format.y_bit_offset,
                                 mouse_format.y_bits, mouse_format.y_signed);

    if (mouse_format.wheel_bits > 0) {
        out->scroll_wheel = (int8_t)hid_extract_int(
            data, length, mouse_format.wheel_bit_offset,
            mouse_format.wheel_bits, mouse_format.wheel_signed);
    } else {
        out->scroll_wheel = 0;
    }

    ESP_LOGD(TAG, "Parsed report: btns=0x%X X=%d Y=%d Wheel=%d", (unsigned)btns,
             out->x_displacement, out->y_displacement, out->scroll_wheel);
    return true;
}


/**
 * @brief USB HID Host Mouse Interface report callback handler
 *
 * @param[in] data    Pointer to input report data buffer
 * @param[in] length  Length of input report data buffer
 */
void hid_host_mouse_report_callback(const uint8_t* const data,
                                           const int length) {
    unified_mouseReport_t unified_mouseReport;
    bool parsed = false;

    // Try to parse using custom descriptor format first
    if (mouse_format.is_valid) {
        parsed = parse_custom_mouse_report(data, length, &unified_mouseReport);
    }

    // Fall back to boot protocol format
    if (!parsed && length >= sizeof(hid_mouse_input_report_boot_t)) {
        hid_mouse_input_report_boot_t* boot_report =
            (hid_mouse_input_report_boot_t*)data;

        // Convert boot format to standard format
        unified_mouseReport.x_displacement = boot_report->x_displacement;
        unified_mouseReport.y_displacement = boot_report->y_displacement;
        unified_mouseReport.buttons.button1 = boot_report->buttons.button1;
        unified_mouseReport.buttons.button2 = boot_report->buttons.button2;
        unified_mouseReport.buttons.button3 = boot_report->buttons.button3;
        unified_mouseReport.scroll_wheel =
            0;  // Boot protocol doesn't have scroll
        parsed = true;

        ESP_LOGD(TAG, "Using boot protocol fallback");
    }

    if (!parsed) {
        ESP_LOGW(TAG, "Failed to parse mouse report (length=%d)", length);
        return;
    }

    // Call registered callback function if one exists
    if (get_registered_mouse_callback() != NULL) {
        (*get_registered_mouse_callback())(&unified_mouseReport);

    }
}
