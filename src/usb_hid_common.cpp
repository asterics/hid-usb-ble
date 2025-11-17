#include <Arduino.h>
#include <esp_log.h>
#include "usb_hid_common.h"

/**
 * Extract integer value of size_bits starting at bit_offset (LSB = bit 0 of
 * data[0])
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
        } else {
            printf("Generic\r\n");
        }
        fflush(stdout);
    }
}


// Global callback function pointer
static mouse_report_callback_t registered_mouse_callback = NULL;

mouse_report_callback_t * get_registered_mouse_callback() {
    return &registered_mouse_callback;
}