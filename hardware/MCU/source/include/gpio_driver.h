#ifndef GPIO_DRIVER_H
#define GPIO_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

/* ==============================================================================
 * Discrete Hardware Input Pins (Reversed from factory dump 0x08006EAC - 0x08007000)
 * ============================================================================== */
#define GPIO_PIN_ACC_SENSE          8   /* GPIOA Pin 8: ACC / 12V Ignition Detect (0x08006EE2) */
#define GPIO_PIN_REV_SENSE          9   /* GPIOC Pin 9: 12V Reverse Camera Trigger (0x08006EF0) */
#define GPIO_PIN_SYNC_SENSE         8   /* GPIOC Pin 8: Video Sync / Illumination Detect (0x08006EFE) */
#define GPIO_PIN_VID_DET0           11  /* GPIOC Pin 11: Video source detect bit 0 (0x08006EC6) */
#define GPIO_PIN_VID_DET1           10  /* GPIOA Pin 10: Video source detect bit 1 (0x08006ED4) */
#define GPIO_PIN_WAKEUP_SENSE       2   /* GPIOD Pin 2: External Wakeup line (0x08006EAC) */

/* ==============================================================================
 * Discrete Output / Relay Control Pins (Reversed from factory dump 0x08006C1A - 0x08006CDE)
 * ============================================================================== */
typedef enum {
    RELAY_VIDEO_MUX     = 0,    /* GPIOB Pin 2: Video Relay Multiplexer (0x08006C3A) */
    RELAY_AV_POWER      = 1,    /* GPIOC Pin 13: Audio Relay / Video Power Switch (0x08006C52) */
    RELAY_AUX1          = 2,    /* GPIOB Pin 3: Aux Control Line 1 (0x08006C70) */
    RELAY_AUX2          = 3,    /* GPIOC Pin 12: Aux Relay 2 (0x08006C8A) */
    RELAY_POWER_HOLD    = 4,    /* GPIOB Pin 4: Main System Power Hold (0x08006CA8) */
    RELAY_SOC_RESET_C14 = 5,    /* GPIOC Pin 14: SoC Hardware Reset Dispatcher (0x08006CC2) */
    RELAY_SOC_RESET_B14 = 6     /* GPIOB Pin 14: Direct ARK1668 Hardware Reset (0x08005A18) */
} gpio_relay_id_t;

/* Polling interval matching factory Task 5 */
#define GPIO_POLL_INTERVAL_MS       50

/* ==============================================================================
 * Function Prototypes
 * ============================================================================== */
void gpio_driver_init(void);
void gpio_set_relay(gpio_relay_id_t relay_id, bool active);
bool gpio_get_acc_status(void);
bool gpio_get_reverse_status(void);
bool gpio_get_sync_status(void);
uint8_t gpio_get_composite_sense_mask(void);
void gpio_poll_senses(void);

#endif /* GPIO_DRIVER_H */
