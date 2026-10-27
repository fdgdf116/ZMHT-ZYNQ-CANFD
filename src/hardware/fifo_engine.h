#ifndef FIFO_ENGINE_H_
#define FIFO_ENGINE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include "ringbuffer.h"

#define AXI_FIFO_DATA_SIZE ((16 * 1024 + 1) * 512)
#define AXI_FIFO_BUF_SIZE (16 * 1024)
#define AXI_FIFO_BUF_UNIT_SIZE (512)

#pragma pack(1)
typedef struct {
    unsigned char chn_id;
    unsigned int can_space;
    unsigned int pulse_space;
    unsigned int simulation_space;
    unsigned int dmaddr_space;
    unsigned int vfifo_space;
    unsigned int axififo_space;
} Cache_report_inf_t;
#pragma pack()

struct fifo_data_info_t {
    u_int8_t* data;
    ring_buffer_t* fifo_data_rb;
    unsigned int map_size;
    unsigned int ringbuf_offset;
    unsigned int ringbuf_count;
    unsigned int done_count;
    unsigned int dma_offset;
    unsigned int dma_size;
    unsigned int dma_flag;
    unsigned int dma_stop_flag;
};

int axififo_data_init(void);
void axififo_data_destory(void);
int axififo_query_capacity(Cache_report_inf_t* capacity);

#ifdef __cplusplus
}
#endif

#endif /* FIFO_ENGINE_H_ */
