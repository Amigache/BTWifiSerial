#pragma once
#include <Arduino.h>

// MSPv2 parser + builder for ELRS Backpack protocol
// Frame: $X< [flags 1B] [function LE16] [size LE16] [payload...] [crc8_dvb_s2]

constexpr uint16_t MSP_ELRS_SET_PTR          = 0x0383;
constexpr uint16_t MSP_ELRS_SET_HEAD_TRACKING = 0x030D;
constexpr uint16_t MSP_ELRS_REQU_VTX_PKT     = 0x0302;

constexpr uint8_t MSP_MAX_PAYLOAD = 16;

// CRC8 DVB-S2 (polynomial 0xD5)
inline uint8_t mspCrc8(uint8_t crc, uint8_t a) {
    crc ^= a;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x80) ? (crc << 1) ^ 0xD5 : (crc << 1);
    return crc;
}

struct MspParser {
    enum State : uint8_t {
        IDLE, DOLLAR, X, DIR,
        FUNC_LO, FUNC_HI,
        SIZE_LO, SIZE_HI,
        PAYLOAD, CRC
    };

    State    state    = IDLE;
    uint16_t function = 0;
    uint16_t size     = 0;
    uint8_t  payload[MSP_MAX_PAYLOAD];
    uint8_t  idx      = 0;
    uint8_t  crc      = 0;

    void reset() { state = IDLE; }

    // Feed one byte. Returns true when a valid packet is complete.
    bool feed(uint8_t b) {
        switch (state) {
        case IDLE:
            if (b == '$') state = DOLLAR;
            return false;
        case DOLLAR:
            state = (b == 'X') ? X : IDLE;
            return false;
        case X:
            state = (b == '<' || b == '>') ? DIR : IDLE;
            return false;
        case DIR:
            crc = mspCrc8(0, b);
            state = FUNC_LO;
            return false;
        case FUNC_LO:
            function = b;
            crc = mspCrc8(crc, b);
            state = FUNC_HI;
            return false;
        case FUNC_HI:
            function |= (uint16_t)b << 8;
            crc = mspCrc8(crc, b);
            state = SIZE_LO;
            return false;
        case SIZE_LO:
            size = b;
            crc = mspCrc8(crc, b);
            state = SIZE_HI;
            return false;
        case SIZE_HI:
            size |= (uint16_t)b << 8;
            crc = mspCrc8(crc, b);
            idx = 0;
            if (size == 0)                    state = CRC;
            else if (size <= MSP_MAX_PAYLOAD) state = PAYLOAD;
            else                              state = IDLE;
            return false;
        case PAYLOAD:
            payload[idx++] = b;
            crc = mspCrc8(crc, b);
            if (idx >= size) state = CRC;
            return false;
        case CRC:
            state = IDLE;
            return (crc == b);
        }
        return false;
    }

    // Extract a little-endian uint16 from payload at byte offset
    uint16_t payloadU16(uint8_t offset) const {
        return payload[offset] | ((uint16_t)payload[offset + 1] << 8);
    }
};

// Build an MSPv2 command frame into buf[]. Returns total frame length, or 0 if buf is too small.
inline uint8_t mspBuildCommand(uint8_t *buf, uint8_t bufSize, uint16_t func,
                               const uint8_t *data, uint16_t payloadSize) {
    uint8_t needed = 9 + payloadSize;
    if (needed > bufSize) return 0;

    buf[0] = '$';
    buf[1] = 'X';
    buf[2] = '<';

    uint8_t crc = 0;
    buf[3] = 0;                          crc = mspCrc8(crc, buf[3]);   // flags
    buf[4] = func & 0xFF;               crc = mspCrc8(crc, buf[4]);   // function LE
    buf[5] = func >> 8;                  crc = mspCrc8(crc, buf[5]);
    buf[6] = payloadSize & 0xFF;         crc = mspCrc8(crc, buf[6]);   // size LE
    buf[7] = payloadSize >> 8;           crc = mspCrc8(crc, buf[7]);

    for (uint16_t i = 0; i < payloadSize; i++) {
        buf[8 + i] = data[i];
        crc = mspCrc8(crc, data[i]);
    }

    buf[8 + payloadSize] = crc;
    return needed;
}
