#include "protocol.h"
#include "can_tx_queue.h"
#include "hw.h"
#include "fifo_engine.h"
#include "sync_send.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <time.h>

namespace {

uint32_t le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

/* Some deployed host tools encoded the channel field of CMD_CAN_START_STOP
 * as little-endian, while the protocol uses big-endian.  Keep the wire
 * format canonical (big-endian), but accept the old form when the canonical
 * value cannot be a valid channel. */
uint32_t start_stop_channel(const uint8_t* p)
{
    const uint32_t be = canfd_get_be32(p);
    if (be < CANFD_CHANNEL_COUNT) {
        return be;
    }
    const uint32_t le = le32(p);
    return le < CANFD_CHANNEL_COUNT ? le : be;
}

std::mutex g_state_mu;

struct ChannelState {
    bool configured;
    uint8_t packet[CANFD_FRAME_SIZE];
    uint32_t run_state;
};

ChannelState g_channels[CANFD_CHANNEL_COUNT];

uint64_t now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

void write_be64(uint8_t* dest, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        dest[i] = static_cast<uint8_t>((value >> (56 - 8 * i)) & 0xFF);
    }
}

/* Table 4: 0x80 status feedback echoes the sent frame, adds States/XOR_States
 * and is uploaded on 9010. */
void send_tx_feedback(const uint8_t* frame, bool ok)
{
    uint8_t feedback[CANFD_FRAME_SIZE];
    std::memcpy(feedback, frame, CANFD_FRAME_SIZE);
    feedback[4] = 1;                                    /* device -> host */
    feedback[5] = static_cast<uint8_t>(UPLINK_INDEX_STATUS);
    feedback[96] = ok ? 1 : 0;                           /* States */
    feedback[97] = 1;                                    /* XOR_States: input was valid */
    write_be64(feedback + 100, now_us());
    const uint32_t checksum = canfd_xor(feedback, 108);
    feedback[108] = static_cast<uint8_t>(checksum);
    feedback[109] = static_cast<uint8_t>(checksum >> 8);
    feedback[110] = static_cast<uint8_t>(checksum >> 16);
    feedback[111] = static_cast<uint8_t>(checksum >> 24);
    (void)canfd_send_uplink(UPLINK_INDEX_STATUS, feedback, CANFD_FRAME_SIZE);
}

void put_status(std::vector<uint8_t>* dest, uint32_t status)
{
    uint8_t bytes[4];
    bytes[0] = static_cast<uint8_t>(status);
    bytes[1] = static_cast<uint8_t>(status >> 8);
    bytes[2] = static_cast<uint8_t>(status >> 16);
    bytes[3] = static_cast<uint8_t>(status >> 24);
    dest->assign(bytes, bytes + 4);
}

bool checksum_ok(const uint8_t* frame, size_t size)
{
    if (frame == nullptr || size < 4 || (size % 4) != 0) {
        return false;
    }
    return canfd_xor(frame, size - 4) == le32(frame + size - 4);
}

void write_checksum(uint8_t* frame, size_t size)
{
    const uint32_t value = canfd_xor(frame, size - 4);
    frame[size - 4] = static_cast<uint8_t>(value);
    frame[size - 3] = static_cast<uint8_t>(value >> 8);
    frame[size - 2] = static_cast<uint8_t>(value >> 16);
    frame[size - 1] = static_cast<uint8_t>(value >> 24);
}

bool init_packet_ok(const uint8_t* frame, uint32_t length)
{
    if (frame == nullptr || length != CANFD_FRAME_SIZE) {
        return false;
    }
    if (le32(frame) != CANFD_INNER_HEAD) {
        return false;
    }
    if (frame[4] != 0 || frame[5] != 0x00 || frame[6] != CANFD_FRAME_SIZE) {
        return false;
    }
    if (frame[7] >= CANFD_CHANNEL_COUNT) {
        return false;
    }
    const uint8_t mode = frame[20];
    if (mode != 0x02 && mode != 0x04) {
        return false;
    }
    const uint8_t brs = frame[32];
    if (brs != 0 && brs != 1) {
        return false;
    }
    const uint32_t baud = le32(frame + 16);
    if (baud < 1 || baud > 1000000u) {
        return false;
    }
    const uint32_t data_baud = le32(frame + 28);
    if (!(data_baud == 0 || (data_baud >= baud && data_baud <= 8000000u))) {
        return false;
    }
    return checksum_ok(frame, CANFD_FRAME_SIZE);
}

bool data_frame_ok(const uint8_t* frame, uint8_t expect_type)
{
    if (frame == nullptr) {
        return false;
    }
    if (le32(frame) != CANFD_INNER_HEAD) {
        return false;
    }
    if (frame[4] != 0 || frame[5] != expect_type || frame[6] != CANFD_FRAME_SIZE) {
        return false;
    }
    if (frame[7] >= CANFD_CHANNEL_COUNT) {
        return false;
    }
    if (!checksum_ok(frame, CANFD_FRAME_SIZE)) {
        return false;
    }
    return true;
}

bool simulator_ok(const uint8_t* packet, uint32_t length)
{
    if (packet == nullptr || length < 32 || length > 0xFFFFu) {
        return false;
    }
    if (le32(packet) != CANFD_INNER_HEAD) {
        return false;
    }
    if (packet[4] != 0 || packet[5] != 0x03) {
        return false;
    }
    if (canfd_get_be16(packet + 6) != length) {
        return false;
    }
    if (packet[8] >= CANFD_CHANNEL_COUNT) {
        return false;
    }
    return canfd_get_be16(packet + 29) == static_cast<uint16_t>(length - 32);
}

int submit_frames(const uint8_t* payload, uint32_t length, uint8_t expect_type)
{
    if (payload == nullptr || length == 0 || (length % CANFD_FRAME_SIZE) != 0) {
        return -1;
    }
    const uint32_t count = length / CANFD_FRAME_SIZE;
    for (uint32_t i = 0; i < count; ++i) {
        if (!data_frame_ok(payload + i * CANFD_FRAME_SIZE, expect_type)) {
            return -1;
        }
    }
    int result = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* frame = payload + i * CANFD_FRAME_SIZE;
        const int rc = can_tx_queue_push(frame[7], expect_type, frame);
        if (rc != 0) {
            static auto last_queue_error = std::chrono::steady_clock::time_point();
            const auto now = std::chrono::steady_clock::now();
            if (last_queue_error.time_since_epoch().count() == 0 ||
                now - last_queue_error >= std::chrono::seconds(5)) {
                std::printf("[CANFD 9012] 入队失败：通道=%u，缓冲区已满或已关闭\n",
                            static_cast<unsigned>(frame[7]));
                last_queue_error = now;
            }
            send_tx_feedback(frame, false);
            result = -2;
        }
    }
    return result;
}

CommandResponse respond_with(const NetHeader& header, std::vector<uint8_t> payload)
{
    CommandResponse response;
    response.index = header.index;
    response.count = header.count;
    response.time = header.time;
    response.payload = payload;
    return response;
}

}  // namespace

void canfd_send_tx_feedback(const uint8_t* frame, bool ok)
{
    send_tx_feedback(frame, ok);
}

void canfd_put_be16(uint8_t* dest, uint16_t value)
{
    dest[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dest[1] = static_cast<uint8_t>(value & 0xFF);
}

void canfd_put_be32(uint8_t* dest, uint32_t value)
{
    dest[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    dest[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dest[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dest[3] = static_cast<uint8_t>(value & 0xFF);
}

uint16_t canfd_get_be16(const uint8_t* src)
{
    return static_cast<uint16_t>((src[0] << 8) | src[1]);
}

uint32_t canfd_get_be32(const uint8_t* src)
{
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) |
           static_cast<uint32_t>(src[3]);
}

uint32_t canfd_xor(const uint8_t* data, size_t size_without_checksum)
{
    uint32_t value = 0;
    for (size_t i = 0; i + 4 <= size_without_checksum; i += 4) {
        value ^= le32(data + i);
    }
    return value;
}

void net_header_encode(uint8_t out[16], const NetHeader& header)
{
    canfd_put_be32(out + 0, header.cmd_code);
    canfd_put_be32(out + 4, header.length);
    canfd_put_be16(out + 8, header.count);
    canfd_put_be16(out + 10, header.index);
    canfd_put_be16(out + 12, header.time);
    canfd_put_be16(out + 14, header.crc);
}

void net_header_decode(const uint8_t in[16], NetHeader* header)
{
    header->cmd_code = canfd_get_be32(in + 0);
    header->length = canfd_get_be32(in + 4);
    header->count = canfd_get_be16(in + 8);
    header->index = canfd_get_be16(in + 10);
    header->time = canfd_get_be16(in + 12);
    header->crc = canfd_get_be16(in + 14);
}

bool net_header_accept(const NetHeader& header, uint32_t expect_code, uint32_t max_payload)
{
    return header.cmd_code == expect_code && header.length <= max_payload;
}

void canfd_state_reset()
{
    std::lock_guard<std::mutex> lock(g_state_mu);
    std::memset(g_channels, 0, sizeof(g_channels));
}

CommandResponse canfd_cmd_status(const NetHeader& header, uint32_t status)
{
    std::vector<uint8_t> body;
    put_status(&body, status);
    return respond_with(header, body);
}

CommandResponse canfd_cmd_config(const NetHeader& header, const uint8_t* payload, uint32_t length)
{
    std::lock_guard<std::mutex> lock(g_state_mu);
    if (!init_packet_ok(payload, length)) {
        std::printf("[CANFD CMD] config reject ch=%u len=%u\n",
                    payload != nullptr && length > 7 ? payload[7] : 0xFFu,
                    length);
        return canfd_cmd_status(header, 0xBB);
    }
    const uint8_t channel = payload[7];
    if (canfd_hw_apply_config(channel, payload, CANFD_FRAME_SIZE) != 0) {
        std::printf("[CANFD CMD] config failed ch=%u\n", channel);
        return canfd_cmd_status(header, 0xBB);
    }
    std::memcpy(g_channels[channel].packet, payload, CANFD_FRAME_SIZE);
    g_channels[channel].configured = true;
    return canfd_cmd_status(header, 0xAA);
}

CommandResponse canfd_cmd_info_query(const NetHeader& header, const uint8_t* payload, uint32_t length)
{
    std::lock_guard<std::mutex> lock(g_state_mu);
    if (payload == nullptr || length != 4 || canfd_get_be32(payload) != 0xFFFFFFFFu) {
        return canfd_cmd_status(header, 1);
    }
    std::vector<uint8_t> body(CANFD_CHANNEL_COUNT * CANFD_FRAME_SIZE, 0);
    for (uint8_t ch = 0; ch < CANFD_CHANNEL_COUNT; ++ch) {
        if (!g_channels[ch].configured) {
            continue;
        }
        uint8_t* dest = body.data() + ch * CANFD_FRAME_SIZE;
        std::memcpy(dest, g_channels[ch].packet, CANFD_FRAME_SIZE);
        dest[4] = 1;
        write_checksum(dest, CANFD_FRAME_SIZE);
    }
    return respond_with(header, body);
}

CommandResponse canfd_cmd_start_stop(const NetHeader& header, const uint8_t* payload, uint32_t length)
{
    std::lock_guard<std::mutex> lock(g_state_mu);
    if (payload == nullptr || length != 8) {
        std::printf("[CANFD CMD] run reject len=%u\n", length);
        return canfd_cmd_status(header, 1);
    }
    const uint32_t channel = start_stop_channel(payload);
    const uint32_t state = canfd_get_be32(payload + 4);
    if (channel >= CANFD_CHANNEL_COUNT || (state != 0xAA && state != 0xBB)) {
        std::printf("[CANFD CMD] run reject ch=%u state=0x%08X\n", channel, state);
        return canfd_cmd_status(header, 1);
    }
    g_channels[channel].run_state = state;
    if (canfd_hw_set_run(static_cast<uint8_t>(channel), state) != 0) {
        std::printf("[CANFD CMD] run failed ch=%u state=0x%08X\n", channel, state);
        return canfd_cmd_status(header, 1);
    }
    return canfd_cmd_status(header, 0);
}

CommandResponse canfd_cmd_buf_query(const NetHeader& header, const uint8_t* payload, uint32_t length)
{
    std::lock_guard<std::mutex> lock(g_state_mu);
    if (payload == nullptr || length != 4 || canfd_get_be32(payload) != 0xFFFFFFFFu) {
        return canfd_cmd_status(header, 1);
    }
    if (axififo_data_init() != 0) {
        return canfd_cmd_status(header, 1);
    }
    Cache_report_inf_t capacity[CANFD_CHANNEL_COUNT];
    std::memset(capacity, 0, sizeof(capacity));
    if (axififo_query_capacity(capacity) != 0) {
        return canfd_cmd_status(header, 1);
    }
    std::vector<uint8_t> body(CANFD_CHANNEL_COUNT * 16, 0);
    for (uint8_t ch = 0; ch < CANFD_CHANNEL_COUNT; ++ch) {
        body[ch * 16] = ch;
        uint8_t* p = body.data() + ch * 16 + 4;
        p[0]=static_cast<uint8_t>(capacity[ch].can_space); p[1]=static_cast<uint8_t>(capacity[ch].can_space>>8); p[2]=static_cast<uint8_t>(capacity[ch].can_space>>16); p[3]=static_cast<uint8_t>(capacity[ch].can_space>>24);
        p = body.data() + ch * 16 + 8;
        p[0]=static_cast<uint8_t>(capacity[ch].pulse_space); p[1]=static_cast<uint8_t>(capacity[ch].pulse_space>>8); p[2]=static_cast<uint8_t>(capacity[ch].pulse_space>>16); p[3]=static_cast<uint8_t>(capacity[ch].pulse_space>>24);
        p = body.data() + ch * 16 + 12;
        p[0]=static_cast<uint8_t>(capacity[ch].simulation_space); p[1]=static_cast<uint8_t>(capacity[ch].simulation_space>>8); p[2]=static_cast<uint8_t>(capacity[ch].simulation_space>>16); p[3]=static_cast<uint8_t>(capacity[ch].simulation_space>>24);
    }
    return respond_with(header, body);
}

CommandResponse canfd_cmd_version(const NetHeader& header, const uint8_t* payload, uint32_t length)
{
    uint32_t version = CANFD_VERSION_FAIL;
    if (payload != nullptr && length == 4 && canfd_get_be32(payload) == CANFD_VERSION_ARM) {
        version = CANFD_APP_VERSION;
    }
    uint8_t bytes[4];
    bytes[0]=static_cast<uint8_t>(version); bytes[1]=static_cast<uint8_t>(version>>8); bytes[2]=static_cast<uint8_t>(version>>16); bytes[3]=static_cast<uint8_t>(version>>24);
    return respond_with(header, std::vector<uint8_t>(bytes, bytes + 4));
}

int canfd_data_normal(const uint8_t* payload, uint32_t length)
{
    return submit_frames(payload, length, 0x01);
}

int canfd_data_pulse(const uint8_t* payload, uint32_t length)
{
    return submit_frames(payload, length, 0x02);
}

int canfd_data_simulator(const uint8_t* payload, uint32_t length)
{
    if (!simulator_ok(payload, length)) {
        return -1;
    }
    if (canfd_hw_submit_simulator(payload[8], payload, length) != 0) {
        return -1;
    }
    return 0;
}
