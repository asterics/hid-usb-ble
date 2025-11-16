
#ifndef USB_HOST_H
#define USB_HOST_H

#include <stdint.h>

#define OUTPUT_USB_MOUSE_REPORT_DEBUG_MESSAGES

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

typedef void (*mouse_report_callback_t)(unified_mouseReport_t* mouse_report);

void register_mouse_report_callback(mouse_report_callback_t callback);
void unbond_all_devices();
void start_usb_host();

#endif  // USB_HOST_H
