#include "can_tx_queue.h"

#include "fifo_engine.h"
#include "hw.h"
#include "protocol.h"
#include "ringbuffer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

constexpr int kQueueCount = 3;

struct TxFrame {
    uint8_t type;
    uint8_t data[CANFD_FRAME_SIZE];
    uint8_t padding[AXI_FIFO_BUF_UNIT_SIZE - 1 - CANFD_FRAME_SIZE];
};
static_assert(sizeof(TxFrame) == AXI_FIFO_BUF_UNIT_SIZE,
              "TX queue slot must match the reported capacity unit");

struct ChannelQueue {
    std::mutex mutex;
    std::condition_variable data_cv;
    ring_buffer_t* queues[kQueueCount] = {};
    uint32_t next_queue = 0;
    bool stop = false;
    std::thread worker;
};

ChannelQueue g_queues[CANFD_CHANNEL_COUNT];
std::mutex g_lifecycle_mutex;
std::atomic<bool> g_started(false);

int queue_index(uint8_t type)
{
    if (type == 0x01) return 0;
    if (type == 0x02) return 1;
    if (type == 0x03) return 2;
    return -1;
}

bool has_work_locked(ChannelQueue* channel)
{
    for (int q = 0; q < kQueueCount; ++q) {
        if (!ringbuffer_is_empty(channel->queues[q])) return true;
    }
    return false;
}

void tx_worker(uint8_t channel_id)
{
    ChannelQueue* channel = &g_queues[channel_id];
    for (;;) {
        TxFrame item = {};
        {
            std::unique_lock<std::mutex> lock(channel->mutex);
            channel->data_cv.wait(lock, [channel] {
                return channel->stop || has_work_locked(channel);
            });
            if (channel->stop) return;

            /* Keep queued frames buffered while the corresponding interface
             * is closed.  The worker checks the interface before consuming. */
            if (!canfd_hw_channel_is_open(channel_id)) {
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            bool found = false;
            for (int i = 0; i < kQueueCount; ++i) {
                const uint32_t q =
                    (channel->next_queue + static_cast<uint32_t>(i)) %
                    static_cast<uint32_t>(kQueueCount);
                if (!ringbuffer_is_empty(channel->queues[q])) {
                    if (ringbuffer_get(channel->queues[q], &item,
                                       sizeof(item)) != 1) {
                        break;
                    }
                    channel->next_queue =
                        (q + 1U) % static_cast<uint32_t>(kQueueCount);
                    found = true;
                    break;
                }
            }
            if (!found) continue;
        }

        int rc = -1;
        if (item.type == 0x03) {
            rc = canfd_hw_submit_simulator(channel_id, item.data,
                                           CANFD_FRAME_SIZE);
        } else {
            rc = canfd_hw_submit_frame(channel_id, item.type, item.data,
                                       CANFD_FRAME_SIZE);
        }
        /* Failed writes, including ENOBUFS, are dropped and reported as
         * States=0.  A closed interface is checked before dequeue. */
        canfd_send_tx_feedback(item.data, rc == 0);
    }
}

void release_queues(ChannelQueue* channel)
{
    for (int q = 0; q < kQueueCount; ++q) {
        if (channel->queues[q] != nullptr) {
            ringbuffer_deinit(channel->queues[q]);
            channel->queues[q] = nullptr;
        }
    }
}

}  // namespace

int can_tx_queue_start(void)
{
    std::lock_guard<std::mutex> lifecycle(g_lifecycle_mutex);
    if (g_started) return 0;

    for (uint8_t channel = 0; channel < CANFD_CHANNEL_COUNT; ++channel) {
        ChannelQueue* q = &g_queues[channel];
        q->stop = false;
        q->next_queue = 0;
        for (int i = 0; i < kQueueCount; ++i) {
            q->queues[i] = ringbuffer_create(
                AXI_FIFO_BUF_SIZE * sizeof(TxFrame), sizeof(TxFrame));
            if (q->queues[i] == nullptr) {
                std::printf("[CANFD] 通道%u类型%d缓冲区创建失败（需要%u字节）\n",
                            static_cast<unsigned>(channel), i,
                            AXI_FIFO_DATA_SIZE);
                for (uint8_t ch = 0; ch <= channel; ++ch) {
                    release_queues(&g_queues[ch]);
                }
                return -1;
            }
        }
    }

    for (uint8_t channel = 0; channel < CANFD_CHANNEL_COUNT; ++channel) {
        g_queues[channel].worker = std::thread(tx_worker, channel);
        ChannelQueue* q = &g_queues[channel];
        const uint32_t normal_frames = ringbuffer_cap(q->queues[0]);
        const uint32_t pulse_frames = ringbuffer_cap(q->queues[1]);
        const uint32_t simulation_frames = ringbuffer_cap(q->queues[2]);
        const uint32_t normal_bytes =
            normal_frames * AXI_FIFO_BUF_UNIT_SIZE;
        const uint32_t pulse_bytes =
            pulse_frames * AXI_FIFO_BUF_UNIT_SIZE;
        const uint32_t simulation_bytes =
            simulation_frames * AXI_FIFO_BUF_UNIT_SIZE;
        std::printf("[CANFD] 通道%u初始化完成：普通=%u帧(%u字节)，秒脉冲=%u帧(%u字节)，模拟器=%u帧(%u字节)，发送线程已创建\n",
                    static_cast<unsigned>(channel),
                    normal_frames, normal_bytes,
                    pulse_frames, pulse_bytes,
                    simulation_frames, simulation_bytes);
    }
    g_started = true;
    return 0;
}

void can_tx_queue_stop(void)
{
    std::lock_guard<std::mutex> lifecycle(g_lifecycle_mutex);
    if (!g_started) return;
    for (uint8_t channel = 0; channel < CANFD_CHANNEL_COUNT; ++channel) {
        ChannelQueue* q = &g_queues[channel];
        {
            std::lock_guard<std::mutex> lock(q->mutex);
            q->stop = true;
        }
        q->data_cv.notify_all();
    }
    for (uint8_t channel = 0; channel < CANFD_CHANNEL_COUNT; ++channel) {
        if (g_queues[channel].worker.joinable()) {
            g_queues[channel].worker.join();
        }
    }
    for (uint8_t channel = 0; channel < CANFD_CHANNEL_COUNT; ++channel) {
        std::lock_guard<std::mutex> lock(g_queues[channel].mutex);
        release_queues(&g_queues[channel]);
    }
    g_started = false;
}

int can_tx_queue_push(uint8_t channel, uint8_t type,
                      const uint8_t frame[CANFD_FRAME_SIZE])
{
    if (channel >= CANFD_CHANNEL_COUNT || frame == nullptr) return -1;
    const int q = queue_index(type);
    if (q < 0) return -1;

    ChannelQueue* queue = &g_queues[channel];
    {
        std::lock_guard<std::mutex> lock(queue->mutex);
        if (!g_started.load() || queue->stop ||
            queue->queues[q] == nullptr ||
            ringbuffer_avail(queue->queues[q]) == 0) {
            return -1;
        }
        TxFrame item = {};
        item.type = type;
        std::memcpy(item.data, frame, CANFD_FRAME_SIZE);
        if (ringbuffer_put(queue->queues[q], &item, sizeof(item)) != 1) {
            return -1;
        }
    }
    queue->data_cv.notify_one();
    return 0;
}

int can_tx_queue_query_capacity(uint8_t channel,
                                uint32_t* normal_space,
                                uint32_t* pulse_space,
                                uint32_t* simulation_space)
{
    if (channel >= CANFD_CHANNEL_COUNT ||
        normal_space == nullptr || pulse_space == nullptr ||
        simulation_space == nullptr || !g_started.load()) {
        return -1;
    }
    ChannelQueue* queue = &g_queues[channel];
    std::lock_guard<std::mutex> lock(queue->mutex);
    uint32_t* spaces[kQueueCount] = {
        normal_space, pulse_space, simulation_space
    };
    for (int q = 0; q < kQueueCount; ++q) {
        if (queue->queues[q] == nullptr) return -1;
        *spaces[q] =
            ringbuffer_avail(queue->queues[q]) * AXI_FIFO_BUF_UNIT_SIZE;
    }
    return 0;
}
