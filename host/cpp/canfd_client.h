#ifndef CANFD_CLIENT_H_
#define CANFD_CLIENT_H_

/*
 * ZYNQ CANFD 控制盒上位机客户端封装
 *
 * 把 TCP 三端口、16 字节外层头、112 字节内层包、XOR 校验、IDR/DLCR
 * 编解码和异步收包全部封装在 Client 内。调用方只需要：
 *
 *   canfd::Client c;
 *   c.Open("192.168.8.24");
 *   c.Configure(0, 500000, 2000000);
 *   c.Start(0);
 *   canfd::Frame tx;
 *   tx.channel = 0; tx.can_id = 0x123; tx.data = {1,2,3,4}; tx.fd = true;
 *   canfd::Frame feedback;
 *   c.Send(tx, &feedback);
 *   canfd::Frame rx;
 *   if (c.Receive(&rx, 1000)) { ... }
 *
 * 适用平台：Windows (Winsock2) / Linux (POSIX socket)。
 * 线程安全：Send/Receive/Configure 可以从不同线程调用；回调在接收线程执行。
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace canfd {

constexpr uint16_t kCmdPort = 9009;   /* 上位机 -> 下位机 命令 */
constexpr uint16_t kUpPort = 9010;    /* 下位机 -> 上位机 应答/上行 */
constexpr uint16_t kDataPort = 9012;  /* 上位机 -> 下位机 数据 */

enum : uint8_t {
    kTypeInit = 0x00,
    kTypeNormal = 0x01,
    kTypePulse = 0x02,
    kTypeSim = 0x03,
    kTypeStatus = 0x80,
    kTypeRx = 0x81,
    kTypeSimUp = 0x82,
};

enum : uint8_t {
    kModeNormal = 0x02,
    kModeLoopback = 0x04,
};

enum : uint32_t {
    kRunOpen = 0xAA,
    kRunClose = 0xBB,
};

enum : uint16_t {
    kCmdBufferQuery = 0x0100,
    kCmdConfig = 0x0101,
    kCmdInfoQuery = 0x0102,
    kCmdStartStop = 0x0103,
    kCmdVersionQuery = 0xFF05,
    kDataIndexNormal = 0x0001,
    kDataIndexPulse = 0x0002,
    kDataIndexSim = 0x0003,
    kUplinkStatus = 0x0080,
    kUplinkRx = 0x0081,
    kUplinkSim = 0x0082,
};

struct Frame {
    uint8_t channel = 0;
    uint32_t can_id = 0;
    std::vector<uint8_t> data;
    bool fd = true;
    bool brs = false;
    bool extended = false;
    bool remote = false;
    bool esi = false;
    uint8_t dlc = 0;
    uint8_t states = 0;      /* 0 失败 / 1 成功（0x80/0x81） */
    uint8_t xor_states = 0;  /* 0x80 校验状态 */
    uint64_t timestamp_us = 0;
    uint32_t total_count = 0;
    uint32_t ch_count = 0;
    uint8_t msg_type = kTypeRx;

    bool TxOk() const { return states != 0; }
};

struct ChannelConfig {
    uint8_t channel = 0;
    bool configured = false;
    uint32_t bitrate = 0;
    uint32_t data_bitrate = 0;
    uint8_t mode = 0;
    uint8_t auto_send = 0;
    uint16_t ier = 0;
    uint32_t packet_interval_us = 0;
    uint8_t brs_en = 0;
    uint64_t timestamp_us = 0;
};

struct BufferStatus {
    uint8_t channel = 0;
    uint32_t normal_space = 0;
    uint32_t pulse_space = 0;
    uint32_t sim_space = 0;
};

class Client {
public:
    Client();
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /*
     * 先连 9010，再连 9009/9012。下位机在 9010 没有客户端时会丢弃所有
     * 应答和上行，因此连接顺序不能反。
     */
    bool Open(const std::string& host,
              std::string* error = nullptr,
              uint16_t cmd_port = kCmdPort,
              uint16_t up_port = kUpPort,
              uint16_t data_port = kDataPort,
              int timeout_ms = 3000);
    void Close();
    bool IsOpen() const;

    /* 0x0101：配置仲裁/数据段波特率、模式、自动重传、BRS 和包间隔。 */
    bool Configure(uint8_t channel,
                   uint32_t bitrate,
                   uint32_t data_bitrate = 0,
                   uint8_t mode = kModeNormal,
                   bool auto_send = true,
                   bool brs = false,
                   uint16_t ier = 0,
                   uint32_t packet_interval_us = 0,
                   int timeout_ms = 3000,
                   std::string* error = nullptr);

    /* 0x0103：打开/关闭一个通道。 */
    bool Start(uint8_t channel, int timeout_ms = 3000, std::string* error = nullptr);
    bool Stop(uint8_t channel, int timeout_ms = 3000, std::string* error = nullptr);

    /* 0x0102 / 0x0100 / 0xFF05。 */
    bool QueryInfo(std::vector<ChannelConfig>* out,
                   int timeout_ms = 3000, std::string* error = nullptr);
    bool QueryBuffers(std::vector<BufferStatus>* out,
                      int timeout_ms = 3000, std::string* error = nullptr);
    bool Version(bool fpga, uint32_t* version,
                 int timeout_ms = 3000, std::string* error = nullptr);

    /*
     * 9012 发送一帧，并等待 9010 上对应的 0x80 反馈。
     * feedback->states == 1 表示下位机发送成功。
     */
    bool Send(const Frame& frame,
              Frame* feedback = nullptr,
              int timeout_ms = 3000,
              std::string* error = nullptr);

    bool Send(uint8_t channel,
              uint32_t can_id,
              const std::vector<uint8_t>& data,
              bool fd = true,
              bool brs = false,
              bool extended = false,
              bool remote = false,
              bool pulse = false,
              Frame* feedback = nullptr,
              int timeout_ms = 3000,
              std::string* error = nullptr);

    /* 等待 9010 上的一帧 0x81 采集上行；超时返回 false。 */
    bool Receive(Frame* frame, int timeout_ms = 1000);

    /* 注册接收回调；回调运行在内部接收线程，不要在里面做长时间阻塞。 */
    void SetFrameCallback(std::function<void(const Frame&)> callback);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace canfd

#endif  // CANFD_CLIENT_H_
