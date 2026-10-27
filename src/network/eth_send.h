/*
 * eth_end.h
 *
 *  Created on: 2021年4月14日
 *      Author: Administrator
 */

#ifndef SRC_ETH_SEND_H_
#define SRC_ETH_SEND_H_
#include "tcp_socket.h"
#include <thread>
#include <atomic>

namespace network {
using namespace std;
//tcp severport for send data to client; base class
class EthSend {
public:
	EthSend(const char* ip, int port){local_ip_ = ip; port_ = port; connected_ = false; is_interrupt_ = false;};
	virtual ~EthSend(){};
	virtual int Init() = 0;
	virtual bool IsActive() = 0;
	virtual int Send(const char* buf, int size) = 0;
protected:
	TcpSocket *server_;
	TcpSocket *exc_client_;
	atomic<bool> connected_;
	atomic<bool> is_interrupt_;
	const char* local_ip_;
	int port_;
};

} /* namespace network */

#endif /* SRC_ETH_SEND_H_ */
