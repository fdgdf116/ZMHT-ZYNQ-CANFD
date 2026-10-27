#ifndef __NETWORK_MAIN_H_
#define __NETWORK_MAIN_H_

#include "protocol.h"
#include "cmd_packet.h"
#include "tcp_socket.h"
#include "thread.h"
#include <mutex>
#include <atomic>
#include <thread>

namespace network {

#define CMD_PORT 9009
#define DATA_DOWN_PORT 9012

class NetServer {
public:
    NetServer();
    ~NetServer();
    int Init(int port);
    CommandResponse DispatchCommand(const NetHeader* header, const uint8_t* payload, uint32_t length);
    int DispatchData(const NetHeader* header, const uint8_t* payload, uint32_t length);

private:
    typedef int (NetServer::*Callback)(Request*);
    typedef struct {
        unsigned short index;
        Callback func;
    } AnswerMapEntry;
    static AnswerMapEntry mapEntry_[];
    static AnswerMapEntry DatamapEntry_[];

    Thread thread_;
    TcpSocket* server_;
    TcpSocket* client_;
    int port_;
    CommandResponse reply_;
    std::mutex io_mutex_;
    std::atomic<bool> report_stop_;
    std::thread report_thread_;

    void ServiceLoop(Thread* thread);
    void DataServiceLoop(Thread* thread);
    void CapacityReportLoop();
    int SendCapacityReport();
    void ReceiveLoop(Thread* thread, uint32_t expect_code, uint32_t max_payload, bool command);
    void DropClient();
    void ProcMessage(char* msg);
    int DataProcMessage(char* msg);
    void ReportUnknowRequest(Request* request);
    int SendCommandResponse(const CommandResponse& response);
    const uint8_t* Payload(Request* request) const;

    int CanBufQuery(Request* request);
    int CanConfig(Request* request);
    int CanInfoQuery(Request* request);
    int CanStartStop(Request* request);
    int VersionQuery(Request* request);

    int NormalData(Request* request);
    int PulseData(Request* request);
    int SimulatorData(Request* request);
};

}  // namespace network

#endif /* __NETWORK_MAIN_H_ */
