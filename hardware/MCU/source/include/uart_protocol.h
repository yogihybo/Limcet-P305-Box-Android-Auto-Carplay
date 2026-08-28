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
#define SOC_CMD_APP_PROTOCOL    0x85  /* App Protocol response/ACK. Real handler
                                        * (0x08008BA8) stores 3 payload bytes then
                                        * queues an outbound packet via an unmapped
                                        * indexed table -- see handle_app_protocol()
                                        * in uart_protocol.c for what's implemented
                                        * vs. approximated. */
#define SOC_CMD_BT_AT_RELAY     0x87  /* Bluetooth AT-command relay to onboard BT module.
                                        * Real firmware's handler at 0x080087A1 loads a
                                        * literal 0x40004800 (real STM32F105 USART3 base)
                                        * on its call path -- confirmed via disassembly this
                                        * session, see docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md.
                                        * That same call path also confirmed USART3's real
                                        * TX/RX pins (PB10/PB11, standard no-remap mapping)
                                        * via a {0xc00,...} GPIO-config struct. Baud rate was
                                        * NOT pinned down from disassembly -- 9600 below is a
                                        * documented best-guess (common BT-module AT default),
                                        * not a confirmed value. */
#define SOC_CMD_CRYPTO_CHALLENGE 0x88 /* TEA-cipher anti-clone challenge/response. Real
                                        * firmware's cipher (0x080050A0) is genuine
                                        * 32-round TEA decrypt, confirmed byte-exact via
                                        * disassembly (DELTA=0x9E3779B9, sum_init=
                                        * 0xC6EF3720). The real key was ALSO recovered, by
                                        * tracing the real firmware's .data init table to
                                        * find its flash source bytes (0x0800BCAC) -- see
                                        * tea_crypto.h for the full derivation. Effective
                                        * key entropy is only 32 bits (each of the 4 key
                                        * words has just its low byte set) -- a real,
                                        * confirmed finding about how weak this scheme
                                        * actually is, not a derivation artifact. */
#define SOC_CMD_DIAG_READ_MEM   0x90  /* Diagnostic Flash/SRAM readback */
#define SOC_CMD_SYNC_SETTINGS   0xA0  /* UI settings sync */
#define SOC_CMD_REBOOT_BOOTLDR  0xE1  /* Enter bootloader for update */
#define SOC_CMD_SYSTEM_RESET    0xFF  /* System State Reset. Real handler (0x080088E8)
                                        * is a sub-command dispatch on payload[0]:
                                        * values 0-9 are genuinely no-op there too, only
                                        * sub-id 0x7F triggers real action -- see
                                        * handle_system_reset() in uart_protocol.c. */

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t payload[UART_MAX_PAYLOAD];
} UartPacket;

/* CMD 0xA0 settings struct fields -- names/offsets follow the real firmware's confirmed
 * struct at SRAM 0x200001D8 (see docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md, "CMD 0xA0 dispatch
 * is a genuine TBB jump table"). Only id=0x00, 0x09, 0x0b have a real GPIO effect traced to
 * a physical pin here; the rest are stored for protocol compatibility with the real 18-entry
 * table but have no confirmed hardware action yet -- ids 0x01-0x06 and 0x0e are genuinely
 * no-op/unimplemented in the real firmware too, not a gap in this reimplementation. */
typedef struct {
    uint8_t  mode_3b;      /* id=0x00: multi-way mode, drives GPIOB Pin 1 when == 1 */
    uint8_t  flag_3a;      /* id=0x07: write-only in the real firmware, as far as a scan
                             * of every immediate-offset ldrb/strb access to this offset
                             * in can_app.bin can show -- found only the handler's own
                             * write, no reader anywhere else via that addressing form.
                             * Caveat: doesn't rule out a register-indexed read. */
    uint8_t  flag_39;      /* id=0x08: same write-only finding/caveat as id=0x07 above. */
    uint8_t  mic_mux_38;   /* id=0x09: mic/audio input mux -- real, drives GPIOB Pin 6 (PB6) */
    uint8_t  value_3c;     /* id=0x0a: same write-only finding/caveat as id=0x07 above. */
    uint8_t  group_3d;     /* id=0x0b: clears to 0 -> fires PA15/PB8/PB9 HIGH together */
    uint16_t value_40;     /* id=0x0c: REAL READER FOUND (0x08005D8E, same poll function
                             * as id=0x0b's PA15/PB8/PB9 group above) -- used as a
                             * threshold compared against a counter/timer at struct
                             * offset 0x4c (not modeled here), gating that SAME 3-pin
                             * group together with struct offset 0x13 (must equal 4 or
                             * 5 -- likely a vehicle-profile/mode field) and 0x3d being
                             * nonzero. Reveals id=0x0b and id=0x0c work together as one
                             * coordinated subsystem, not independent settings -- real
                             * finding, but the physical GPIO effect is still the same
                             * PA15/PB8/PB9 group already deliberately left unwired for
                             * id=0x0b (see below), so no new pin toggle added here. */
    uint8_t  flag_42;      /* id=0x0d: same write-only finding/caveat as id=0x07 above. */
    uint8_t  value_43;     /* id=0x0f: plain stored value, confirmed NO GPIO effect */
    uint8_t  value_44;     /* id=0x10: plain stored value, confirmed NO GPIO effect */
    uint8_t  value_45;     /* id=0x11: gated by flag_5e; real target GPIOC Pin 13
                             * (collides with the SoC hardware-reset pin -- see the
                             * handle_sync_settings id=0x11 case comment) */
    uint8_t  flag_5e;      /* Gates id=0x11's real GPIO effect (== 1 required). Real
                             * firmware sets this elsewhere; not traced in this pass --
                             * defaults to 0, so id=0x11 stays inert until it is. */
} McuSettings;

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
const McuSettings *mcu_settings_get(void);

/* USART3 / Bluetooth AT relay (CMD 0x87) */
void usart3_relay_init(void);
void usart3_relay_send(const uint8_t *data, uint8_t len);

#endif /* UART_PROTOCOL_H */
