#include "canfd_service.h"

#include "canfd_codec.h"
#include "fifo_engine.h"
#include "hw.h"
#include "network_main.h"
#include "protocol.h"
#include "sync_send.h"

#include <cstring>
#include <mutex>
#include <time.h>

namespace {

std::mutex g_service_lock;
bool g_running = false;
network::NetServer* g_cmd_server = nullptr;
network::NetServer* g_data_server = nullptr;

std::mutex g_rx_lock;
canfd_service_rx_cb g_rx_cb = nullptr;
void* g_rx_user = nullptr;

uint32_t g_total_tx = 0;
uint32_t g_ch_tx[CANFD_CHANNEL_COUNT] = {0, 0, 0, 0, 0, 0};

uint64_t now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

static void put_le32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFU);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFU);
}

void write_be64(uint8_t* dest, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        dest[i] = static_cast<uint8_t>((value >> (56 - 8 * i)) & 0xFF);
    }
}

void hw_rx_bridge(uint8_t channel, const canfd_message_t* msg, void* user)
{
    (void)user;
    if (msg == nullptr) {
        return;
    }
    canfd_service_frame_t frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.channel = channel;
    frame.can_id = msg->can_id;
    frame.length = msg->len > 64 ? 64 : msg->len;
    std::memcpy(frame.data, msg->data, frame.length);
    frame.fd = msg->fd;
    frame.brs = msg->brs;
    frame.extended = msg->eff;
    frame.remote = msg->rtr;
    frame.esi = msg->esi;
    frame.timestamp_low16 = msg->timestamp;
    frame.timestamp_us = now_us();

    canfd_service_rx_cb cb;
    void* cb_user;
    {
        std::lock_guard<std::mutex> lock(g_rx_lock);
        cb = g_rx_cb;
        cb_user = g_rx_user;
    }
    if (cb != nullptr) {
        cb(&frame, cb_user);
    }
}

void cleanup_locked()
{
    canfd_hw_set_rx_callback(nullptr, nullptr);
    delete g_data_server;
    g_data_server = nullptr;
    delete g_cmd_server;
    g_cmd_server = nullptr;
    network::SyncSend::GetInstance()->DeInit();
    axififo_data_destory();
    canfd_hw_stop();
    g_running = false;
}

}  // namespace

extern "C" int canfd_service_start(void)
{
    std::lock_guard<std::mutex> lock(g_service_lock);
    if (g_running) {
        return 0;
    }
    if (canfd_hw_start() != 0) {
        return -1;
    }
    if (axififo_data_init() != 0) {
        canfd_hw_stop();
        return -1;
    }
    if (network::SyncSend::GetInstance()->Init() != 0) {
        axififo_data_destory();
        canfd_hw_stop();
        return -1;
    }

    g_cmd_server = new network::NetServer();
    if (g_cmd_server->Init(CMD_PORT) != 0) {
        cleanup_locked();
        return -1;
    }
    g_data_server = new network::NetServer();
    if (g_data_server->Init(DATA_DOWN_PORT) != 0) {
        cleanup_locked();
        return -1;
    }
    canfd_hw_set_rx_callback(hw_rx_bridge, nullptr);
    g_running = true;
    return 0;
}

extern "C" void canfd_service_stop(void)
{
    std::lock_guard<std::mutex> lock(g_service_lock);
    if (!g_running) {
        return;
    }
    cleanup_locked();
}

extern "C" int canfd_service_is_running(void)
{
    std::lock_guard<std::mutex> lock(g_service_lock);
    return g_running ? 1 : 0;
}

extern "C" int canfd_service_send(uint8_t channel,
                                  uint32_t can_id,
                                  const uint8_t* data,
                                  uint8_t length,
                                  int fd,
                                  int brs,
                                  int extended,
                                  int remote,
                                  uint8_t* tx_state)
{
    if (tx_state != nullptr) {
        *tx_state = 0;
    }
    if (channel >= CANFD_CHANNEL_COUNT || (data == nullptr && length != 0) ||
        length > 64 || (remote && fd) ||
        (!extended && can_id > 0x7FF) ||
        (extended && can_id > 0x1FFFFFFF) ||
        (!fd && length > 8)) {
        return -1;
    }

    uint32_t total_count;
    uint32_t ch_count;
    {
        std::lock_guard<std::mutex> lock(g_service_lock);
        if (!g_running) {
            return -1;
        }
        g_total_tx = g_total_tx + 1;
        g_ch_tx[channel] = g_ch_tx[channel] + 1;
        total_count = g_total_tx;
        ch_count = g_ch_tx[channel];
    }

    uint8_t packet[CANFD_FRAME_SIZE];
    std::memset(packet, 0, sizeof(packet));
    put_le32(packet, CANFD_INNER_HEAD);
    packet[4] = 0;                 /* host -> device path */
    packet[5] = 0x01;              /* normal send */
    packet[6] = CANFD_FRAME_SIZE;
    packet[7] = channel;
    put_le32(packet + 8, total_count);
    put_le32(packet + 12, ch_count);

    canfd_message_t msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.can_id = can_id;
    msg.eff = extended ? 1 : 0;
    msg.rtr = remote ? 1 : 0;
    msg.fd = fd ? 1 : 0;
    msg.brs = brs ? 1 : 0;
    msg.len = length;
    if (length != 0) {
        std::memcpy(msg.data, data, length);
    }
    canfd_packet_encode_message(packet, &msg);

    canfd_put_be16(packet + 96, 1);    /* Packets_Sum */
    canfd_put_be16(packet + 98, 0);    /* Packets_No */
    write_be64(packet + 100, now_us());
    canfd_put_be32(packet + 108, canfd_xor(packet, 108));

    const int rc = canfd_hw_submit_frame(channel, 0x01, packet, CANFD_FRAME_SIZE);
    if (tx_state != nullptr) {
        *tx_state = rc == 0 ? 1 : 0;
    }
    return rc;
}

extern "C" void canfd_service_set_rx_callback(canfd_service_rx_cb cb, void* user)
{
    std::lock_guard<std::mutex> lock(g_rx_lock);
    g_rx_cb = cb;
    g_rx_user = user;
}
