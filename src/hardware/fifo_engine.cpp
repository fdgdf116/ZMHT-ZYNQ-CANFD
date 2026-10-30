#include "fifo_engine.h"
#include "can_tx_queue.h"
#include "protocol.h"

#include <stdio.h>

typedef struct {
    struct fifo_data_info_t can_info[CANFD_CHANNEL_COUNT];
    struct fifo_data_info_t pulse_info[CANFD_CHANNEL_COUNT];
    struct fifo_data_info_t simulation_info[CANFD_CHANNEL_COUNT];
} axififo_info_t;

static axififo_info_t* g_axififo_info;

void axififo_data_destory(void)
{
    if (g_axififo_info == nullptr) return;
    can_tx_queue_stop();
    g_axififo_info = NULL;
}

int axififo_data_init(void)
{
    if (g_axififo_info != nullptr) return 0;
    if (can_tx_queue_start() != 0) return -1;
    static axififo_info_t info;
    g_axififo_info = &info;
    printf("[CANFD] 发送缓冲区初始化完成：通道=%d，每类=%u帧(%u字节)，"
           "每通道三类=%u MiB，总计=%u MiB\n",
           CANFD_CHANNEL_COUNT, AXI_FIFO_BUF_SIZE, AXI_FIFO_DATA_SIZE,
           AXI_FIFO_DATA_SIZE * 3 / (1024 * 1024),
           AXI_FIFO_DATA_SIZE * 3 * CANFD_CHANNEL_COUNT / (1024 * 1024));
    return 0;
}

int axififo_query_capacity(Cache_report_inf_t* capacity)
{
    if (g_axififo_info == NULL || capacity == NULL) {
        return -1;
    }
    for (unsigned chn_id = 0; chn_id < CANFD_CHANNEL_COUNT; ++chn_id) {
        uint32_t normal_space = 0;
        uint32_t pulse_space = 0;
        uint32_t simulation_space = 0;
        if (can_tx_queue_query_capacity(chn_id, &normal_space,
                                        &pulse_space,
                                        &simulation_space) != 0) {
            return -1;
        }
        capacity[chn_id].chn_id = (unsigned char)chn_id;
        capacity[chn_id].can_space = normal_space;
        capacity[chn_id].pulse_space = pulse_space;
        capacity[chn_id].simulation_space = simulation_space;
        capacity[chn_id].dmaddr_space = 0;
        capacity[chn_id].vfifo_space = 0;
        capacity[chn_id].axififo_space = 0;
    }
    return 0;
}
