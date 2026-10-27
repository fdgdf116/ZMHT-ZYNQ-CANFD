#ifndef CANFD_HW_H_
#define CANFD_HW_H_

#include <stdint.h>
#include "canfd_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol command 0x0103 run states (see protocol document, table 2). */
#define CANFD_RUN_OPEN 0xAAu
#define CANFD_RUN_CLOSE 0xBBu

/* Runs in the per-channel RX thread. */
typedef void (*canfd_hw_rx_cb)(uint8_t channel, const canfd_message_t* msg, void* user);

/*
 * SocketCAN backed CAN FD hardware layer.
 *
 * The implementation talks to the kernel's xilinx_can driver through
 * PF_CAN raw sockets (can0..can5) and configures bit timing with
 * "ip link set".  When an interface does not exist (for example on a
 * development host) the layer degrades to a logging stub so that the
 * protocol/TCP code stays testable.
 */
int canfd_hw_start(void);
void canfd_hw_stop(void);
int canfd_hw_apply_config(uint8_t channel, const uint8_t* init_frame, uint32_t length);
int canfd_hw_set_run(uint8_t channel, uint32_t state);
int canfd_hw_submit_frame(uint8_t channel, uint8_t type, const uint8_t* frame, uint32_t length);
int canfd_hw_submit_simulator(uint8_t channel, const uint8_t* packet, uint32_t length);
void canfd_hw_set_rx_callback(canfd_hw_rx_cb cb, void* user);

/* Non-zero when no real CAN interface is available for any channel. */
int canfd_hw_is_stub(void);

#ifdef __cplusplus
}
#endif

#endif /* CANFD_HW_H_ */
