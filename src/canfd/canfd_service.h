#ifndef CANFD_SERVICE_H_
#define CANFD_SERVICE_H_

/*
 * Board-side CANFD service interface.
 *
 * This is the layer to call from another application on the ZYNQ:
 *   - Start() brings up the TCP protocol service (9009/9010/9012) and
 *     the SocketCAN hardware layer.
 *   - Send() packs one CAN/CAN FD frame into the 112-byte protocol packet
 *     and queues it for the per-channel sender thread.
 *   - SetRxCallback() receives frames from the CAN bus in the RX thread.
 *
 * The TCP path itself is unchanged:
 *   9012 -> protocol.cpp -> per-channel TX queue -> canfd_hw_submit_frame()
 *   rx thread -> 0x81 packet -> 9010
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t channel;
    uint32_t can_id;
    uint8_t data[64];
    uint8_t length;
    uint8_t fd;
    uint8_t brs;
    uint8_t extended;
    uint8_t remote;
    uint8_t esi;
    uint16_t timestamp_low16;
    uint64_t timestamp_us;
} canfd_service_frame_t;

typedef void (*canfd_service_rx_cb)(const canfd_service_frame_t* frame, void* user);

/* Start 9009/9010/9012 and the CAN hardware layer. Idempotent. */
int canfd_service_start(void);

/* Stop the TCP service and close all CAN channels. */
void canfd_service_stop(void);

int canfd_service_is_running(void);

/*
 * Direct board-side send. Returns 0 when queued, -1 when the queue rejects it.
 * tx_state, if non-NULL, receives 1 for queued / 0 for rejected. The actual
 * transmit result is reported later by the 0x80 feedback packet.
 */
int canfd_service_send(uint8_t channel,
                       uint32_t can_id,
                       const uint8_t* data,
                       uint8_t length,
                       int fd,
                       int brs,
                       int extended,
                       int remote,
                       uint8_t* tx_state);

/* Callback runs in the per-channel SocketCAN RX thread. */
void canfd_service_set_rx_callback(canfd_service_rx_cb cb, void* user);

#ifdef __cplusplus
}
#endif

#endif /* CANFD_SERVICE_H_ */
