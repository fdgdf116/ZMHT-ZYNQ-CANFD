#include "canfd_codec.h"

#include <string.h>

namespace {

const uint8_t kFdLen[16] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64
};

const uint32_t kDlcrDlcShift = 28U;
const uint32_t kDlcrEdl = 0x08000000U;
const uint32_t kDlcrBrs = 0x04000000U;
const uint32_t kDlcrEsi = 0x02000000U;

uint32_t be32_read(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

void be32_write(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

}  // namespace

extern "C" uint32_t canfd_idr_encode(uint32_t can_id, int eff, int rtr)
{
    if (eff) {
        return (((can_id >> 18) & 0x7FFU) << 21) |
               (1U << 20) |               /* SRR = 1 for extended frames */
               (1U << 19) |               /* IDE = 1 */
               ((can_id & 0x3FFFFU) << 1) |
               (rtr ? 1U : 0U);
    }
    return (((can_id & 0x7FFU) << 21) |
            (rtr ? (1U << 20) : 0U));     /* SRR/RTR bit, IDE = 0 */
}

extern "C" int canfd_idr_decode(uint32_t idr, uint32_t* can_id, int* eff, int* rtr)
{
    const int extended = (int)((idr >> 19) & 0x1U);
    if (extended) {
        if (can_id != nullptr) {
            *can_id = (((idr >> 21) & 0x7FFU) << 18) | ((idr >> 1) & 0x3FFFFU);
        }
        if (rtr != nullptr) {
            *rtr = (int)(idr & 0x1U);
        }
    } else {
        if (can_id != nullptr) {
            *can_id = (idr >> 21) & 0x7FFU;
        }
        if (rtr != nullptr) {
            *rtr = (int)((idr >> 20) & 0x1U);
        }
    }
    if (eff != nullptr) {
        *eff = extended;
    }
    return 0;
}

extern "C" int canfd_dlc_to_len(uint32_t dlc)
{
    return kFdLen[dlc & 0xFU];
}

extern "C" uint32_t canfd_len_to_dlc(int len)
{
    /* Table 3-2: DLC 0..8 are byte counts, then 12/16/20/24/32/48/64.
     * Pick the smallest DLC able to carry len bytes (pad with zeros). */
    if (len <= 0) {
        return 0U;
    }
    for (uint32_t i = 1; i < 16U; ++i) {
        if (len <= kFdLen[i]) {
            return i;
        }
    }
    return 15U;
}

extern "C" uint32_t canfd_dlcr_encode(const canfd_message_t* msg)
{
    if (msg == nullptr) {
        return 0;
    }
    int len = msg->len;
    if (len < 0) {
        len = 0;
    }
    if (len > CANFD_MAX_DATA_LEN) {
        len = CANFD_MAX_DATA_LEN;
    }
    uint32_t dlc;
    if (msg->fd) {
        dlc = canfd_len_to_dlc(len);
    } else {
        if (len > 8) {
            len = 8;
        }
        dlc = static_cast<uint32_t>(len);
    }
    uint32_t dlcr = (dlc & 0xFU) << kDlcrDlcShift;
    if (msg->fd) {
        dlcr |= kDlcrEdl;
    }
    if (msg->brs) {
        dlcr |= kDlcrBrs;
    }
    if (msg->esi) {
        dlcr |= kDlcrEsi;
    }
    dlcr |= static_cast<uint32_t>(msg->timestamp);
    return dlcr;
}

extern "C" int canfd_dlcr_decode(uint32_t dlcr, canfd_message_t* msg)
{
    if (msg == nullptr) {
        return -1;
    }
    const uint32_t dlc = (dlcr >> kDlcrDlcShift) & 0xFU;
    const int fd = (dlcr & kDlcrEdl) ? 1 : 0;
    msg->fd = static_cast<uint8_t>(fd);
    msg->brs = (dlcr & kDlcrBrs) ? 1 : 0;
    msg->esi = (dlcr & kDlcrEsi) ? 1 : 0;
    msg->timestamp = static_cast<uint16_t>(dlcr & 0xFFFFU);
    if (fd) {
        msg->len = static_cast<uint8_t>(canfd_dlc_to_len(dlc));
    } else {
        msg->len = static_cast<uint8_t>(dlc > 8U ? 8U : dlc);
    }
    return 0;
}

extern "C" int canfd_packet_decode_message(const uint8_t packet[112], canfd_message_t* msg)
{
    if (packet == nullptr || msg == nullptr) {
        return -1;
    }
    memset(msg, 0, sizeof(*msg));

    uint32_t can_id = 0;
    int eff = 0;
    int rtr = 0;
    canfd_idr_decode(be32_read(packet + 16), &can_id, &eff, &rtr);
    msg->can_id = can_id;
    msg->eff = static_cast<uint8_t>(eff);
    msg->rtr = static_cast<uint8_t>(rtr);
    canfd_dlcr_decode(be32_read(packet + 20), msg);

    /* CAN FD frames have no remote variant; the doc requires RTR=0 there. */
    if (msg->fd) {
        msg->rtr = 0;
    }
    if (msg->len > CANFD_MAX_DATA_LEN) {
        msg->len = CANFD_MAX_DATA_LEN;
    }
    memcpy(msg->data, packet + 24, msg->len);
    return 0;
}

extern "C" void canfd_packet_encode_message(uint8_t packet[112], const canfd_message_t* msg)
{
    if (packet == nullptr || msg == nullptr) {
        return;
    }
    canfd_message_t fixed = *msg;
    if (fixed.len > CANFD_MAX_DATA_LEN) {
        fixed.len = CANFD_MAX_DATA_LEN;
    }
    if (fixed.fd) {
        fixed.rtr = 0;
    }
    be32_write(packet + 16, canfd_idr_encode(fixed.can_id, fixed.eff, fixed.rtr));
    be32_write(packet + 20, canfd_dlcr_encode(&fixed));
    memset(packet + 24, 0, CANFD_MAX_DATA_LEN);
    memcpy(packet + 24, fixed.data, fixed.len);
}
