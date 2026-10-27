#ifndef CANFD_CODEC_H_
#define CANFD_CODEC_H_

/*
 * Pure conversions between the CAN FD protocol document's IDR/DLCR register
 * layout and a neutral CAN message description.  No sockets, no kernel
 * headers: this file is unit-testable on the host and reused by the
 * SocketCAN layer.
 *
 * IDR  (PG223 Table 30/38): 31:21 ID[28:18], 20 SRR/RTR, 19 IDE,
 *                           18:1 ID[17:0], 0 RTR/RRS
 * DLCR (PG223 Table 31/39): 31:28 DLC, 27 EDL, 26 BRS, 25 ESI/rsv,
 *                           23:16 MM, 15:0 timestamp (RX) / reserved (TX)
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CANFD_MAX_DATA_LEN 64

typedef struct {
    uint32_t can_id;    /* 11-bit or 29-bit identifier, no flag bits */
    uint8_t eff;        /* 29-bit extended identifier */
    uint8_t rtr;        /* remote frame (classic CAN only) */
    uint8_t fd;         /* CAN FD frame (EDL=1) */
    uint8_t brs;        /* bit rate switch (data phase faster) */
    uint8_t esi;        /* error state indicator (receive side) */
    uint8_t len;        /* payload length in bytes, 0..64 */
    uint8_t data[CANFD_MAX_DATA_LEN];
    uint16_t timestamp; /* DLCR low 16 bits, 0 when not used */
} canfd_message_t;

/* --- IDR --- */
uint32_t canfd_idr_encode(uint32_t can_id, int eff, int rtr);
int canfd_idr_decode(uint32_t idr, uint32_t* can_id, int* eff, int* rtr);

/* --- DLC helpers --- */
int canfd_dlc_to_len(uint32_t dlc);
uint32_t canfd_len_to_dlc(int len);

/* --- DLCR --- */
uint32_t canfd_dlcr_encode(const canfd_message_t* msg);
int canfd_dlcr_decode(uint32_t dlcr, canfd_message_t* msg);

/* --- 112-byte protocol packet message area (Byte17..Byte88) --- */
int canfd_packet_decode_message(const uint8_t packet[112], canfd_message_t* msg);
void canfd_packet_encode_message(uint8_t packet[112], const canfd_message_t* msg);

#ifdef __cplusplus
}
#endif

#endif /* CANFD_CODEC_H_ */
