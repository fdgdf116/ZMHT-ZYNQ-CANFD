#ifndef CAN_TX_QUEUE_H_
#define CAN_TX_QUEUE_H_

#include <stdint.h>

int can_tx_queue_start(void);
void can_tx_queue_stop(void);

/* 0x01, 0x02 and 0x03 select the normal, pulse and simulator queues.
 * Returns -1 immediately when the selected queue is full. */
int can_tx_queue_push(uint8_t channel, uint8_t type,
                      const uint8_t frame[112]);

int can_tx_queue_query_capacity(uint8_t channel,
                                uint32_t* normal_space,
                                uint32_t* pulse_space,
                                uint32_t* simulation_space);

#endif /* CAN_TX_QUEUE_H_ */
