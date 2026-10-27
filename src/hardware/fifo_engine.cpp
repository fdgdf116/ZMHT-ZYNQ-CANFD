#include "fifo_engine.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct fifo_data_info_t can_info[CANFD_CHANNEL_COUNT];
    struct fifo_data_info_t pulse_info[CANFD_CHANNEL_COUNT];
    struct fifo_data_info_t simulation_info[CANFD_CHANNEL_COUNT];
} axififo_info_t;

static axififo_info_t* g_axififo_info;

static void release_stream(struct fifo_data_info_t* info)
{
    if (info->fifo_data_rb != NULL) {
        ringbuffer_deinit(info->fifo_data_rb);
        info->fifo_data_rb = NULL;
    }
    free(info->data);
    info->data = NULL;
}

void axififo_data_destory(void)
{
    if (g_axififo_info == NULL) {
        return;
    }
    for (unsigned chn_id = 0; chn_id < CANFD_CHANNEL_COUNT; ++chn_id) {
        release_stream(&g_axififo_info->can_info[chn_id]);
        release_stream(&g_axififo_info->pulse_info[chn_id]);
        release_stream(&g_axififo_info->simulation_info[chn_id]);
    }
    free(g_axififo_info);
    g_axififo_info = NULL;
}

static int create_stream(struct fifo_data_info_t* info)
{
    info->map_size = AXI_FIFO_DATA_SIZE;
    info->data = (u_int8_t*)malloc(info->map_size);
    if (info->data == NULL) {
        printf("[ERROR] axififo data malloc error \n");
        return -1;
    }
    memset(info->data, 0, info->map_size);
    info->fifo_data_rb = ringbuffer_create(AXI_FIFO_BUF_SIZE * sizeof(image_frame_info_t),
                                           sizeof(image_frame_info_t));
    if (info->fifo_data_rb == NULL) {
        printf("[ERROR] axififo data ringbuffer create error \n");
        return -1;
    }
    return 0;
}

int axififo_data_init(void)
{
    if (g_axififo_info != NULL) {
        return 0;
    }
    g_axififo_info = (axififo_info_t*)malloc(sizeof(axififo_info_t));
    if (g_axififo_info == NULL) {
        printf("[ERROR] axififo info malloc error \n");
        return -1;
    }
    memset(g_axififo_info, 0, sizeof(*g_axififo_info));
    for (unsigned chn_id = 0; chn_id < CANFD_CHANNEL_COUNT; ++chn_id) {
        if (create_stream(&g_axififo_info->can_info[chn_id]) != 0 ||
            create_stream(&g_axififo_info->pulse_info[chn_id]) != 0 ||
            create_stream(&g_axififo_info->simulation_info[chn_id]) != 0) {
            axififo_data_destory();
            return -1;
        }
    }
    printf("[CANFD] ringbuffer ready channels=%d slots=%u unit=%u\n",
           CANFD_CHANNEL_COUNT, AXI_FIFO_BUF_SIZE, AXI_FIFO_BUF_UNIT_SIZE);
    return 0;
}

int axififo_query_capacity(Cache_report_inf_t* capacity)
{
    if (g_axififo_info == NULL || capacity == NULL) {
        return -1;
    }
    for (unsigned chn_id = 0; chn_id < CANFD_CHANNEL_COUNT; ++chn_id) {
        capacity[chn_id].chn_id = (unsigned char)chn_id;
        capacity[chn_id].can_space =
            ringbuffer_avail(g_axififo_info->can_info[chn_id].fifo_data_rb) * AXI_FIFO_BUF_UNIT_SIZE;
        capacity[chn_id].pulse_space =
            ringbuffer_avail(g_axififo_info->pulse_info[chn_id].fifo_data_rb) * AXI_FIFO_BUF_UNIT_SIZE;
        capacity[chn_id].simulation_space =
            ringbuffer_avail(g_axififo_info->simulation_info[chn_id].fifo_data_rb) * AXI_FIFO_BUF_UNIT_SIZE;
        capacity[chn_id].dmaddr_space = 0;
        capacity[chn_id].vfifo_space = 0;
        capacity[chn_id].axififo_space = 0;
    }
    return 0;
}
