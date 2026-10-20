/*
 * tusb_config.h - TinyUSB setup for ps1-writer:
 *   root port 0 (the Pico's own USB): device, two serial ports:
 *       log messages, and the AI link to the PC
 *   root port 1 (PIO-USB on GPIO pins): host, for the USB keyboard
 */

#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUSB_DEBUG          0

#define BOARD_TUD_RHPORT        0
#define BOARD_TUH_RHPORT        1

/* device: two serial ports */
#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_DEFAULT_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64
#define CFG_TUD_CDC             2
#define CFG_TUD_CDC_RX_BUFSIZE  1024
#define CFG_TUD_CDC_TX_BUFSIZE  1024
#define CFG_TUD_CDC_EP_BUFSIZE  64

/* host: keyboard over PIO-USB */
#define CFG_TUH_ENABLED         1
#define CFG_TUH_MAX_SPEED       OPT_MODE_DEFAULT_SPEED
#define CFG_TUH_RPI_PIO_USB     1
#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB             1               /* keyboards with a built-in hub */
#define CFG_TUH_DEVICE_MAX      4
#define CFG_TUH_HID             8
#define CFG_TUH_HID_EPIN_BUFSIZE  64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#define CFG_TUD_MEM_ALIGN       __attribute__ ((aligned(4)))
#define CFG_TUH_MEM_ALIGN       __attribute__ ((aligned(4)))

#endif
