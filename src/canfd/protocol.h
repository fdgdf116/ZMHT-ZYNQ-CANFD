#ifndef CANFD_PROTOCOL_H_
#define CANFD_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>
#include <vector>

enum {
    NET_CMD_CODE = 0x03CCF0FF,
    NET_DATA_CODE = 0x04CCF0FF,
    NET_RES_CODE = 0x05CCF0FF,
    NET_UP_CODE = 0x01CCF0FF,
    CANFD_INNER_HEAD = 0x499602D2,
    CANFD_FRAME_SIZE = 112,
    CANFD_CHANNEL_COUNT = 6,
    CANFD_CMD_MAX_PAYLOAD = 4096,
    CANFD_DATA_MAX_PAYLOAD = 8192,
    CANFD_APP_VERSION = 0x20261013,
    CANFD_VERSION_ARM = 0xAA,
    CANFD_VERSION_FPGA = 0xBB,
    CANFD_VERSION_FAIL = 0xFFFFFFFF,
    CMD_CAN_BUF_QUERY = 0x0100,
    CMD_CAN_CONFIG = 0x0101,
    CMD_CAN_INFO_QUERY = 0x0102,
    CMD_CAN_START_STOP = 0x0103,
    CMD_VERSION_QUERY = 0xFF05,
    DATA_INDEX_NORMAL = 0x0001,
    DATA_INDEX_PULSE = 0x0002,
    DATA_INDEX_SIM = 0x0003,
    UPLINK_INDEX_STATUS = 0x0080,
    UPLINK_INDEX_RX = 0x0081,
    UPLINK_INDEX_SIM = 0x0082
};

struct NetHeader {
    uint32_t cmd_code;
    uint32_t length;
    uint16_t count;
    uint16_t index;
    uint16_t time;
    uint16_t crc;
};

struct CommandResponse {
    uint16_t index;
    uint16_t count;
    uint16_t time;
    std::vector<uint8_t> payload;
};

void net_header_encode(uint8_t out[16], const NetHeader& header);
void net_header_decode(const uint8_t in[16], NetHeader* header);
bool net_header_accept(const NetHeader& header, uint32_t expect_code, uint32_t max_payload);

void canfd_put_be16(uint8_t* dest, uint16_t value);
void canfd_put_be32(uint8_t* dest, uint32_t value);
uint16_t canfd_get_be16(const uint8_t* src);
uint32_t canfd_get_be32(const uint8_t* src);
uint32_t canfd_xor(const uint8_t* data, size_t size_without_checksum);

CommandResponse canfd_cmd_status(const NetHeader& header, uint32_t status);
CommandResponse canfd_cmd_buf_query(const NetHeader& header, const uint8_t* payload, uint32_t length);
CommandResponse canfd_cmd_config(const NetHeader& header, const uint8_t* payload, uint32_t length);
CommandResponse canfd_cmd_info_query(const NetHeader& header, const uint8_t* payload, uint32_t length);
CommandResponse canfd_cmd_start_stop(const NetHeader& header, const uint8_t* payload, uint32_t length);
CommandResponse canfd_cmd_version(const NetHeader& header, const uint8_t* payload, uint32_t length);

int canfd_data_normal(const uint8_t* payload, uint32_t length);
int canfd_data_pulse(const uint8_t* payload, uint32_t length);
int canfd_data_simulator(const uint8_t* payload, uint32_t length);
void canfd_send_tx_feedback(const uint8_t* frame, bool ok);

CommandResponse canfd_handle_command(const NetHeader& header, const uint8_t* payload, uint32_t length);
int canfd_handle_downlink(const NetHeader& header, const uint8_t* payload, uint32_t length);
void canfd_state_reset();

#endif /* CANFD_PROTOCOL_H_ */
