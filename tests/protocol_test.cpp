#include "protocol.h"
#include "fifo_engine.h"
#include "canfd_codec.h"
#include "hw.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_failed = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failed;                                                     \
        }                                                                   \
    } while (0)

static void fill_init(uint8_t frame[112], uint8_t channel, uint32_t baud,
                      uint32_t data_baud, uint8_t mode, uint8_t brs)
{
    std::memset(frame, 0, 112);
    canfd_put_be32(frame, CANFD_INNER_HEAD);
    frame[4] = 0;
    frame[5] = 0x00;
    frame[6] = 112;
    frame[7] = channel;
    canfd_put_be32(frame + 16, baud);
    frame[20] = mode;
    canfd_put_be32(frame + 28, data_baud);
    frame[32] = brs;
    canfd_put_be32(frame + 108, canfd_xor(frame, 108));
}

static void fill_data(uint8_t frame[112], uint8_t channel, uint8_t type)
{
    std::memset(frame, 0, 112);
    canfd_put_be32(frame, CANFD_INNER_HEAD);
    frame[4] = 0;
    frame[5] = type;
    frame[6] = 112;
    frame[7] = channel;
    canfd_put_be32(frame + 108, canfd_xor(frame, 108));
}

static CommandResponse command(uint16_t index, const uint8_t* payload, uint32_t length)
{
    NetHeader header;
    std::memset(&header, 0, sizeof(header));
    header.cmd_code = NET_CMD_CODE;
    header.length = length;
    header.count = 0x2222;
    header.index = index;
    header.time = 0x3333;
    return canfd_handle_command(header, payload, length);
}

static uint32_t status_of(const CommandResponse& response)
{
    CHECK(response.payload.size() == 4);
    if (response.payload.size() != 4) {
        return 0xFFFFFFFFu;
    }
    return canfd_get_be32(response.payload.data());
}

static void test_header_roundtrip()
{
    NetHeader header;
    header.cmd_code = NET_CMD_CODE;
    header.length = 112;
    header.count = 0x1234;
    header.index = CMD_CAN_CONFIG;
    header.time = 0xABCD;
    header.crc = 0;
    uint8_t raw[16];
    net_header_encode(raw, header);
    const uint8_t expect[16] = {
        0x03, 0xCC, 0xF0, 0xFF, 0x00, 0x00, 0x00, 0x70,
        0x12, 0x34, 0x01, 0x01, 0xAB, 0xCD, 0x00, 0x00
    };
    CHECK(std::memcmp(raw, expect, 16) == 0);
    NetHeader decoded;
    net_header_decode(raw, &decoded);
    CHECK(decoded.cmd_code == header.cmd_code);
    CHECK(decoded.length == header.length);
    CHECK(decoded.count == header.count);
    CHECK(decoded.index == header.index);
    CHECK(decoded.time == header.time);
    CHECK(decoded.crc == header.crc);

    header.crc = 0xFFFF;
    CHECK(net_header_accept(header, NET_CMD_CODE, CANFD_CMD_MAX_PAYLOAD));
    header.length = 4096;
    CHECK(net_header_accept(header, NET_CMD_CODE, CANFD_CMD_MAX_PAYLOAD));
    header.length = 4097;
    CHECK(!net_header_accept(header, NET_CMD_CODE, CANFD_CMD_MAX_PAYLOAD));
    header.length = 8192;
    CHECK(net_header_accept(header, NET_DATA_CODE, CANFD_DATA_MAX_PAYLOAD) == false);
    header.cmd_code = NET_DATA_CODE;
    CHECK(net_header_accept(header, NET_DATA_CODE, CANFD_DATA_MAX_PAYLOAD));
    header.length = 8193;
    CHECK(!net_header_accept(header, NET_DATA_CODE, CANFD_DATA_MAX_PAYLOAD));
    header.length = 112;
    header.cmd_code = 0x11111111;
    CHECK(!net_header_accept(header, NET_CMD_CODE, CANFD_CMD_MAX_PAYLOAD));
    CHECK(!net_header_accept(header, NET_DATA_CODE, CANFD_DATA_MAX_PAYLOAD));
}

static void test_xor()
{
    uint8_t frame[112];
    std::memset(frame, 0, sizeof(frame));
    canfd_put_be32(frame, CANFD_INNER_HEAD);
    CHECK(frame[0] == 0x49 && frame[1] == 0x96 && frame[2] == 0x02 && frame[3] == 0xD2);
    CHECK(canfd_xor(frame, 108) == CANFD_INNER_HEAD);
    canfd_put_be32(frame + 108, canfd_xor(frame, 108));
    CHECK(canfd_xor(frame, 108) == canfd_get_be32(frame + 108));
    frame[5] ^= 0x01;
    CHECK(canfd_xor(frame, 108) != canfd_get_be32(frame + 108));
}

static void test_commands()
{
    canfd_state_reset();
    uint8_t init[112];
    fill_init(init, 0, 500000, 2000000, 0x02, 1);
    CommandResponse ok = command(CMD_CAN_CONFIG, init, 112);
    CHECK(ok.index == CMD_CAN_CONFIG);
    CHECK(ok.count == 0x2222);
    CHECK(ok.time == 0x3333);
    CHECK(status_of(ok) == 0);

    uint8_t bad[112];
    std::memcpy(bad, init, 112);
    bad[20] = 0x01;
    canfd_put_be32(bad + 108, canfd_xor(bad, 108));
    CHECK(status_of(command(CMD_CAN_CONFIG, bad, 112)) == 1);

    fill_init(bad, 0, 0, 0, 0x02, 0);
    CHECK(status_of(command(CMD_CAN_CONFIG, bad, 112)) == 1);
    fill_init(bad, 0, 1000001, 0, 0x02, 0);
    CHECK(status_of(command(CMD_CAN_CONFIG, bad, 112)) == 1);
    fill_init(bad, 0, 500000, 100, 0x02, 0);
    CHECK(status_of(command(CMD_CAN_CONFIG, bad, 112)) == 1);
    fill_init(bad, 0, 500000, 8000001, 0x04, 1);
    CHECK(status_of(command(CMD_CAN_CONFIG, bad, 112)) == 1);
    fill_init(bad, 6, 500000, 0, 0x02, 0);
    CHECK(status_of(command(CMD_CAN_CONFIG, bad, 112)) == 1);
    fill_init(bad, 0, 500000, 0, 0x02, 2);
    CHECK(status_of(command(CMD_CAN_CONFIG, bad, 112)) == 1);
    CHECK(status_of(command(CMD_CAN_CONFIG, init, 111)) == 1);
    std::memcpy(bad, init, 112);
    bad[108] ^= 0xFF;
    CHECK(status_of(command(CMD_CAN_CONFIG, bad, 112)) == 1);

    uint8_t query[4];
    canfd_put_be32(query, 0xFFFFFFFFu);
    CommandResponse info = command(CMD_CAN_INFO_QUERY, query, 4);
    CHECK(info.payload.size() == 6u * 112u);
    CHECK(info.payload[4] == 1);
    CHECK(info.payload[5] == 0x00);
    CHECK(info.payload[7] == 0);
    CHECK(canfd_xor(info.payload.data(), 108) == canfd_get_be32(info.payload.data() + 108));
    for (int i = 0; i < 108; ++i) {
        if (i == 4) {
            continue;
        }
        CHECK(info.payload[i] == init[i]);
    }
    for (int ch = 1; ch < 6; ++ch) {
        for (int i = 0; i < 112; ++i) {
            CHECK(info.payload[ch * 112 + i] == 0);
        }
    }
    canfd_put_be32(query, 0);
    CHECK(status_of(command(CMD_CAN_INFO_QUERY, query, 4)) == 1);

    uint8_t run[8];
    canfd_put_be32(run, 0);
    canfd_put_be32(run + 4, 0xAA);
    CHECK(status_of(command(CMD_CAN_START_STOP, run, 8)) == 0);
    canfd_put_be32(run + 4, 0xBB);
    CHECK(status_of(command(CMD_CAN_START_STOP, run, 8)) == 0);
    canfd_put_be32(run + 4, 0x01);
    CHECK(status_of(command(CMD_CAN_START_STOP, run, 8)) == 1);
    canfd_put_be32(run, 6);
    canfd_put_be32(run + 4, 0xAA);
    CHECK(status_of(command(CMD_CAN_START_STOP, run, 8)) == 1);
    CHECK(status_of(command(CMD_CAN_START_STOP, run, 4)) == 1);

    canfd_put_be32(query, 0xFFFFFFFFu);
    CommandResponse buffers = command(CMD_CAN_BUF_QUERY, query, 4);
    CHECK(buffers.payload.size() == 96u);
    const uint32_t space = AXI_FIFO_BUF_SIZE * AXI_FIFO_BUF_UNIT_SIZE;
    for (int ch = 0; ch < 6; ++ch) {
        CHECK(buffers.payload[ch * 16] == ch);
        CHECK(buffers.payload[ch * 16 + 1] == 0);
        CHECK(buffers.payload[ch * 16 + 2] == 0);
        CHECK(buffers.payload[ch * 16 + 3] == 0);
        CHECK(canfd_get_be32(buffers.payload.data() + ch * 16 + 4) == space);
        CHECK(canfd_get_be32(buffers.payload.data() + ch * 16 + 8) == space);
        CHECK(canfd_get_be32(buffers.payload.data() + ch * 16 + 12) == space);
    }
    canfd_put_be32(query, 1);
    CHECK(status_of(command(CMD_CAN_BUF_QUERY, query, 4)) == 1);

    uint8_t version[4];
    canfd_put_be32(version, CANFD_VERSION_ARM);
    CommandResponse arm = command(CMD_VERSION_QUERY, version, 4);
    CHECK(arm.payload.size() == 4u);
    CHECK(canfd_get_be32(arm.payload.data()) == CANFD_APP_VERSION);
    canfd_put_be32(version, CANFD_VERSION_FPGA);
    CommandResponse fpga = command(CMD_VERSION_QUERY, version, 4);
    CHECK(canfd_get_be32(fpga.payload.data()) == CANFD_VERSION_FAIL);
    canfd_put_be32(version, 0x01);
    CHECK(canfd_get_be32(command(CMD_VERSION_QUERY, version, 4).payload.data()) == CANFD_VERSION_FAIL);
    CHECK(canfd_get_be32(command(CMD_VERSION_QUERY, version, 0).payload.data()) == CANFD_VERSION_FAIL);

    CommandResponse unknown = command(0x9999, query, 4);
    CHECK(unknown.index == 0x9999);
    CHECK(status_of(unknown) == 1);
}

static void test_downlink()
{
    uint8_t frames[224];
    fill_data(frames, 1, 0x01);
    fill_data(frames + 112, 2, 0x01);
    NetHeader header;
    std::memset(&header, 0, sizeof(header));
    header.cmd_code = NET_DATA_CODE;
    header.length = 224;
    header.index = DATA_INDEX_NORMAL;
    CHECK(canfd_handle_downlink(header, frames, 224) == 0);

    fill_data(frames + 112, 2, 0x02);
    CHECK(canfd_handle_downlink(header, frames, 224) == -1);
    header.length = 100;
    CHECK(canfd_handle_downlink(header, frames, 100) == -1);
    header.length = 0;
    CHECK(canfd_handle_downlink(header, frames, 0) == -1);

    fill_data(frames, 3, 0x02);
    header.length = 112;
    header.index = DATA_INDEX_PULSE;
    CHECK(canfd_handle_downlink(header, frames, 112) == 0);
    fill_data(frames, 3, 0x01);
    CHECK(canfd_handle_downlink(header, frames, 112) == -1);

    uint8_t sim[104];
    std::memset(sim, 0, sizeof(sim));
    canfd_put_be32(sim, CANFD_INNER_HEAD);
    sim[4] = 0;
    sim[5] = 0x03;
    canfd_put_be16(sim + 6, 104);
    sim[8] = 4;
    canfd_put_be16(sim + 29, 72);
    header.index = DATA_INDEX_SIM;
    header.length = 104;
    CHECK(canfd_handle_downlink(header, sim, 104) == 0);
    canfd_put_be16(sim + 6, 100);
    CHECK(canfd_handle_downlink(header, sim, 104) == -1);
    canfd_put_be16(sim + 6, 104);
    canfd_put_be16(sim + 29, 70);
    CHECK(canfd_handle_downlink(header, sim, 104) == -1);
    canfd_put_be16(sim + 29, 72);
    sim[8] = 6;
    CHECK(canfd_handle_downlink(header, sim, 104) == -1);

    header.cmd_code = NET_CMD_CODE;
    header.index = DATA_INDEX_NORMAL;
    header.length = 112;
    fill_data(frames, 0, 0x01);
    CHECK(canfd_handle_downlink(header, frames, 112) == -1);
}

static void test_codec()
{
    /* IDR: standard identifier in bits 31:21, IDE=0. */
    CHECK(canfd_idr_encode(0x123, 0, 0) == (0x123u << 21));
    CHECK(canfd_idr_encode(0x123, 0, 1) == ((0x123u << 21) | (1u << 20)));
    uint32_t id = 0;
    int eff = -1;
    int rtr = -1;
    canfd_idr_decode(canfd_idr_encode(0x123, 0, 1), &id, &eff, &rtr);
    CHECK(id == 0x123 && eff == 0 && rtr == 1);

    /* IDR: extended identifier spreads ID[28:18] and ID[17:0] around IDE. */
    const uint32_t ext = 0x1ABCDEFu;
    canfd_idr_decode(canfd_idr_encode(ext, 1, 0), &id, &eff, &rtr);
    CHECK(id == ext && eff == 1 && rtr == 0);
    const uint32_t ext_idr = canfd_idr_encode(ext, 1, 0);
    CHECK(((ext_idr >> 21) & 0x7FFu) == ((ext >> 18) & 0x7FFu));
    CHECK(((ext_idr >> 1) & 0x3FFFFu) == (ext & 0x3FFFFu));
    CHECK((ext_idr & (1u << 19)) != 0 && (ext_idr & (1u << 20)) != 0);

    /* Table 3-2 length <-> DLC mapping (smallest fitting level). */
    CHECK(canfd_len_to_dlc(0) == 0);
    CHECK(canfd_len_to_dlc(8) == 8);
    CHECK(canfd_len_to_dlc(9) == 9);
    CHECK(canfd_len_to_dlc(12) == 9);
    CHECK(canfd_len_to_dlc(21) == 12);
    CHECK(canfd_len_to_dlc(64) == 15);
    CHECK(canfd_dlc_to_len(8) == 8);
    CHECK(canfd_dlc_to_len(9) == 12);
    CHECK(canfd_dlc_to_len(15) == 64);

    /* DLCR bit layout: DLC[31:28], EDL[27], BRS[26]. */
    canfd_message_t msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.fd = 1;
    msg.brs = 1;
    msg.len = 64;
    const uint32_t dlcr = canfd_dlcr_encode(&msg);
    CHECK(dlcr == 0xFC000000u);
    canfd_message_t back;
    std::memset(&back, 0, sizeof(back));
    canfd_dlcr_decode(dlcr, &back);
    CHECK(back.fd == 1 && back.brs == 1 && back.len == 64);

    std::memset(&msg, 0, sizeof(msg));
    msg.len = 8;
    const uint32_t classic = canfd_dlcr_encode(&msg);
    CHECK(classic == 0x80000000u);
    canfd_dlcr_decode(classic, &back);
    CHECK(back.fd == 0 && back.len == 8);

    /* 112-byte packet message area round trip, Byte17..Byte88. */
    uint8_t packet[112];
    std::memset(packet, 0, sizeof(packet));
    canfd_put_be32(packet, CANFD_INNER_HEAD);
    canfd_message_t out;
    std::memset(&out, 0, sizeof(out));
    out.can_id = 0x1ABCDEFu;
    out.eff = 1;
    out.fd = 1;
    out.brs = 1;
    out.len = 64;
    for (int i = 0; i < 64; ++i) {
        out.data[i] = static_cast<uint8_t>(i + 1);
    }
    canfd_packet_encode_message(packet, &out);
    CHECK(canfd_get_be32(packet + 16) == canfd_idr_encode(0x1ABCDEFu, 1, 0));
    CHECK(canfd_get_be32(packet + 20) == 0xFC000000u);
    canfd_message_t in;
    CHECK(canfd_packet_decode_message(packet, &in) == 0);
    CHECK(in.can_id == out.can_id && in.eff == 1 && in.fd == 1 && in.brs == 1);
    CHECK(in.len == 64);
    CHECK(std::memcmp(in.data, out.data, 64) == 0);

    /* Classic remote frame keeps the RTR bit. */
    std::memset(&out, 0, sizeof(out));
    out.can_id = 0x321;
    out.rtr = 1;
    out.len = 0;
    canfd_packet_encode_message(packet, &out);
    CHECK(canfd_packet_decode_message(packet, &in) == 0);
    CHECK(in.can_id == 0x321 && in.eff == 0 && in.rtr == 1 && in.fd == 0);
}

int main()
{
    canfd_hw_start();
    test_header_roundtrip();
    test_xor();
    test_codec();
    test_commands();
    test_downlink();
    if (g_failed != 0) {
        std::printf("%d checks failed\n", g_failed);
        return 1;
    }
    std::printf("protocol tests passed\n");
    return 0;
}
