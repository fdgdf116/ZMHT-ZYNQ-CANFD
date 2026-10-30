#include "sync_send.h"
#include "protocol.h"

#include <stdio.h>

namespace network {

static void put_le16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t* p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

SyncSend* SyncSend::instance_ = nullptr;

SyncSend* SyncSend::GetInstance()
{
    /* Function-local static: initialisation is thread-safe since C++11.
     * The per-channel CAN RX threads all reach this from their own thread. */
    static SyncSend* instance = new SyncSend(nullptr, 9010);
    instance_ = instance;
    return instance;
}

SyncSend::SyncSend(const char* ip, int port)
    : EthSend(ip, port)
    , uplink_count_(0)
{
    server_ = nullptr;
    exc_client_ = nullptr;
    send_mutex_.Init();
}

SyncSend::~SyncSend()
{
    DeInit();
}

int SyncSend::Init()
{
    if (server_) {
        return OK;
    }
    server_ = new TcpSocket();
    is_interrupt_ = false;
    if (server_->CreateServer(port_)) {
        printf("[CANFD 9010] create server failed\n");
        delete server_;
        server_ = nullptr;
        return FAIL;
    }
    printf("[CANFD 9010] server ready\n");
    if (!daemond_thread_.Run(&SyncSend::DaemodLoop, this, Thread::PRIORITY_HIGH)) {
        return FAIL;
    }
    return OK;
}

void SyncSend::DeInit()
{
    is_interrupt_ = true;
    daemond_thread_.Interrupt();
    if (server_) {
        server_->Close();
    }
    daemond_thread_.Join();
    delete server_;
    server_ = nullptr;
    MutexScopedLocker locker(send_mutex_);
    delete exc_client_;
    exc_client_ = nullptr;
}

bool SyncSend::IsActive()
{
    MutexScopedLocker locker(send_mutex_);
    if (exc_client_ == nullptr) {
        return false;
    }
    return exc_client_->IsAlive();
}

int SyncSend::SendLocked(uint32_t code, uint16_t index, uint16_t count, uint16_t time,
                         const void* payload, uint32_t len, int channel)
{
    if (len > 0 && payload == nullptr) {
        return FAIL;
    }
    if (len > CANFD_DATA_MAX_PAYLOAD) {
        return FAIL;
    }
    uint8_t header[16];
    NetHeader net;
    net.cmd_code = code;
    net.length = len;
    net.count = count;
    net.index = index;
    net.time = time;
    net.crc = 0;
    net_header_encode(header, net);
    if (channel >= 0) {
        canfd_put_be32(header + 12,
                       canfd_get_be32(header) ^
                       canfd_get_be32(header + 4) ^
                       canfd_get_be32(header + 8));
    }

    if (exc_client_ == nullptr || !exc_client_->IsConnected()) {
        return FAIL;
    }
    if (!exc_client_->SendFully(header, 16)) {
        delete exc_client_;
        exc_client_ = nullptr;
        return FAIL;
    }
    if (channel >= 0) {
        uint8_t channel_bytes[4];
        canfd_put_be32(channel_bytes, static_cast<uint32_t>(channel));
        if (!exc_client_->SendFully(channel_bytes, sizeof(channel_bytes))) {
            delete exc_client_;
            exc_client_ = nullptr;
            return FAIL;
        }
    }
    if (len > 0 && !exc_client_->SendFully(payload, static_cast<int>(len))) {
        delete exc_client_;
        exc_client_ = nullptr;
        return FAIL;
    }
    return OK;
}

int SyncSend::SendResponse(uint16_t index, uint16_t count, uint16_t time,
                           const void* payload, uint32_t len)
{
    MutexScopedLocker locker(send_mutex_);
    return SendLocked(NET_RES_CODE, index, count, time, payload, len, -1);
}

int SyncSend::SendUplink(uint16_t index, const void* payload, uint32_t len)
{
    if (payload == nullptr || len != CANFD_FRAME_SIZE) {
        return FAIL;
    }
    if (index != UPLINK_INDEX_STATUS && index != UPLINK_INDEX_RX && index != UPLINK_INDEX_SIM) {
        return FAIL;
    }
    int channel = -1;
    if (index == UPLINK_INDEX_STATUS || index == UPLINK_INDEX_RX) {
        const uint8_t* frame = static_cast<const uint8_t*>(payload);
        if (frame[7] >= CANFD_CHANNEL_COUNT) {
            return FAIL;
        }
        channel = frame[7];
    }
    MutexScopedLocker locker(send_mutex_);
    const uint16_t count = static_cast<uint16_t>(uplink_count_ + 1);
    const int ret = SendLocked(NET_UP_CODE, index, count, 0, payload, len, channel);
    if (ret == OK) {
        uplink_count_ = count;
    }
    return ret;
}

int SyncSend::Send(const char* buf, int size)
{
    MutexScopedLocker locker(send_mutex_);
    if (exc_client_ == nullptr || buf == nullptr || size < 0) {
        return FAIL;
    }
    if (!exc_client_->SendFully(buf, size)) {
        delete exc_client_;
        exc_client_ = nullptr;
        return FAIL;
    }
    return OK;
}

void SyncSend::DaemodLoop(Thread* thread)
{
    (void)thread;
    while (!is_interrupt_) {
        bool connected = false;
        {
            MutexScopedLocker locker(send_mutex_);
            if (exc_client_ != nullptr) {
                if (!exc_client_->IsAlive()) {
                    printf("[CANFD 9010] client disconnected\n");
                    delete exc_client_;
                    exc_client_ = nullptr;
                } else {
                    connected = true;
                }
            }
        }
        if (!connected) {
            TcpSocket* accepted = server_->Accept(1);
            if (accepted == nullptr) {
                continue;
            }
            if (!accepted->IsConnected()) {
                delete accepted;
                continue;
            }
            accepted->SetSendBUfSize(4 * 1024 * 1024);
            accepted->SetSendTimeout(3000);
            accepted->set_keepalive(200, 60, 20);
            MutexScopedLocker locker(send_mutex_);
            if (is_interrupt_ || exc_client_ != nullptr) {
                delete accepted;
                continue;
            }
            exc_client_ = accepted;
            printf("[CANFD 9010] client connected\n");
            continue;
        }
        Thread::Sleep(1000);
    }
}

}  // namespace network

int canfd_send_response(uint16_t index, uint16_t count, uint16_t time,
                        const void* payload, uint32_t len)
{
    return network::SyncSend::GetInstance()->SendResponse(index, count, time, payload, len);
}

int canfd_send_uplink(uint16_t index, const void* payload, uint32_t len)
{
    return network::SyncSend::GetInstance()->SendUplink(index, payload, len);
}
