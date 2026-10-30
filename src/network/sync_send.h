#ifndef SRC_SYNC_SEND_H_
#define SRC_SYNC_SEND_H_

#include "eth_send.h"
#include "thread.h"
#include <stdint.h>

namespace network {

class SyncSend : public EthSend {
public:
    static SyncSend* GetInstance();
    SyncSend(const SyncSend&) = delete;
    SyncSend& operator=(const SyncSend&) = delete;
    int Init();
    void DeInit();
    bool IsActive();
    int SendResponse(uint16_t index, uint16_t count, uint16_t time, const void* payload, uint32_t len);
    int SendUplink(uint16_t index, const void* payload, uint32_t len);
    int Send(const char* buf, int size);

private:
    SyncSend(const char* ip, int port);
    virtual ~SyncSend();
    int SendLocked(uint32_t code, uint16_t index, uint16_t count, uint16_t time,
                   const void* payload, uint32_t len, int channel);
    void DaemodLoop(Thread* thread);

    Thread daemond_thread_;
    Mutex send_mutex_;
    uint16_t uplink_count_;
    static SyncSend* instance_;
};

}  // namespace network

int canfd_send_response(uint16_t index, uint16_t count, uint16_t time,
                        const void* payload, uint32_t len);
int canfd_send_uplink(uint16_t index, const void* payload, uint32_t len);

#endif /* SRC_SYNC_SEND_H_ */
