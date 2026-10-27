#ifndef _RINGBUFFER_H_
#define _RINGBUFFER_H_

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t* data;
    uint32_t in;
    uint32_t out;
    uint32_t mask;
    uint32_t esize;
} ring_buffer_t;

typedef struct {
    volatile unsigned int frame_size;
    volatile unsigned int frame_offset;
} image_frame_info_t;

#ifdef __cplusplus
extern "C" {
#endif

ring_buffer_t* ringbuffer_create(uint32_t size, uint32_t esize);
bool ringbuffer_is_full(ring_buffer_t* rb);
bool ringbuffer_is_empty(ring_buffer_t* rb);
uint32_t ringbuffer_put(ring_buffer_t* rb, const void* buf, uint32_t len);
uint32_t ringbuffer_get(ring_buffer_t* rb, void* buf, uint32_t len);
uint32_t ringbuffer_len(ring_buffer_t* rb);
uint32_t ringbuffer_cap(ring_buffer_t* rb);
uint32_t ringbuffer_avail(ring_buffer_t* rb);
void ringbuffer_reset(ring_buffer_t* rb);
void ringbuffer_deinit(ring_buffer_t* rb);
void pcie_data_to_queue(ring_buffer_t* rb, image_frame_info_t* frame_info);

#ifdef __cplusplus
}
#endif

#endif /* _RINGBUFFER_H_ */
