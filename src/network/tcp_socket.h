#ifndef __TCP_SOCKET_H_
#define __TCP_SOCKET_H_
#include "net_common.h"
#include <sys/uio.h>
namespace network {

class TcpSocket {
public:
    TcpSocket();
    TcpSocket(int fd);
    virtual ~TcpSocket();
    int CreateServer(int port,const char * ip);
    int CreateServer(int port);
    bool Connect(const char * address,int port,int timeout);
    TcpSocket * Accept(int timeout);
    int Send(const void *buffer,int size);
    int Recv(void * buffer,int size);
    bool RecvFully(void * buffer,int size);
    bool SendFully(const void *buffer,int size);
    // Copies descriptors only; payload remains in caller-owned memory until return.
    bool SendVectorFully(const struct iovec *buffers, int count);
    void Close();
    int GetSocketId();
    bool IsAlive();
    bool IsConnected() const;
    void SetRecvTimeout(int milliseconds);
    void SetSendTimeout(int milliseconds);
    void SetRecvBUfSize(int size);
    void SetSendBUfSize(int size);
    int set_keepalive(int keepalive_time,int keepalive_intvl,int keepalive_probes);
private:
    int socket_;
};

} // namespace network

#endif // __TCP_SOCKET_H_
