#include "can_driver.h"
#include "vehicle_profiles.h"
#include "uart_protocol.h"

static CanRingBuffer g_can_rx_ring;
static uint8_t g_active_mode = 1;

void CAN1_RX0_IRQHandler(void) {
    /* Check if FIFO0 has pending messages (FMP0 > 0) */
    if ((CAN1->RF0R & 0x03) != 0) {
        uint8_t next_head = (g_can_rx_ring.head + 1) % CAN_RX_RING_SIZE;
        if (next_head != g_can_rx_ring.tail) {
            CanFrame *f = &g_can_rx_ring.frames[g_can_rx_ring.head];
            
            uint32_t rir = CAN1->sFIFOMailBox[0].RIR;
            uint32_t rdtr = CAN1->sFIFOMailBox[0].RDTR;
            uint32_t rdlr = CAN1->sFIFOMailBox[0].RDLR;
            uint32_t rdhr = CAN1->sFIFOMailBox[0].RDHR;
            
            f->ide = (uint8_t)((rir >> 2) & 0x01) ? 4 : 0;
            f->rtr = (uint8_t)((rir >> 1) & 0x01) ? 2 : 0;
            if (f->ide == 0) {
                f->id = (rir >> 21) & 0x7FF;
                f->ext_id = 0;
            } else {
                f->id = (rir >> 3) & 0x1FFFFFFF;
                f->ext_id = f->id;
            }
            f->dlc = rdtr & 0x0F;
            f->fmi = (rdtr >> 8) & 0xFF;
            
            f->data[0] = (uint8_t)(rdlr);
            f->data[1] = (uint8_t)(rdlr >> 8);
            f->data[2] = (uint8_t)(rdlr >> 16);
            f->data[3] = (uint8_t)(rdlr >> 24);
            f->data[4] = (uint8_t)(rdhr);
            f->data[5] = (uint8_t)(rdhr >> 8);
            f->data[6] = (uint8_t)(rdhr >> 16);
            f->data[7] = (uint8_t)(rdhr >> 24);
            
            g_can_rx_ring.head = next_head;
        }
        /* Release FIFO0 output mailbox */
        CAN1->RF0R |= (1UL << 5); /* RFOM0 */
    }
}

void can_init(uint32_t baudrate) {
    (void)baudrate;
    
    g_can_rx_ring.head = 0;
    g_can_rx_ring.tail = 0;
    
    /* Enable Clocks: CAN1, GPIOA, GPIOB, AFIO */
    RCC->APB1ENR |= (1UL << 25); /* CAN1EN */
    RCC->APB2ENR |= (1UL << 2) | (1UL << 3) | (1UL << 0); /* IOPAEN, IOPBEN, AFIOEN */
    
    /* Configure CAN1 pins (Default: PA11 RX, PA12 TX) */
    /* PA11: Input pull-up / floating (Mode 00, CNF 10) */
    GPIOA->CRH &= ~(0x0FUL << 12);
    GPIOA->CRH |=  (0x08UL << 12);
    GPIOA->ODR |=  (1UL << 11);
    
    /* PA12: Alternate function push-pull 50MHz (Mode 11, CNF 10) */
    GPIOA->CRH &= ~(0x0FUL << 16);
    GPIOA->CRH |=  (0x0BUL << 16);
    
    /* Exit Sleep Mode & Enter Initialization Mode */
    CAN1->MCR &= ~(1UL << 1); /* Clear SLEEP */
    CAN1->MCR |=  (1UL << 0); /* Set INRQ */
    while ((CAN1->MSR & (1UL << 0)) == 0) {} /* Wait for INAK */
    
    /* Configure CAN Timing for 500 kbit/s (Assuming APB1 = 36 MHz) */
    /* Prescaler = 4 -> Tq = 4 / 36 MHz = 111.11 ns */
    /* 18 Tq per bit (1 Sync + 12 BS1 + 5 BS2) -> 18 * 111.11 ns = 2.0 us = 500 kbps */
    CAN1->BTR = (0UL << 30) | (0UL << 24) | (4UL << 20) | (11UL << 16) | (3UL << 0);
    
    /* Automatic Bus-Off management, Auto-Wakeup */
    CAN1->MCR |= (1UL << 6) | (1UL << 5); /* ABOM, AWUM */
    
    /* Configure Acceptance Filters: Pass All into FIFO0 */
    CAN1->FMR |= (1UL << 0); /* FINIT */
    CAN1->FA1R &= ~(1UL << 0); /* Deactivate filter 0 */
    CAN1->FS1R |=  (1UL << 0); /* 32-bit scale */
    CAN1->FM1R &= ~(1UL << 0); /* Identifier Mask mode */
    CAN1->FFA1R &= ~(1UL << 0); /* Assign to FIFO 0 */
    CAN1->sFilterRegister[0].FR1 = 0x00000000;
    CAN1->sFilterRegister[0].FR2 = 0x00000000;
    CAN1->FA1R |=  (1UL << 0); /* Activate filter 0 */
    CAN1->FMR &= ~(1UL << 0); /* Exit Filter Init */
    
    /* Enable FIFO0 Message Pending Interrupt */
    CAN1->IER |= (1UL << 1); /* FMPIE0 */
    
    /* Enter Normal Operating Mode */
    CAN1->MCR &= ~(1UL << 0); /* Clear INRQ */
    while ((CAN1->MSR & (1UL << 0)) != 0) {} /* Wait for normal mode */
    
    /* Enable NVIC IRQ 20 (CAN1_RX0) */
    nvic_enable_irq(20);
}

bool can_transmit(const CanFrame *frame) {
    if (!frame) return false;
    
    /* Find empty TX mailbox (TME0, TME1, TME2) */
    uint32_t tsr = CAN1->TSR;
    uint8_t mb = 0xFF;
    if (tsr & (1UL << 26)) mb = 0;
    else if (tsr & (1UL << 27)) mb = 1;
    else if (tsr & (1UL << 28)) mb = 2;
    
    if (mb == 0xFF) return false; /* All mailboxes full */
    
    uint32_t tir = 0;
    if (frame->ide == 0) {
        tir = (frame->id << 21);
    } else {
        tir = (frame->ext_id << 3) | (1UL << 2);
    }
    if (frame->rtr) tir |= (1UL << 1);
    
    CAN1->sTxMailBox[mb].TIR = tir;
    CAN1->sTxMailBox[mb].TDTR = (frame->dlc & 0x0F);
    CAN1->sTxMailBox[mb].TDLR = (uint32_t)frame->data[0] |
                                ((uint32_t)frame->data[1] << 8) |
                                ((uint32_t)frame->data[2] << 16) |
                                ((uint32_t)frame->data[3] << 24);
    CAN1->sTxMailBox[mb].TDHR = (uint32_t)frame->data[4] |
                                ((uint32_t)frame->data[5] << 8) |
                                ((uint32_t)frame->data[6] << 16) |
                                ((uint32_t)frame->data[7] << 24);
                                
    /* Request Transmission */
    CAN1->sTxMailBox[mb].TIR |= (1UL << 0); /* TXRQ */
    return true;
}

bool can_pop_rx_frame(CanFrame *out_frame) {
    if (g_can_rx_ring.head == g_can_rx_ring.tail) {
        return false;
    }
    if (out_frame) {
        *out_frame = g_can_rx_ring.frames[g_can_rx_ring.tail];
    }
    g_can_rx_ring.tail = (g_can_rx_ring.tail + 1) % CAN_RX_RING_SIZE;
    return true;
}

void can_set_active_mode(uint8_t mode) {
    g_active_mode = mode;
}

uint8_t can_get_active_mode(void) {
    return g_active_mode;
}

void can_reset_rx_ring(void) {
    g_can_rx_ring.head = 0;
    g_can_rx_ring.tail = 0;
}

void can_dispatch_process(void) {
    CanFrame frame;
    while (can_pop_rx_frame(&frame)) {
        uint8_t count = 0;
        const CanDispatchEntry *table = vehicle_get_dispatch_table(g_active_mode, &count);
        if (table) {
            for (uint8_t i = 0; i < count; i++) {
                if (table[i].can_id == frame.id && table[i].handler) {
                    table[i].handler(&frame);
                    break;
                }
            }
        }
    }
}
