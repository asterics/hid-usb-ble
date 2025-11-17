/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "usb/usb_host.h"

#include <Arduino.h>
#include <esp_log.h>

#include "hid_host.h"
#include "hid_usage_keyboard.h"
#include "hid_usage_mouse.h"
#include "usb_host.h"

// Global callback function pointer
static mouse_report_callback_t registered_mouse_callback = NULL;

static const char* TAG = "usb-hid-host";
QueueHandle_t hid_host_event_queue;
bool user_shutdown = false;

// Structure to store parsed HID mouse report format
typedef struct mouse_report_format {
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

static mouse_report_format_t mouse_format = {0};

/**
 * Extract integer value of size_bits starting at bit_offset (LSB = bit 0 of
 * data[0])
 */
static int32_t hid_extract_int(const uint8_t* data, int data_bytes,
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

static bool parse_mouse_report_descriptor(const uint8_t* desc, size_t desc_len,
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

    ESP_LOGI(TAG, "Parsing HID report descriptor (%u bytes)",
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

/**
 * @brief Extract signed integer from byte array with bit-level precision
 */
static int32_t extract_signed_int(const uint8_t* data, int bit_offset,
                                  int size_bits, bool is_signed) {
    if (size_bits <= 0 || size_bits > 32) {
        return 0;
    }

    // Use a 64-bit integer to safely read up to 32 bits from a non-aligned
    // position
    uint64_t temp = 0;
    int byte_offset = bit_offset / 8;

    // Read enough bytes to contain the entire field
    for (int i = 0; i < 8; ++i) {
        if (byte_offset + i <
            64) {  // Prevent reading past a reasonable report size
            temp |= (uint64_t)data[byte_offset + i] << (i * 8);
        }
    }

    // Shift to remove bits before our field
    temp >>= (bit_offset % 8);

    // Create a mask for the desired number of bits
    uint64_t mask = (1ULL << size_bits) - 1;
    uint32_t val = temp & mask;

    // Sign extend if the value is negative
    if (is_signed && (val & (1UL << (size_bits - 1)))) {
        // Fill the upper bits with 1s
        val |= (0xFFFFFFFFUL << size_bits);
    }

    return (int32_t)val;
}

static bool parse_custom_mouse_report(const uint8_t* data, int length,
                                      unified_mouseReport_t* out) {
    if (!mouse_format.is_valid) return false;

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
 * @brief Register a callback function to be called when mouse report is updated
 *
 * @param[in] callback Pointer to callback function that accepts
 * unified_mouseReport_t*
 */
void register_mouse_report_callback(mouse_report_callback_t callback) {
    registered_mouse_callback = callback;
    ESP_LOGI(TAG, "Mouse report callback %s",
             callback ? "registered" : "unregistered");
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
 * @brief Key event
 */
typedef struct {
    enum key_state {
        KEY_STATE_PRESSED = 0x00,
        KEY_STATE_RELEASED = 0x01
    } state;
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
    /* HID_KEY_SHARP           */  // HOTFIX: for NonUS Keyboards repeat
                                   // HID_KEY_BACK_SLASH
    {';', ':'},                    /* HID_KEY_COLON           */
    {'\'', '"'},                   /* HID_KEY_QUOTE           */
    {'`', '~'},                    /* HID_KEY_TILDE           */
    {',', '<'},                    /* HID_KEY_LESS            */
    {'.', '>'},                    /* HID_KEY_GREATER         */
    {'/', '?'}                     /* HID_KEY_SLASH           */
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
                                         unsigned char* key_char) {
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
#endif  // KEYBOARD_ENTER_LF_EXTEND
        fflush(stdout);
    }
}

/**
 * @brief Key Event. Key event with the key code, state and modifier.
 *
 * @param[in] key_event Pointer to Key Event structure
 *
 */
static void key_event_callback(key_event_t* key_event) {
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
static inline bool key_found(const uint8_t* const src, uint8_t key,
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
static void hid_host_keyboard_report_callback(const uint8_t* const data,
                                              const int length) {
    hid_keyboard_input_report_boot_t* kb_report =
        (hid_keyboard_input_report_boot_t*)data;

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
static void hid_host_mouse_report_callback(const uint8_t* const data,
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

#ifdef OUTPUT_USB_MOUSE_REPORT_DEBUG_MESSAGES
    hid_print_new_device_report_header(HID_PROTOCOL_MOUSE);

    printf("X: %06d\tY: %06d\t|%c|%c|%c|\t%d\n",
           unified_mouseReport.x_displacement,
           unified_mouseReport.y_displacement,
           (unified_mouseReport.buttons.button1 ? 'L' : ' '),
           (unified_mouseReport.buttons.button3 ? 'M' : ' '),
           (unified_mouseReport.buttons.button2 ? 'R' : ' '),
           unified_mouseReport.scroll_wheel);
    fflush(stdout);
#endif

    // Call registered callback function if one exists
    if (registered_mouse_callback != NULL) {
        registered_mouse_callback(&unified_mouseReport);
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
static void hid_host_generic_report_callback(const uint8_t* const data,
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
                           const hid_host_driver_event_t event, void* arg) {
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));
    const hid_host_device_config_t dev_config = {
        .callback = hid_host_interface_callback, .callback_arg = NULL};

    switch (event) {
        case HID_HOST_DRIVER_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "HID Device, protocol '%s' CONNECTED",
                     hid_proto_name_str[dev_params.proto]);
            ESP_ERROR_CHECK(
                hid_host_device_open(hid_device_handle, &dev_config));

            if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
                if (HID_PROTOCOL_MOUSE == dev_params.proto) {
                    ESP_LOGI(
                        TAG,
                        "Mouse device detected, parsing report descriptor...");

                    // Try to get and parse the HID report descriptor
                    size_t report_desc_len = 0;
                    uint8_t* report_desc = hid_host_get_report_descriptor(
                        hid_device_handle, &report_desc_len);

                    bool use_boot_protocol = true;

                    if (report_desc != NULL && report_desc_len > 0) {
                        ESP_LOGI(TAG, "Got report descriptor, length: %zu",
                                 report_desc_len);

                        if (parse_mouse_report_descriptor(
                                report_desc, report_desc_len, &mouse_format)) {
                            ESP_LOGI(TAG,
                                     "Successfully parsed mouse report "
                                     "descriptor, using report protocol");
                            ESP_ERROR_CHECK(hid_class_request_set_protocol(
                                hid_device_handle, HID_REPORT_PROTOCOL_REPORT));
                            use_boot_protocol = false;
                        } else {
                            ESP_LOGW(TAG,
                                     "Failed to parse mouse report descriptor");
                        }

                        // Note: Check if we need to free the report descriptor
                        // Some implementations return a pointer that needs to
                        // be freed free(report_desc); // Uncomment if needed
                    } else {
                        ESP_LOGW(TAG,
                                 "Could not get report descriptor (NULL or "
                                 "length=0)");
                    }

                    if (use_boot_protocol) {
                        ESP_LOGI(TAG,
                                 "Falling back to boot protocol for mouse");
                        ESP_ERROR_CHECK(hid_class_request_set_protocol(
                            hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
                        mouse_format.is_valid =
                            false;  // Use boot protocol parsing
                    }
                }

                if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                    ESP_ERROR_CHECK(
                        hid_class_request_set_idle(hid_device_handle, 0, 0));
                    //        ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle,HID_REPORT_PROTOCOL_BOOT));
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

        // Release devices once all clients has deregistered
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
            ESP_LOGI(TAG, "USB Event flags: NO_CLIENTS");
        }
        // All devices were removed
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB Event flags: ALL_FREE");
            digitalWrite(LED_BUILTIN, HIGH);
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
