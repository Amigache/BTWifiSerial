/**
 * @file sbus_output.cpp
 * @brief SBUS trainer serial output for EdgeTX (TX16S MK3+)
 *
 * SBUS frame structure (25 bytes):
 *   Byte  0:      0x0F (header)
 *   Bytes 1-22:   16 channels × 11 bits = 176 bits packed LSB-first
 *   Byte  23:     Flags (bit0=ch17, bit1=ch18, bit2=frame_lost, bit3=failsafe)
 *   Byte  24:     0x00 (footer)
 *
 * Serial config: 100000 baud, 8E2, inverted.
 * On ESP32-C3, UART inversion is handled in hardware.
 * Frame sent every ~14ms (analog mode).
 */

#include "sbus_output.h"
#include "config.h"
#include "channel_data.h"
#include "log.h"
#include <driver/uart.h>

// ─── SBUS constants ─────────────────────────────────────────────────
static constexpr uint32_t SBUS_BAUD           = 100000;
static constexpr uint8_t  SBUS_FRAME_SIZE     = 25;
static constexpr uint8_t  SBUS_HEADER         = 0x0F;
static constexpr uint8_t  SBUS_FOOTER         = 0x00;
static constexpr uint32_t SBUS_FRAME_INTERVAL = 14;  // ms

// ─── Module state ───────────────────────────────────────────────────
static bool     s_running      = false;
static uint32_t s_lastFrameMs  = 0;
static uint8_t  s_frame[SBUS_FRAME_SIZE];

/**
 * @brief Pack 16 channel values (11-bit each) into the 22-byte SBUS data area
 *
 * SBUS channels are packed LSB-first across byte boundaries:
 * ch1  = frame[1]     | (frame[2]  << 8)                        & 0x7FF
 * ch2  = (frame[2]>>3)| (frame[3]  << 5)                        & 0x7FF
 * ch3  = (frame[3]>>6)| (frame[4]  << 2) | (frame[5] << 10)    & 0x7FF
 * ... etc.
 */
static void packSbusFrame(const uint16_t* channels, uint8_t numCh) {
    memset(s_frame, 0, SBUS_FRAME_SIZE);
    s_frame[0] = SBUS_HEADER;

    // Pack up to 16 channels, 11 bits each
    uint16_t ch[SBUS_NUM_CHANNELS];
    for (uint8_t i = 0; i < SBUS_NUM_CHANNELS; i++) {
        if (i < numCh) {
            ch[i] = ChannelData::frskyToSbus(channels[i]);
        } else {
            ch[i] = SBUS_CENTER;  // Center unused channels
        }
    }

    // Bit-pack 16 × 11-bit values into bytes 1..22
    s_frame[1]  = (uint8_t)(ch[0]  & 0xFF);
    s_frame[2]  = (uint8_t)((ch[0]  >> 8)  | (ch[1]  << 3));
    s_frame[3]  = (uint8_t)((ch[1]  >> 5)  | (ch[2]  << 6));
    s_frame[4]  = (uint8_t)((ch[2]  >> 2)  & 0xFF);
    s_frame[5]  = (uint8_t)((ch[2]  >> 10) | (ch[3]  << 1));
    s_frame[6]  = (uint8_t)((ch[3]  >> 7)  | (ch[4]  << 4));
    s_frame[7]  = (uint8_t)((ch[4]  >> 4)  | (ch[5]  << 7));
    s_frame[8]  = (uint8_t)((ch[5]  >> 1)  & 0xFF);
    s_frame[9]  = (uint8_t)((ch[5]  >> 9)  | (ch[6]  << 2));
    s_frame[10] = (uint8_t)((ch[6]  >> 6)  | (ch[7]  << 5));
    s_frame[11] = (uint8_t)((ch[7]  >> 3)  & 0xFF);
    s_frame[12] = (uint8_t)(ch[8]   & 0xFF);
    s_frame[13] = (uint8_t)((ch[8]  >> 8)  | (ch[9]  << 3));
    s_frame[14] = (uint8_t)((ch[9]  >> 5)  | (ch[10] << 6));
    s_frame[15] = (uint8_t)((ch[10] >> 2)  & 0xFF);
    s_frame[16] = (uint8_t)((ch[10] >> 10) | (ch[11] << 1));
    s_frame[17] = (uint8_t)((ch[11] >> 7)  | (ch[12] << 4));
    s_frame[18] = (uint8_t)((ch[12] >> 4)  | (ch[13] << 7));
    s_frame[19] = (uint8_t)((ch[13] >> 1)  & 0xFF);
    s_frame[20] = (uint8_t)((ch[13] >> 9)  | (ch[14] << 2));
    s_frame[21] = (uint8_t)((ch[14] >> 6)  | (ch[15] << 5));
    s_frame[22] = (uint8_t)((ch[15] >> 3)  & 0xFF);

    // Flags byte: no failsafe, no frame lost
    s_frame[23] = 0x00;
    if (g_channelData.isStale()) {
        s_frame[23] |= 0x04;  // Frame lost flag
    }

    s_frame[24] = SBUS_FOOTER;
}

// ─── Public API ─────────────────────────────────────────────────────

void sbusInit() {
    // Configure UART1 for SBUS: 100000 baud, 8E2, inverted
    // ESP32-C3 supports hardware signal inversion
    uart_config_t uart_config = {
        .baud_rate  = SBUS_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_EVEN,
        .stop_bits  = UART_STOP_BITS_2,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, PIN_SERIAL_TX, PIN_SERIAL_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 256, 256, 0, NULL, 0));

    // Invert TX signal for SBUS
    ESP_ERROR_CHECK(uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_TXD_INV));

    s_running     = true;
    s_lastFrameMs = 0;

    LOG_I("SBUS", "Initialized: TX=%d RX=%d @ %lu baud (8E2 inverted)",
          PIN_SERIAL_TX, PIN_SERIAL_RX, SBUS_BAUD);
}

void sbusLoop() {
    if (!s_running) return;

    uint32_t now = millis();
    if (now - s_lastFrameMs < SBUS_FRAME_INTERVAL) return;
    s_lastFrameMs = now;

    // Get current channel data
    uint16_t channels[BT_CHANNELS];
    g_channelData.getChannels(channels, BT_CHANNELS);

    // Pack and send SBUS frame
    packSbusFrame(channels, BT_CHANNELS);
    uart_write_bytes(UART_NUM_1, (const char*)s_frame, SBUS_FRAME_SIZE);
}

void sbusStop() {
    if (s_running) {
        uart_driver_delete(UART_NUM_1);
        s_running = false;
        LOG_I("SBUS", "Stopped");
    }
}
