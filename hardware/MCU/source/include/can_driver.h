#ifndef CAN_DRIVER_H
#define CAN_DRIVER_H

#include "stm32f105.h"

#define CAN_RX_RING_SIZE 15

typedef struct {
    uint32_t id;        /* Standard (11-bit) or Extended (29-bit) ID */
    uint32_t ext_id;
    uint8_t  ide;       /* 0 = Standard, 4 = Extended */
    uint8_t  rtr;       /* 0 = Data, 2 = Remote */
    uint8_t  dlc;       /* 0 to 8 */
    uint8_t  data[8];
    uint8_t  fmi;       /* Filter index */
} CanFrame;

typedef void (*CanHandlerFunc)(const CanFrame *frame);

typedef struct {
    uint32_t       can_id;
    CanHandlerFunc handler;
} CanDispatchEntry;

/* Ring Buffer */
typedef struct {
    CanFrame frames[CAN_RX_RING_SIZE];
    volatile uint8_t head;
    volatile uint8_t tail;
} CanRingBuffer;

/* Driver API */
void can_init(uint32_t baudrate);
bool can_transmit(const CanFrame *frame);
bool can_pop_rx_frame(CanFrame *out_frame);
void can_set_active_mode(uint8_t mode);
uint8_t can_get_active_mode(void);
void can_dispatch_process(void);
void can_reset_rx_ring(void);

#endif /* CAN_DRIVER_H */
