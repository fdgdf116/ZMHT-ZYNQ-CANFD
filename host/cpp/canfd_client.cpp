#include "canfd_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static const socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static const socket_t kInvalidSocket = -1;
#endif

namespace canfd {
namespace {

constexpr uint32_t kHead = 0x499602D2u;
constexpr uint32_t kCmdCode = 0x03CCF0FFu;
constexpr uint32_t kDataCode = 0x04CCF0FFu;
constexpr uint32_t kResCode = 0x05CCF0FFu;
constexpr uint32_t kUpCode = 0x01CCF0FFu;
constexpr size_t kFrameSize = 112;
constexpr size_t kNetHeadSize = 16;
constexpr size_t kDataChannelSize = 4;
constexpr int kReadyDelayMs = 1200;

const uint8_t kFdLen[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};

// ---------------------------------------------------------------------------
// 字节序 / 校验
// ---------------------------------------------------------------------------

void PutBe16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void PutBe32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

uint16_t GetBe16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t GetBe32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t GetBe64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

uint32_t Xor32(const uint8_t* data, size_t size)
{
    uint32_t value = 0;
    for (size_t i = 0; i + 4 <= size; i += 4) {
        value ^= GetBe32(data + i);
    }
    return value;
}

// ---------------------------------------------------------------------------
// IDR / DLCR
// ---------------------------------------------------------------------------

uint32_t EncodeIdr(uint32_t can_id, bool extended, bool remote)
{
    if (extended) {
        return (((can_id >> 18) & 0x7FFu) << 21) |
               (1u << 20) | (1u << 19) |
               ((can_id & 0x3FFFFu) << 1) |
               (remote ? 1u : 0u);
    }
    return ((can_id & 0x7FFu) << 21) | (remote ? (1u << 20) : 0u);
}

void DecodeIdr(uint32_t idr, Frame* frame)
{
    frame->extended = ((idr >> 19) & 1u) != 0;
    if (frame->extended) {
        frame->can_id = (((idr >> 21) & 0x7FFu) << 18) | ((idr >> 1) & 0x3FFFFu);
        frame->remote = (idr & 1u) != 0;
    } else {
        frame->can_id = (idr >> 21) & 0x7FFu;
        frame->remote = ((idr >> 20) & 1u) != 0;
    }
}

int DlcToLen(unsigned dlc)
{
    return kFdLen[dlc & 0x0Fu];
}

unsigned LenToDlc(size_t len)
{
    if (len == 0) {
        return 0;
    }
    for (unsigned i = 1; i < 16; ++i) {
        if (len <= static_cast<size_t>(kFdLen[i])) {
            return i;
        }
    }
    return 15;
}

uint32_t EncodeDlcr(bool fd, bool brs, unsigned dlc, bool esi = false,
                    uint16_t timestamp = 0)
{
    uint32_t value = (dlc & 0x0Fu) << 28;
    if (fd) value |= 1u << 27;
    if (brs) value |= 1u << 26;
    if (esi) value |= 1u << 25;
    value |= timestamp;
    return value;
}

void DecodeDlcr(uint32_t dlcr, Frame* frame)
{
    frame->dlc = static_cast<uint8_t>((dlcr >> 28) & 0x0Fu);
    frame->fd = ((dlcr >> 27) & 1u) != 0;
    frame->brs = ((dlcr >> 26) & 1u) != 0;
    frame->esi = ((dlcr >> 25) & 1u) != 0;
    const int len = frame->fd ? DlcToLen(frame->dlc) : std::min<int>(frame->dlc, 8);
    frame->data.resize(static_cast<size_t>(len));
}

// ---------------------------------------------------------------------------
// 内层 112 字节包
// ---------------------------------------------------------------------------

void FinishInner(std::vector<uint8_t>* packet)
{
    PutBe32(packet->data() + 108, Xor32(packet->data(), 108));
}

std::vector<uint8_t> BuildConfig(uint8_t channel, uint32_t bitrate,
                                 uint32_t data_bitrate, uint8_t mode,
                                 bool auto_send, bool brs, uint16_t ier,
                                 uint32_t interval_us)
{
    std::vector<uint8_t> p(kFrameSize, 0);
    PutBe32(p.data(), kHead);
    p[4] = 0;
    p[5] = kTypeInit;
    p[6] = kFrameSize;
    p[7] = channel;
    PutBe32(p.data() + 16, bitrate);
    p[20] = mode;
    p[21] = auto_send ? 1 : 0;
    PutBe16(p.data() + 22, ier & 0x0FFFu);
    PutBe32(p.data() + 24, interval_us);
    PutBe32(p.data() + 28, data_bitrate);
    p[32] = brs ? 1 : 0;
    FinishInner(&p);
    return p;
}

std::vector<uint8_t> BuildData(const Frame& frame, uint32_t total_count,
                               uint32_t ch_count, uint8_t msg_type)
{
    std::vector<uint8_t> p(kFrameSize, 0);
    PutBe32(p.data(), kHead);
    p[4] = 0;
    p[5] = msg_type;
    p[6] = kFrameSize;
    p[7] = frame.channel;
    PutBe32(p.data() + 8, total_count);
    PutBe32(p.data() + 12, ch_count);
    PutBe32(p.data() + 16, EncodeIdr(frame.can_id, frame.extended, frame.remote));
    unsigned dlc;
    if (frame.fd) {
        dlc = LenToDlc(frame.data.size());
    } else {
        dlc = static_cast<unsigned>(std::min<size_t>(frame.data.size(), 8));
    }
    PutBe32(p.data() + 20, EncodeDlcr(frame.fd, frame.brs, dlc));
    if (!frame.remote) {
        std::copy(frame.data.begin(), frame.data.end(), p.begin() + 24);
    }
    PutBe16(p.data() + 96, 1);   // Packets_Sum
    PutBe16(p.data() + 98, 0);   // Packets_No
    FinishInner(&p);
    return p;
}

bool ParseInner(const std::vector<uint8_t>& p, Frame* frame)
{
    if (p.size() != kFrameSize || GetBe32(p.data()) != kHead) {
        return false;
    }
    frame->msg_type = p[5];
    frame->channel = p[7];
    frame->total_count = GetBe32(p.data() + 8);
    frame->ch_count = GetBe32(p.data() + 12);
    DecodeIdr(GetBe32(p.data() + 16), frame);
    DecodeDlcr(GetBe32(p.data() + 20), frame);
    if (frame->fd) {
        frame->remote = false;
    }
    if (frame->data.size() > 64) {
        frame->data.resize(64);
    }
    if (!frame->remote && !frame->data.empty()) {
        std::copy(p.begin() + 24, p.begin() + 24 + frame->data.size(),
                  frame->data.begin());
    } else {
        frame->data.clear();
    }
    frame->states = (frame->msg_type == kTypeStatus || frame->msg_type == kTypeRx)
                        ? p[96] : 0;
    frame->xor_states = (frame->msg_type == kTypeStatus) ? p[97] : 0;
    frame->timestamp_us = GetBe64(p.data() + 100);
    return true;
}

bool ParseConfigRow(const uint8_t* p, uint8_t channel, ChannelConfig* cfg)
{
    cfg->channel = channel;
    cfg->configured = false;
    if (GetBe32(p) != kHead) {
        return true;   // 未配置通道为全 0
    }
    cfg->configured = true;
    cfg->channel = p[4] == 1 ? p[7] : channel;
    cfg->bitrate = GetBe32(p + 16);
    cfg->mode = p[20];
    cfg->auto_send = p[21];
    cfg->ier = GetBe16(p + 22);
    cfg->packet_interval_us = GetBe32(p + 24);
    cfg->data_bitrate = GetBe32(p + 28);
    cfg->brs_en = p[32];
    cfg->timestamp_us = GetBe64(p + 100);
    return true;
}

// ---------------------------------------------------------------------------
// socket 辅助
// ---------------------------------------------------------------------------

bool InitSockets()
{
#ifdef _WIN32
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

bool ValidSocket(socket_t s)
{
    return s != kInvalidSocket;
}

void CloseSocket(socket_t s)
{
    if (!ValidSocket(s)) {
        return;
    }
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

void ShutdownSocket(socket_t s)
{
    if (!ValidSocket(s)) {
        return;
    }
#ifdef _WIN32
    ::shutdown(s, SD_BOTH);
#else
    ::shutdown(s, SHUT_RDWR);
#endif
}

int LastSocketError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool WouldBlock(int err)
{
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EINPROGRESS || err == EWOULDBLOCK;
#endif
}

bool SetNonBlocking(socket_t s, bool enable)
{
#ifdef _WIN32
    u_long mode = enable ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(s, F_SETFL, enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

socket_t ConnectTcp(const std::string& host, uint16_t port, int timeout_ms,
                    std::string* error)
{
    if (!InitSockets()) {
        if (error) *error = "WSAStartup failed";
        return kInvalidSocket;
    }
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char service[16];
    std::snprintf(service, sizeof(service), "%u", static_cast<unsigned>(port));
    struct addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), service, &hints, &result) != 0) {
        if (error) *error = "getaddrinfo(" + host + ") failed";
        return kInvalidSocket;
    }
    for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        socket_t s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (!ValidSocket(s)) {
            continue;
        }
        SetNonBlocking(s, true);
        int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool connected = (rc == 0);
        if (!connected && WouldBlock(LastSocketError())) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(s, &wfds);
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
            rc = select(0, nullptr, &wfds, nullptr, &tv);
#else
            rc = select(s + 1, nullptr, &wfds, nullptr, &tv);
#endif
            if (rc > 0) {
                int so_error = 0;
#ifdef _WIN32
                int len = sizeof(so_error);
#else
                socklen_t len = sizeof(so_error);
#endif
                getsockopt(s, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&so_error), &len);
                connected = (so_error == 0);
            }
        }
        if (connected) {
            SetNonBlocking(s, false);
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&one), sizeof(one));
            freeaddrinfo(result);
            return s;
        }
        CloseSocket(s);
    }
    freeaddrinfo(result);
    if (error) {
        *error = "connect " + host + ":" + std::to_string(port) + " failed";
    }
    return kInvalidSocket;
}

bool SendAll(socket_t s, const uint8_t* data, size_t size)
{
    size_t sent = 0;
    while (sent < size) {
#ifdef _WIN32
        int n = ::send(s, reinterpret_cast<const char*>(data + sent),
                       static_cast<int>(size - sent), 0);
#else
        ssize_t n = ::send(s, data + sent, size - sent, 0);
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool RecvExact(socket_t s, uint8_t* data, size_t size)
{
    size_t received = 0;
    while (received < size) {
#ifdef _WIN32
        int n = ::recv(s, reinterpret_cast<char*>(data + received),
                       static_cast<int>(size - received), 0);
#else
        ssize_t n = ::recv(s, data + received, size - received, 0);
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n <= 0) {
            return false;
        }
        received += static_cast<size_t>(n);
    }
    return true;
}

std::vector<uint8_t> PackNetHeader(uint32_t code, uint32_t length,
                                   uint16_t count, uint16_t index)
{
    std::vector<uint8_t> h(kNetHeadSize);
    PutBe32(h.data(), code);
    PutBe32(h.data() + 4, length);
    PutBe16(h.data() + 8, count);
    PutBe16(h.data() + 10, index);
    PutBe16(h.data() + 12, 0);
    PutBe16(h.data() + 14, 0);
    return h;
}

bool SendPacket(socket_t s, uint32_t code, uint16_t count, uint16_t index,
                const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> header = PackNetHeader(
        code, static_cast<uint32_t>(payload.size()), count, index);
    if (!SendAll(s, header.data(), header.size())) {
        return false;
    }
    return payload.empty() || SendAll(s, payload.data(), payload.size());
}

bool SendDataPacket(socket_t s, uint16_t index, uint8_t channel,
                    const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> header = PackNetHeader(
        kDataCode, static_cast<uint32_t>(payload.size()), 0, index);
    PutBe32(header.data() + 12, Xor32(header.data(), 12));
    uint8_t channel_bytes[kDataChannelSize];
    PutBe32(channel_bytes, channel);
    return SendAll(s, header.data(), header.size()) &&
           SendAll(s, channel_bytes, sizeof(channel_bytes)) &&
           (payload.empty() || SendAll(s, payload.data(), payload.size()));
}

uint32_t Key16(uint16_t index, uint16_t count)
{
    return (static_cast<uint32_t>(index) << 16) | count;
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Client::Impl {
    struct CmdWait {
        bool done = false;
        std::vector<uint8_t> payload;
    };
    struct TxWait {
        bool done = false;
        Frame frame;
    };

    socket_t cmd = kInvalidSocket;
    socket_t up = kInvalidSocket;
    socket_t data = kInvalidSocket;
    std::thread reader;
    std::atomic<bool> stop{false};
    bool opened = false;

    std::mutex cmd_send_mu;
    std::mutex counter_mu;
    uint16_t cmd_count = 0;
    uint32_t total_tx = 0;
    uint32_t ch_tx[6] = {0, 0, 0, 0, 0, 0};

    std::mutex wait_mu;
    std::condition_variable cmd_cv;
    std::condition_variable tx_cv;
    std::map<uint32_t, CmdWait> cmd_wait;
    std::map<std::pair<uint8_t, uint32_t>, TxWait> tx_wait;

    std::mutex rx_mu;
    std::condition_variable rx_cv;
    std::deque<Frame> rxq;

    std::mutex cb_mu;
    std::function<void(const Frame&)> callback;

    void ReaderLoop();
    bool Request(uint16_t index, const std::vector<uint8_t>& payload,
                 std::vector<uint8_t>* out, int timeout_ms, std::string* error);
    void Reset();
};

void Client::Impl::ReaderLoop()
{
    while (!stop.load()) {
        uint8_t header[kNetHeadSize];
        if (!RecvExact(up, header, sizeof(header))) {
            break;
        }
        const uint32_t code = GetBe32(header);
        const uint32_t length = GetBe32(header + 4);
        const uint16_t count = GetBe16(header + 8);
        const uint16_t index = GetBe16(header + 10);
        if (length > 16 * 1024 * 1024) {
            break;
        }
        uint32_t wire_channel = 0xFFFFFFFFu;
        if (code == kUpCode && (index == kUplinkStatus || index == kUplinkRx)) {
            uint8_t channel_bytes[kDataChannelSize];
            if (!RecvExact(up, channel_bytes, sizeof(channel_bytes))) {
                break;
            }
            wire_channel = GetBe32(channel_bytes);
        }
        std::vector<uint8_t> payload(length);
        if (length != 0 && !RecvExact(up, payload.data(), payload.size())) {
            break;
        }
        if (code == kResCode) {
            std::lock_guard<std::mutex> lock(wait_mu);
            auto it = cmd_wait.find(Key16(index, count));
            if (it != cmd_wait.end()) {
                it->second.done = true;
                it->second.payload = std::move(payload);
                cmd_cv.notify_all();
            }
            continue;
        }
        if (code != kUpCode || payload.size() != kFrameSize) {
            continue;
        }
        Frame frame;
        if (!ParseInner(payload, &frame)) {
            continue;
        }
        if (wire_channel != 0xFFFFFFFFu && frame.channel != wire_channel) {
            continue;
        }
        if (index == kUplinkStatus) {
            std::lock_guard<std::mutex> lock(wait_mu);
            auto it = tx_wait.find(std::make_pair(frame.channel, frame.ch_count));
            if (it != tx_wait.end()) {
                it->second.done = true;
                it->second.frame = frame;
                tx_cv.notify_all();
            }
            continue;
        }
        if (index == kUplinkRx || index == kUplinkSim) {
            {
                std::lock_guard<std::mutex> lock(rx_mu);
                rxq.push_back(frame);
            }
            rx_cv.notify_all();
            std::function<void(const Frame&)> cb;
            {
                std::lock_guard<std::mutex> lock(cb_mu);
                cb = callback;
            }
            if (cb) {
                cb(frame);
            }
        }
    }
    stop.store(true);
    cmd_cv.notify_all();
    tx_cv.notify_all();
    rx_cv.notify_all();
}

bool Client::Impl::Request(uint16_t index, const std::vector<uint8_t>& payload,
                           std::vector<uint8_t>* out, int timeout_ms,
                           std::string* error)
{
    std::lock_guard<std::mutex> send_lock(cmd_send_mu);
    uint16_t count;
    {
        std::lock_guard<std::mutex> lock(counter_mu);
        count = ++cmd_count;
    }
    const uint32_t key = Key16(index, count);
    {
        std::lock_guard<std::mutex> lock(wait_mu);
        cmd_wait[key] = CmdWait();
    }
    if (!SendPacket(cmd, kCmdCode, count, index, payload)) {
        std::lock_guard<std::mutex> lock(wait_mu);
        cmd_wait.erase(key);
        if (error) *error = "send command failed";
        return false;
    }
    std::unique_lock<std::mutex> lock(wait_mu);
    const bool ready = cmd_cv.wait_for(
        lock, std::chrono::milliseconds(std::max(0, timeout_ms)),
        [&] { return cmd_wait[key].done || stop.load(); });
    if (!ready || stop.load()) {
        cmd_wait.erase(key);
        if (error) *error = "command 0x" + [&] {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%04X", index);
            return std::string(buf);
        }() + " timeout";
        return false;
    }
    if (out) {
        *out = std::move(cmd_wait[key].payload);
    }
    cmd_wait.erase(key);
    return true;
}

void Client::Impl::Reset()
{
    stop.store(true);
    ShutdownSocket(cmd);
    ShutdownSocket(up);
    ShutdownSocket(data);
    if (reader.joinable()) {
        reader.join();
    }
    CloseSocket(cmd);
    CloseSocket(up);
    CloseSocket(data);
    cmd = up = data = kInvalidSocket;
    {
        std::lock_guard<std::mutex> lock(rx_mu);
        rxq.clear();
    }
    {
        std::lock_guard<std::mutex> lock(wait_mu);
        cmd_wait.clear();
        tx_wait.clear();
    }
    opened = false;
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

Client::Client() : impl_(new Impl()) {}

Client::~Client()
{
    Close();
    delete impl_;
}

bool Client::Open(const std::string& host, std::string* error,
                  uint16_t cmd_port, uint16_t up_port, uint16_t data_port,
                  int timeout_ms)
{
    if (impl_->opened) {
        return true;
    }
    std::string err;
    impl_->stop.store(false);
    impl_->up = ConnectTcp(host, up_port, timeout_ms, &err);
    if (!ValidSocket(impl_->up)) {
        if (error) *error = err;
        return false;
    }
    /* 下位机 9010 的 accept 线程最多 1 秒轮询一次，等它把客户端挂上，
     * 否则第一条命令的应答会被丢弃。 */
    std::this_thread::sleep_for(std::chrono::milliseconds(kReadyDelayMs));
    impl_->cmd = ConnectTcp(host, cmd_port, timeout_ms, &err);
    if (!ValidSocket(impl_->cmd)) {
        if (error) *error = err;
        Close();
        return false;
    }
    impl_->data = ConnectTcp(host, data_port, timeout_ms, &err);
    if (!ValidSocket(impl_->data)) {
        if (error) *error = err;
        Close();
        return false;
    }
    impl_->opened = true;
    impl_->reader = std::thread(&Impl::ReaderLoop, impl_);
    return true;
}

void Client::Close()
{
    if (!impl_ || (!impl_->opened && !ValidSocket(impl_->up))) {
        return;
    }
    impl_->Reset();
}

bool Client::IsOpen() const
{
    return impl_ && impl_->opened && !impl_->stop.load();
}

bool Client::Configure(uint8_t channel, uint32_t bitrate, uint32_t data_bitrate,
                       uint8_t mode, bool auto_send, bool brs, uint16_t ier,
                       uint32_t packet_interval_us, int timeout_ms,
                       std::string* error)
{
    if (channel >= 6) {
        if (error) *error = "channel must be 0..5";
        return false;
    }
    if (bitrate == 0 || bitrate > 1000000 ||
        (data_bitrate != 0 &&
         (data_bitrate < bitrate || data_bitrate > 8000000))) {
        if (error) *error = "invalid bitrate";
        return false;
    }
    std::vector<uint8_t> reply;
    if (!impl_->Request(kCmdConfig,
                        BuildConfig(channel, bitrate, data_bitrate, mode,
                                    auto_send, brs, ier, packet_interval_us),
                        &reply, timeout_ms, error)) {
        return false;
    }
    if (reply.size() < 4 || GetBe32(reply.data()) != 0) {
        if (error) *error = "device rejected configuration";
        return false;
    }
    return true;
}

bool Client::Start(uint8_t channel, int timeout_ms, std::string* error)
{
    if (channel >= 6) {
        if (error) *error = "channel must be 0..5";
        return false;
    }
    std::vector<uint8_t> payload(8);
    PutBe32(payload.data(), channel);
    PutBe32(payload.data() + 4, kRunOpen);
    std::vector<uint8_t> reply;
    if (!impl_->Request(kCmdStartStop, payload, &reply, timeout_ms, error)) {
        return false;
    }
    if (reply.size() < 4 || GetBe32(reply.data()) != 0) {
        if (error) *error = "device rejected start";
        return false;
    }
    return true;
}

bool Client::Stop(uint8_t channel, int timeout_ms, std::string* error)
{
    if (channel >= 6) {
        if (error) *error = "channel must be 0..5";
        return false;
    }
    std::vector<uint8_t> payload(8);
    PutBe32(payload.data(), channel);
    PutBe32(payload.data() + 4, kRunClose);
    std::vector<uint8_t> reply;
    if (!impl_->Request(kCmdStartStop, payload, &reply, timeout_ms, error)) {
        return false;
    }
    if (reply.size() < 4 || GetBe32(reply.data()) != 0) {
        if (error) *error = "device rejected stop";
        return false;
    }
    return true;
}

bool Client::QueryInfo(std::vector<ChannelConfig>* out, int timeout_ms,
                       std::string* error)
{
    std::vector<uint8_t> payload = {0xFF, 0xFF, 0xFF, 0xFF};
    std::vector<uint8_t> reply;
    if (!impl_->Request(kCmdInfoQuery, payload, &reply, timeout_ms, error)) {
        return false;
    }
    if (reply.size() == 4) {
        if (error) *error = "device rejected info query";
        return false;
    }
    if (reply.size() != 6 * kFrameSize) {
        if (error) *error = "invalid info reply length";
        return false;
    }
    std::vector<ChannelConfig> rows(6);
    for (uint8_t i = 0; i < 6; ++i) {
        ParseConfigRow(reply.data() + i * kFrameSize, i, &rows[i]);
    }
    if (out) *out = std::move(rows);
    return true;
}

bool Client::QueryBuffers(std::vector<BufferStatus>* out, int timeout_ms,
                          std::string* error)
{
    std::vector<uint8_t> payload = {0xFF, 0xFF, 0xFF, 0xFF};
    std::vector<uint8_t> reply;
    if (!impl_->Request(kCmdBufferQuery, payload, &reply, timeout_ms, error)) {
        return false;
    }
    if (reply.size() == 4) {
        if (error) *error = "device rejected buffer query";
        return false;
    }
    if (reply.size() != 6 * 16) {
        if (error) *error = "invalid buffer reply length";
        return false;
    }
    std::vector<BufferStatus> rows(6);
    for (uint8_t i = 0; i < 6; ++i) {
        const uint8_t* p = reply.data() + i * 16;
        rows[i].channel = p[0];
        rows[i].normal_space = GetBe32(p + 4);
        rows[i].pulse_space = GetBe32(p + 8);
        rows[i].sim_space = GetBe32(p + 12);
    }
    if (out) *out = std::move(rows);
    return true;
}

bool Client::Version(bool fpga, uint32_t* version, int timeout_ms,
                     std::string* error)
{
    std::vector<uint8_t> payload(4);
    PutBe32(payload.data(), fpga ? 0xBB : 0xAA);
    std::vector<uint8_t> reply;
    if (!impl_->Request(kCmdVersionQuery, payload, &reply, timeout_ms, error)) {
        return false;
    }
    if (reply.size() < 4) {
        if (error) *error = "invalid version reply";
        return false;
    }
    if (version) *version = GetBe32(reply.data());
    return true;
}

bool Client::Send(const Frame& frame, Frame* feedback, int timeout_ms,
                  std::string* error)
{
    if (frame.channel >= 6) {
        if (error) *error = "channel must be 0..5";
        return false;
    }
    if (frame.remote && frame.fd) {
        if (error) *error = "CAN FD does not support remote frames";
        return false;
    }
    if (!frame.fd && frame.data.size() > 8) {
        if (error) *error = "classic CAN data must be <= 8 bytes";
        return false;
    }
    if (frame.data.size() > 64) {
        if (error) *error = "CAN FD data must be <= 64 bytes";
        return false;
    }
    if ((!frame.extended && frame.can_id > 0x7FF) ||
        (frame.extended && frame.can_id > 0x1FFFFFFF)) {
        if (error) *error = "CAN id out of range";
        return false;
    }
    if (!impl_->opened) {
        if (error) *error = "client is not open";
        return false;
    }

    uint32_t total_count;
    uint32_t ch_count;
    {
        std::lock_guard<std::mutex> lock(impl_->counter_mu);
        impl_->total_tx = impl_->total_tx + 1;
        impl_->ch_tx[frame.channel] = impl_->ch_tx[frame.channel] + 1;
        total_count = impl_->total_tx;
        ch_count = impl_->ch_tx[frame.channel];
    }
    const uint8_t msg_type =
        frame.msg_type == kTypePulse ? kTypePulse : kTypeNormal;
    const uint16_t index =
        msg_type == kTypePulse ? kDataIndexPulse : kDataIndexNormal;
    const std::vector<uint8_t> packet =
        BuildData(frame, total_count, ch_count, msg_type);
    const std::pair<uint8_t, uint32_t> key(frame.channel, ch_count);
    {
        std::lock_guard<std::mutex> lock(impl_->wait_mu);
        impl_->tx_wait[key] = Impl::TxWait();
    }
    const bool sent = SendDataPacket(impl_->data, index, frame.channel, packet);
    if (!sent) {
        std::lock_guard<std::mutex> lock(impl_->wait_mu);
        impl_->tx_wait.erase(key);
        if (error) *error = "send data failed";
        return false;
    }
    std::unique_lock<std::mutex> lock(impl_->wait_mu);
    const bool ready = impl_->tx_cv.wait_for(
        lock, std::chrono::milliseconds(std::max(0, timeout_ms)),
        [&] {
            auto it = impl_->tx_wait.find(key);
            return impl_->stop.load() ||
                   (it != impl_->tx_wait.end() && it->second.done);
        });
    if (!ready || impl_->stop.load()) {
        impl_->tx_wait.erase(key);
        if (error) *error = "wait for 0x80 feedback timeout";
        return false;
    }
    Frame result = impl_->tx_wait[key].frame;
    impl_->tx_wait.erase(key);
    lock.unlock();
    if (feedback) {
        *feedback = std::move(result);
    }
    return true;
}

bool Client::Send(uint8_t channel, uint32_t can_id,
                  const std::vector<uint8_t>& data, bool fd, bool brs,
                  bool extended, bool remote, bool pulse, Frame* feedback,
                  int timeout_ms, std::string* error)
{
    Frame frame;
    frame.channel = channel;
    frame.can_id = can_id;
    frame.data = data;
    frame.fd = fd;
    frame.brs = brs;
    frame.extended = extended;
    frame.remote = remote;
    frame.msg_type = pulse ? kTypePulse : kTypeNormal;
    return Send(frame, feedback, timeout_ms, error);
}

bool Client::Receive(Frame* frame, int timeout_ms)
{
    if (!impl_->opened) {
        return false;
    }
    std::unique_lock<std::mutex> lock(impl_->rx_mu);
    const bool ready = impl_->rx_cv.wait_for(
        lock, std::chrono::milliseconds(std::max(0, timeout_ms)),
        [&] { return !impl_->rxq.empty() || impl_->stop.load(); });
    if (!ready || impl_->rxq.empty()) {
        return false;
    }
    if (frame) {
        *frame = std::move(impl_->rxq.front());
    }
    impl_->rxq.pop_front();
    return true;
}

void Client::SetFrameCallback(std::function<void(const Frame&)> callback)
{
    std::lock_guard<std::mutex> lock(impl_->cb_mu);
    impl_->callback = std::move(callback);
}

}  // namespace canfd
