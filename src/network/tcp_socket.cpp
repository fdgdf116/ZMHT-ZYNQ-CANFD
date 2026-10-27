#include "net_common.h"
#include "tcp_socket.h"
#include <vector>

namespace network {

TcpSocket::TcpSocket(){
    socket_ = -1;
}
TcpSocket::TcpSocket(int fd){
    socket_ = fd;
}
TcpSocket::~TcpSocket() {
    if(socket_ != -1 ) {
        shutdown(socket_,SHUT_RDWR);close(socket_);socket_=-1;
    }
}
int TcpSocket::CreateServer(int port){
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr=INADDR_ANY;
    socket_ = socket(AF_INET,SOCK_STREAM,0);
    if( socket_ == -1 ) {
        printf("8.create socket errno=%d:%s!!!!!!",errno,strerror(errno));return -1;
    }
    int reuse = 1;
    setsockopt(socket_,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    if( bind(socket_,(struct sockaddr *)&server_addr,sizeof(server_addr)) == -1 ){
        printf("8.bind errno=%d:%s!!!!!!",errno,strerror(errno));return -1;
    }
    if( listen(socket_,5) == -1 ) {
        printf("8.listen errno=%d:%s!!!!!!",errno,strerror(errno));return -1;
    }
    return 0;
}
int TcpSocket::CreateServer(int port,const char * ip){
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr=inet_addr(ip);
    socket_ = socket(AF_INET,SOCK_STREAM,0);
    if( socket_ == -1 ) {
        printf("8.create socket errno=%d:%s!!!!!!",errno,strerror(errno));return -1;
    }
    int reuse = 1;
    setsockopt(socket_,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    if( bind(socket_,(struct sockaddr *)&server_addr,sizeof(server_addr)) == -1 ){
        printf("8.bind errno=%d:%s!!!!!!",errno,strerror(errno));return -1;
    }
    if( listen(socket_,5) == -1 ) {
        printf("8.listen errno=%d:%s!!!!!!",errno,strerror(errno));return -1;
    }
    return 0;
}
bool TcpSocket::Connect(const char * address,int port,int timeout){
    struct sockaddr_in server = {0} ;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(address);
    // ret = inet_aton(address,&server.sin_addr); ret=0 error.
    socket_ = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(socket_==-1) {
        printf("8.create socket errno=%d:%s!!!!!!",errno,strerror(errno));return -1;
    }
    
    unsigned long ul = 1;
	ioctl(socket_, FIONBIO, &ul);
    
    if( connect(socket_,(const sockaddr *)&server,sizeof(server)) != 0 ) {
        int error=-1,len=sizeof(int);
        struct timeval tm;
        tm.tv_sec=0;tm.tv_usec=timeout*1000;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(socket_,&set);
        int ret = select(socket_+1,NULL,&set,NULL,&tm);
        if(ret>0) {
            getsockopt(socket_,SOL_SOCKET,SO_ERROR,&error,(socklen_t *)&len);
        }
        if( error != 0 ) {
            Close();return false;
        }
    }
    
    ul = 0;
	ioctl(socket_, FIONBIO, &ul);
    return true;
}
TcpSocket * TcpSocket::Accept(int timeout){
    struct timeval tv;
	tv.tv_sec = timeout;
	tv.tv_usec = 0;
	fd_set read_set;
	FD_ZERO(&read_set);
	FD_SET(socket_, &read_set);

	int ret = select(socket_+1, &read_set,NULL,NULL,&tv);
    if(ret>0) {
        if(FD_ISSET(socket_,&read_set)) {
            struct sockaddr_in client_address;
            socklen_t address_len = sizeof(client_address);
            int fd = accept(socket_,(struct sockaddr *)&client_address,&address_len);
            printf("8.accept client fd = %d ip = %s:%d",fd,inet_ntoa(client_address.sin_addr),ntohs(client_address.sin_port));
            return new TcpSocket(fd);
        }
    }
    return nullptr;
}
int TcpSocket::Send(const void *buffer,int size){
	return send(socket_, buffer, size, 0);
}
int TcpSocket::Recv(void * buffer,int size){
	return recv(socket_, buffer, size, 0);
}
bool TcpSocket::RecvFully(void * buffer,int size){
    int recv = 0;
    while( recv < size ) {
        int ret = Recv((char *)buffer+recv,size-recv);
        if(ret<=0) {
            return false;
        }
        recv += ret;
    }
    return true;
}
bool TcpSocket::SendFully(const void *buffer,int size){
    const char * p = (const char *)buffer;
    int send = 0;
    while( send < size ) {
        int ret = Send(p,size-send);
		if(ret<=0) {
            return false;
        }
        p+=ret;
        send += ret;
    }
    return true;
}
bool TcpSocket::SendVectorFully(const struct iovec *buffers, int count){
    if(count < 0 || (count > 0 && buffers == nullptr)) return false;
    if(count == 0) return true;
    std::vector<struct iovec> remaining(buffers, buffers + count);
    size_t first = 0;
    while(first < remaining.size()) {
        while(first < remaining.size() && remaining[first].iov_len == 0) ++first;
        if(first == remaining.size()) return true;
        struct msghdr message = {};
        message.msg_iov = remaining.data() + first;
        message.msg_iovlen = remaining.size() - first;
        ssize_t sent = sendmsg(socket_, &message, MSG_NOSIGNAL);
        if(sent < 0 && errno == EINTR) continue;
        // A send timeout or disconnect aborts this frame, as with SendFully.
        if(sent <= 0) return false;
        size_t consumed = static_cast<size_t>(sent);
        while(first < remaining.size() && consumed >= remaining[first].iov_len) {
            consumed -= remaining[first].iov_len;
            ++first;
        }
        if(first < remaining.size() && consumed != 0) {
            remaining[first].iov_base =
                static_cast<char*>(remaining[first].iov_base) + consumed;
            remaining[first].iov_len -= consumed;
        }
    }
    return true;
}

void TcpSocket::Close(){
    if(socket_!=-1){
        shutdown(socket_,SHUT_RDWR);close(socket_);socket_=-1;
    }
}
int TcpSocket::GetSocketId(){
    return socket_;
}
bool TcpSocket::IsAlive(){
    struct tcp_info info;
    int len = sizeof(info);
    getsockopt(socket_, IPPROTO_TCP, TCP_INFO, &info, (socklen_t *) & len);
    if ((info.tcpi_state == TCP_ESTABLISHED)) {
        // log("8.tcp is alive......\n");    
        return true;
    } else {
        // log("8.tcp is not alive.!!!!!!\n");
        return false;
    }
}
bool TcpSocket::IsConnected() const{
    return socket_ != -1;
}
void TcpSocket::SetRecvTimeout(int milliseconds){
	timeval tm;
	tm.tv_sec = milliseconds / 1000;
	tm.tv_usec = (milliseconds % 1000) * 1000;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tm, sizeof(tm));
}
void TcpSocket::SetSendTimeout(int milliseconds){
	timeval tm;
	tm.tv_sec = milliseconds / 1000;
	tm.tv_usec = (milliseconds % 1000) * 1000;
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &tm, sizeof(tm));
}
void TcpSocket::SetRecvBUfSize(int size){
	setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
}
void TcpSocket::SetSendBUfSize(int size){
	setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}
int TcpSocket::set_keepalive(int keepalive_time,int keepalive_intvl,int keepalive_probes){
    int optval;
	socklen_t optlen = sizeof(optval);
	optval = 1;
	if (-1 == setsockopt(socket_, SOL_SOCKET, SO_KEEPALIVE, &optval, optlen)) {
		perror("setsockopt failure.");
		return -1;
	}

	optval = keepalive_probes;
	if (-1 == setsockopt(socket_, SOL_TCP, TCP_KEEPCNT, &optval, optlen)) {
		perror("setsockopt failure.");
		return -1;
	}

	optval = keepalive_intvl;
	if (-1 == setsockopt(socket_, SOL_TCP, TCP_KEEPINTVL, &optval, optlen)) {
		perror("setsockopt failure.");
		return -1;
	}

	optval = keepalive_time;
	if (-1 == setsockopt(socket_, SOL_TCP, TCP_KEEPIDLE, &optval, optlen)) {
		perror("setsockopt failure.");
		return -1;
	}
	return 0;
}

} // namespace network
