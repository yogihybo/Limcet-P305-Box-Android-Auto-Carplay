#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include "stm32f105.h"

#define UART_HEADER_SIG         0x2E
#define UART_MAX_PAYLOAD        32
#define UART_RX_RING_SIZE       8

/* Outbound Command Codes (MCU -> SoC) */
#define MCU_CMD_HANDSHAKE_VER   0x01  /* Version & handshake report */
#define MCU_CMD_INPUT_EVENT     0x02  /* Key / SWC button event (2 bytes: [Key, State]) */
#define MCU_CMD_STATUS_BITS     0x03  /* General status */
#define MCU_CMD_RADAR_LEVEL     0x04  /* Parking radar distance */
#define MCU_CMD_STATUS_5018     0x05  /* Power / ACC status */
#define MCU_CMD_REVERSE_GEAR    0x06  /* Reverse / camera trigger */
#define MCU_CMD_STEERING_ANGLE  0x0A  /* Steering trajectory angle */
#define MCU_CMD_DIP_PROFILE     0x12  /* Vehicle DIP switch profile report */
#define MCU_CMD_STATUS_QUERY    0x20  /* Status query */
#define MCU_CMD_VERSION_REPORT  0x7F  /* MCU version string */

/* Inbound Command Codes (SoC -> MCU) */
#define SOC_CMD_INIT_HANDSHAKE  0x81  /* Init handshake */
#define SOC_CMD_APP_STATE       0x82  /* App foreground/mode change */
#define SOC_CMD_AUDIO_ROUTE     0x84  /* Audio routing */
#define SOC_CMD_APP_PROTOCOL    0x85  /* App protocol */
#define SOC_CMD_SETTINGS_SELECT 0x87  /* Menu / BT command */
#define SOC_CMD_TIMESTAMP       0x88  /* Timestamp / counter */
#define SOC_CMD_DIAG_READ_MEM   0x90  /* Diagnostic Flash/SRAM readback */
#define SOC_CMD_SYNC_SETTINGS   0xA0  /* UI settings sync */
#define SOC_CMD_REBOOT_BOOTLDR  0xE1  /* Enter bootloader for update */
#define SOC_CMD_SYSTEM_RESET    0xFF  /* State reset */

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t payload[UART_MAX_PAYLOAD];
} UartPacket;

typedef void (*UartCmdHandlerFunc)(const UartPacket *packet);

typedef struct {
    uint8_t            cmd;
    uint8_t            reserved[3];
    UartCmdHandlerFunc handler;
} UartCmdDispatchEntry;

/* Driver API */
void uart_protocol_init(uint32_t baudrate);
void uart_send_packet(uint8_t cmd, const uint8_t *payload, uint8_t len);
void uart_send_key_event(uint8_t key_code, bool pressed);
void uart_send_reverse_state(bool reverse_active);
void uart_send_steering_angle(int16_t angle_deci_degrees);
void uart_send_radar_levels(uint8_t left, uint8_t mid_left, uint8_t mid_right, uint8_t right);
void uart_process_rx(void);
void uart_trigger_bootloader_reset(void);

#endif /* UART_PROTOCOL_H */
