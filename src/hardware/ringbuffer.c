#include "ringbuffer.h"

#include <stdio.h>

#include <stdlib.h>
#include <string.h>

#define rb_min(X, Y)                \
    ({ typeof(X) __x = (X);         \
        typeof(Y) __y = (Y);        \
        (__x < __y) ? __x : __y; })

static inline int is_power_of_2(uint32_t n)
{
    return (n != 0 && ((n & (n - 1)) == 0));
}

static inline uint32_t rounddown_pow_of_two(uint32_t n)
{
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return (n + 1) >> 1;
}

void pcie_data_to_queue(ring_buffer_t* rb, image_frame_info_t* frame_info)
{
    image_frame_info_t frame_param, tmp_param;
    if (ringbuffer_is_full(rb)) {
        printf("[%s,%d] ringbuffer is full get\n", __func__, __LINE__);
        ringbuffer_get(rb, (uint8_t*)&tmp_param, sizeof(tmp_param));
    }
    memcpy(&frame_param, frame_info, sizeof(image_frame_info_t));
    ringbuffer_put(rb, (uint8_t*)&frame_param, sizeof(frame_param));
}

ring_buffer_t* ringbuffer_create(uint32_t size, uint32_t esize)
{
    ring_buffer_t* rb = NULL;
    uint8_t* buf = NULL;
    uint32_t buf_size = 0;

    if (esize == 0) {
        return NULL;
    }

    rb = (ring_buffer_t*)malloc(sizeof(*rb));
    if (rb == NULL) {
        return NULL;
    }
    memset(rb, 0, sizeof(*rb));

    size /= esize;
    if (!is_power_of_2(size)) {
        size = rounddown_pow_of_two(size);
    }

    buf_size = esize * size;
    buf = (uint8_t*)malloc(buf_size);
    if (buf == NULL) {
        free(rb);
        return NULL;
    }
    memset(buf, 0, buf_size);

    rb->in = 0;
    rb->out = 0;
    rb->esize = esize;
    rb->data = buf;

    if (size < 2) {
        free(rb);
        free(buf);
        return NULL;
    }

    rb->mask = size - 1;
    return rb;
}

static void ringbuffer_copy_in(ring_buffer_t* rb, const void* src, uint32_t len, uint32_t off)
{
    uint32_t size = rb->mask + 1;
    uint32_t esize = rb->esize;
    uint32_t l;

    off &= rb->mask;
    if (esize != 1) {
        off *= esize;
        size *= esize;
        len *= esize;
    }
    l = rb_min(len, size - off);

    memcpy(rb->data + off, src, l);
    memcpy(rb->data, (const uint8_t*)src + l, len - l);
    __sync_synchronize();
}

static inline unsigned int ringbuffer_unused(ring_buffer_t* rb)
{
    return (rb->mask + 1) - (rb->in - rb->out);
}

uint32_t ringbuffer_put(ring_buffer_t* rb, const void* buf, uint32_t len)
{
    uint32_t l;
    /* Same as the 422 ring: one element is stored per call. */
    uint32_t count = (len >= rb->esize) ? 1 : 1;

    l = ringbuffer_unused(rb);
    if (count > l) {
        count = l;
    }

    ringbuffer_copy_in(rb, buf, count, rb->in);
    rb->in += count;
    return count;
}

static void ringbuffer_copy_out(ring_buffer_t* rb, void* dst, uint32_t len, uint32_t off)
{
    uint32_t size = rb->mask + 1;
    uint32_t esize = rb->esize;
    uint32_t l;

    off &= rb->mask;
    if (esize != 1) {
        off *= esize;
        size *= esize;
        len *= esize;
    }
    l = rb_min(len, size - off);

    memcpy(dst, rb->data + off, l);
    memcpy((uint8_t*)dst + l, rb->data, len - l);
    __sync_synchronize();
}

static uint32_t ringbuffer_out_peek(ring_buffer_t* rb, void* buf, uint32_t len)
{
    uint32_t l;

    l = rb->in - rb->out;
    if (len > l) {
        len = l;
    }

    ringbuffer_copy_out(rb, buf, len, rb->out);
    return len;
}

uint32_t ringbuffer_get(ring_buffer_t* rb, void* buf, uint32_t len)
{
    uint32_t count = (len >= rb->esize) ? 1 : 1;

    count = ringbuffer_out_peek(rb, buf, count);
    rb->out += count;
    return count;
}

uint32_t ringbuffer_len(ring_buffer_t* rb)
{
    return rb->in - rb->out;
}

uint32_t ringbuffer_cap(ring_buffer_t* rb)
{
    return rb->mask + 1;
}

uint32_t ringbuffer_avail(ring_buffer_t* rb)
{
    return ringbuffer_cap(rb) - ringbuffer_len(rb);
}

bool ringbuffer_is_full(ring_buffer_t* rb)
{
    return ringbuffer_len(rb) > rb->mask;
}

bool ringbuffer_is_empty(ring_buffer_t* rb)
{
    return rb->in == rb->out;
}

void ringbuffer_reset(ring_buffer_t* rb)
{
    rb->in = 0;
    rb->out = 0;
}

void ringbuffer_deinit(ring_buffer_t* rb)
{
    if (rb == NULL) {
        return;
    }
    free(rb->data);
    rb->data = NULL;
    free(rb);
}
