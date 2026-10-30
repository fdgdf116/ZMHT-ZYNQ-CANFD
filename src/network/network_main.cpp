#include "network_main.h"
#include "sync_send.h"

#include <stdio.h>
#include <string.h>
#include <vector>
#include <chrono>
#include "fifo_engine.h"

namespace {

static void put_le16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

static void put_le32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

NetHeader HeaderFromRequest(const Request* request)
{
    NetHeader header;
    header.cmd_code = static_cast<uint32_t>(request->cmd_code);
    header.length = request->length > 0 ? static_cast<uint32_t>(request->length) : 0;
    header.count = ntohs(request->count);
    header.index = request->index;
    header.time = ntohs(request->time);
    header.crc = ntohs(request->crc);
    return header;
}

void StoreRequest(std::vector<uint8_t>* buf, const NetHeader* header,
                  const uint8_t* payload, uint32_t length)
{
    buf->assign(sizeof(Request) + length, 0);
    Request* request = reinterpret_cast<Request*>(buf->data());
    request->cmd_code = static_cast<int>(header->cmd_code);
    request->length = static_cast<int>(length);
    request->count = htons(header->count);
    request->index = htons(header->index);
    request->time = htons(header->time);
    request->crc = htons(header->crc);
    if (length > 0 && payload != nullptr) {
        memcpy(request + 1, payload, length);
    }
}

}  // namespace

namespace network {

NetServer::AnswerMapEntry NetServer::mapEntry_[] = {
    { (unsigned short)0x0100, &NetServer::CanBufQuery },
    { (unsigned short)0x0101, &NetServer::CanConfig },
    { (unsigned short)0x0102, &NetServer::CanInfoQuery },
    { (unsigned short)0x0103, &NetServer::CanStartStop },
    { (unsigned short)0xFF05, &NetServer::VersionQuery },
    { (unsigned short)0, NULL },
};

NetServer::AnswerMapEntry NetServer::DatamapEntry_[] = {
    { (unsigned short)0x0001, &NetServer::NormalData },
    { (unsigned short)0x0002, &NetServer::PulseData },
    { (unsigned short)0x0003, &NetServer::SimulatorData },
    { (unsigned short)0, NULL },
};

NetServer::NetServer()
    : server_(nullptr)
    , client_(nullptr)
    , port_(0)
    , report_stop_(true)
{
}

NetServer::~NetServer()
{
    report_stop_ = true;
    if (report_thread_.joinable()) report_thread_.join();
    thread_.Interrupt();
    if (client_ != nullptr) {
        client_->Close();
    }
    if (server_ != nullptr) {
        server_->Close();
    }
    thread_.Join();
    DropClient();
    delete server_;
    server_ = nullptr;
}

int NetServer::Init(int port)
{
    if (port != CMD_PORT && port != DATA_DOWN_PORT) {
        return -1;
    }
    if (server_) {
        return 0;
    }
    port_ = port;
    server_ = new TcpSocket();
    if (server_->CreateServer(port)) {
        printf("[CANFD] create port %d failed\n", port);
        delete server_;
        server_ = nullptr;
        return -1;
    }
    printf("[CANFD] port %d ready\n", port);
    bool started = false;
    if (port == CMD_PORT) {
        started = thread_.Run(&NetServer::ServiceLoop, this, Thread::PRIORITY_HIGH);
    } else {
        started = thread_.Run(&NetServer::DataServiceLoop, this, Thread::PRIORITY_HIGH);
        if (started) {
            report_stop_ = false;
            report_thread_ = std::thread(&NetServer::CapacityReportLoop, this);
        }
    }
    return started ? 0 : -1;
}

void NetServer::DropClient()
{
    delete client_;
    client_ = nullptr;
}

const uint8_t* NetServer::Payload(Request* request) const
{
    ClassRequest view(request);
    if (view.GetPayloadSize() <= 0) {
        return nullptr;
    }
    return static_cast<const uint8_t*>(view.GetPayloadAddr());
}

int NetServer::SendCommandResponse(const CommandResponse& response)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    const void* body = response.payload.empty() ? nullptr : response.payload.data();
    // 9009 only receives. The reply leaves on 9010 and is dropped if nobody is connected.
    int ret = canfd_send_response(response.index, response.count, response.time,
                                  body, static_cast<uint32_t>(response.payload.size()));
    unsigned status = 0xFFFFFFFFu;
    if (response.payload.size() == 4 && body != nullptr) {
        const uint8_t* p = static_cast<const uint8_t*>(body);
        status = static_cast<unsigned>(p[0]) |
                 (static_cast<unsigned>(p[1]) << 8) |
                 (static_cast<unsigned>(p[2]) << 16) |
                 (static_cast<unsigned>(p[3]) << 24);
    }
    const char* status_text = "n/a";
    if (status != 0xFFFFFFFFu) {
        status_text = (status == 0 || status == 0xAA) ? "OK" :
                      ((status == 1 || status == 0xBB) ? "FAIL" : "UNKNOWN");
    }
    printf("[CANFD RESP] port=%d index=0x%04X count=%u len=%u status=%s(0x%08X) rc=%d\n",
           port_, response.index, response.count,
           static_cast<unsigned>(response.payload.size()),
           status_text, status, ret);
    if (ret != 0 && port_ == CMD_PORT) {
        printf("[CANFD] port %d response dropped, 9010 has no client\n", port_);
    }
    return 0;
}

int NetServer::SendCapacityReport()
{
    /* Keep the legacy ground-station format: report 32 slots, while only
     * channels 0..5 are backed by hardware.  Unsupported slots are 0xFFFFFFFF. */
    const unsigned report_channels = 32;
    Cache_report_inf_t capacity[report_channels] = {};
    Cache_report_inf_t hw_capacity[CANFD_CHANNEL_COUNT] = {};
    if (axififo_query_capacity(hw_capacity) != 0) return -1;
    for (unsigned i = 0; i < report_channels; ++i) {
        capacity[i].chn_id = static_cast<unsigned char>(i);
        capacity[i].can_space = 0xFFFFFFFFu;
        capacity[i].pulse_space = 0xFFFFFFFFu;
        capacity[i].simulation_space = 0xFFFFFFFFu;
        capacity[i].dmaddr_space = 0xFFFFFFFFu;
        capacity[i].vfifo_space = 0xFFFFFFFFu;
        capacity[i].axififo_space = 0xFFFFFFFFu;
    }
    for (unsigned i = 0; i < CANFD_CHANNEL_COUNT; ++i) capacity[i] = hw_capacity[i];
    std::vector<uint8_t> payload(report_channels * sizeof(Cache_report_inf_t), 0);
    for (unsigned i = 0; i < report_channels; ++i) {
        uint8_t* p = payload.data() + i * sizeof(Cache_report_inf_t);
        p[0] = capacity[i].chn_id;
        put_le32(p + 1, capacity[i].can_space);
        put_le32(p + 5, capacity[i].pulse_space);
        put_le32(p + 9, capacity[i].simulation_space);
        put_le32(p + 13, capacity[i].dmaddr_space);
        put_le32(p + 17, capacity[i].vfifo_space);
        put_le32(p + 21, capacity[i].axififo_space);
    }
    NetHeader h = {};
    h.cmd_code = NET_RES_CODE;
    h.length = static_cast<uint32_t>(payload.size());
    h.count = 0;
    h.index = 0x0001;
    h.time = 0;
    uint8_t raw[16];
    net_header_encode(raw, h);
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (client_ == nullptr || !client_->IsConnected()) return -1;
    const bool ok = client_->SendFully(raw, sizeof(raw)) &&
                    client_->SendFully(payload.data(), static_cast<int>(payload.size()));
    return ok ? 0 : -1;
}

void NetServer::CapacityReportLoop()
{
    std::this_thread::sleep_for(std::chrono::seconds(5));
    while (!report_stop_) {
        if (port_ == DATA_DOWN_PORT && client_ != nullptr) {
            (void)SendCapacityReport();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

void NetServer::ProcMessage(char* msg)
{
    Request* request = reinterpret_cast<Request*>(msg);
    request->index = ntohs(request->index);
    const AnswerMapEntry* map_entry = mapEntry_;
    while (map_entry->index != 0) {
        if (map_entry->index == request->index) {
            (this->*map_entry->func)(request);
            return;
        }
        map_entry++;
    }
    ReportUnknowRequest(request);
}

int NetServer::DataProcMessage(char* msg)
{
    Request* request = reinterpret_cast<Request*>(msg);
    request->index = ntohs(request->index);
    if (request->cmd_code != DATA_CODE) {
        printf("[CANFD 9012] reject index=0x%04X length=%u\n",
               request->index, static_cast<unsigned>(request->length));
        return -1;
    }
    const AnswerMapEntry* map_entry = DatamapEntry_;
    while (map_entry->index != 0) {
        if (map_entry->index == request->index) {
            int rc = (this->*map_entry->func)(request);
            if (rc != 0) {
                printf("[CANFD 9012] reject index=0x%04X length=%u\n",
                       request->index, static_cast<unsigned>(request->length));
            }
            return rc;
        }
        map_entry++;
    }
    printf("[CANFD 9012] reject index=0x%04X length=%u\n",
           request->index, static_cast<unsigned>(request->length));
    return -1;
}

void NetServer::ReportUnknowRequest(Request* request)
{
    printf("[CANFD 9009] unknown command 0x%04X\n", request->index);
    reply_ = canfd_cmd_status(HeaderFromRequest(request), 1);
    SendCommandResponse(reply_);
}

int NetServer::CanBufQuery(Request* request)
{
    NetHeader header = HeaderFromRequest(request);
    reply_ = canfd_cmd_buf_query(header, Payload(request), header.length);
    return SendCommandResponse(reply_);
}

int NetServer::CanConfig(Request* request)
{
    NetHeader header = HeaderFromRequest(request);
    reply_ = canfd_cmd_config(header, Payload(request), header.length);
    return SendCommandResponse(reply_);
}

int NetServer::CanInfoQuery(Request* request)
{
    NetHeader header = HeaderFromRequest(request);
    reply_ = canfd_cmd_info_query(header, Payload(request), header.length);
    return SendCommandResponse(reply_);
}

int NetServer::CanStartStop(Request* request)
{
    NetHeader header = HeaderFromRequest(request);
    reply_ = canfd_cmd_start_stop(header, Payload(request), header.length);
    return SendCommandResponse(reply_);
}

int NetServer::VersionQuery(Request* request)
{
    NetHeader header = HeaderFromRequest(request);
    reply_ = canfd_cmd_version(header, Payload(request), header.length);
    return SendCommandResponse(reply_);
}

int NetServer::NormalData(Request* request)
{
    NetHeader header = HeaderFromRequest(request);
    return canfd_data_normal(Payload(request), header.length);
}

int NetServer::PulseData(Request* request)
{
    NetHeader header = HeaderFromRequest(request);
    return canfd_data_pulse(Payload(request), header.length);
}

int NetServer::SimulatorData(Request* request)
{
    NetHeader header = HeaderFromRequest(request);
    return canfd_data_simulator(Payload(request), header.length);
}

CommandResponse NetServer::DispatchCommand(const NetHeader* header, const uint8_t* payload, uint32_t length)
{
    reply_ = CommandResponse();
    if (header == nullptr || header->length != length || (length > 0 && payload == nullptr)) {
        if (header != nullptr) {
            reply_ = canfd_cmd_status(*header, 1);
        }
        return reply_;
    }
    std::vector<uint8_t> msg;
    StoreRequest(&msg, header, payload, length);
    ProcMessage(reinterpret_cast<char*>(msg.data()));
    return reply_;
}

int NetServer::DispatchData(const NetHeader* header, const uint8_t* payload, uint32_t length)
{
    if (header == nullptr || header->cmd_code != NET_DATA_CODE || header->length != length ||
        (length > 0 && payload == nullptr)) {
        uint16_t index = header == nullptr ? 0 : header->index;
        printf("[CANFD 9012] reject index=0x%04X length=%u\n", index, length);
        return -1;
    }
    std::vector<uint8_t> msg;
    StoreRequest(&msg, header, payload, length);
    return DataProcMessage(reinterpret_cast<char*>(msg.data()));
}

void NetServer::ReceiveLoop(Thread* thread, uint32_t expect_code, uint32_t max_payload, bool command)
{
    while (thread->IsInterrupted() == false) {
        if (client_ == nullptr) {
            client_ = server_->Accept(10);
            if (client_ != nullptr && !client_->IsConnected()) {
                DropClient();
            }
            if (client_ != nullptr) {
                client_->SetRecvTimeout(0);
                client_->SetSendTimeout(3000);
                client_->set_keepalive(3, 3, 3);
                printf("[CANFD] client connected on port %d\n", port_);
            }
        }
        if (client_ == nullptr) {
            continue;
        }

        /* The 9012 data packet header is:
         * sync/length/count/type/checksum (16 bytes) + channel (4 bytes).
         * Its length field describes only the following payload. */
        const int wire_header_size = command ? MESSAGE_HEAD_SIZE : DATA_HEADER_SIZE;
        char recv_buf[DATA_HEADER_SIZE] = {0};
        int ret = client_->RecvFully(recv_buf, wire_header_size);
        if (ret <= 0) {
            printf("[CANFD] client left port %d\n", port_);
            DropClient();
            continue;
        }

        /* Validate the data header while it is still in network byte order.
         * The two fields below are normalized in place afterwards. */
        const uint8_t* wire_header = reinterpret_cast<const uint8_t*>(recv_buf);
        uint32_t packet_channel = 0;
        bool data_header_ok = true;
        uint32_t wire_checksum = 0;
        uint32_t calculated_checksum = 0;
        if (!command) {
            wire_checksum = canfd_get_be32(wire_header + 12);
            calculated_checksum = canfd_get_be32(wire_header) ^
                                  canfd_get_be32(wire_header + 4) ^
                                  canfd_get_be32(wire_header + 8);
            data_header_ok = wire_checksum == calculated_checksum;
            packet_channel = canfd_get_be32(wire_header + MESSAGE_HEAD_SIZE);
        }

        Request* req_head = reinterpret_cast<Request*>(recv_buf);
        req_head->cmd_code = static_cast<int>(ntohl(static_cast<uint32_t>(req_head->cmd_code)));
        req_head->length = static_cast<int>(ntohl(static_cast<uint32_t>(req_head->length)));
        if (static_cast<uint32_t>(req_head->cmd_code) != expect_code ||
            req_head->length < 0 ||
            static_cast<uint32_t>(req_head->length) > max_payload) {
            printf("[CANFD] port %d close, code=0x%08X length=%u\n",
                   port_, static_cast<unsigned>(req_head->cmd_code),
                   static_cast<unsigned>(req_head->length));
            DropClient();
            continue;
        }

        std::vector<uint8_t> msg(sizeof(Request) + static_cast<uint32_t>(req_head->length));
        memcpy(msg.data(), recv_buf, sizeof(Request));
        if (req_head->length > 0 &&
            !client_->RecvFully(msg.data() + sizeof(Request), req_head->length)) {
            printf("[CANFD] client left port %d during payload\n", port_);
            DropClient();
            continue;
        }
        if (command) {
            ProcMessage(reinterpret_cast<char*>(msg.data()));
        } else {
            if (!data_header_ok) {
                printf("[CANFD 9012] reject header checksum wire=0x%08X calculated=0x%08X\n",
                       wire_checksum, calculated_checksum);
                continue;
            }
            if (packet_channel >= CANFD_CHANNEL_COUNT) {
                printf("[CANFD 9012] reject index=0x%04X channel=%u length=%u\n",
                       ntohs(req_head->index), packet_channel,
                       static_cast<unsigned>(req_head->length));
                continue;
            }
            DataProcMessage(reinterpret_cast<char*>(msg.data()));
        }
    }
}

void NetServer::ServiceLoop(Thread* thread)
{
    ReceiveLoop(thread, CMD_CODE, CANFD_CMD_MAX_PAYLOAD, true);
}

void NetServer::DataServiceLoop(Thread* thread)
{
    ReceiveLoop(thread, DATA_CODE, CANFD_DATA_MAX_PAYLOAD, false);
}

}  // namespace network

CommandResponse canfd_handle_command(const NetHeader& header, const uint8_t* payload, uint32_t length)
{
    network::NetServer server;
    return server.DispatchCommand(&header, payload, length);
}

int canfd_handle_downlink(const NetHeader& header, const uint8_t* payload, uint32_t length)
{
    network::NetServer server;
    return server.DispatchData(&header, payload, length);
}
