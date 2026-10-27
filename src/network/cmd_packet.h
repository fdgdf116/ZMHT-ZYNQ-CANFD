#ifndef __CMD_PACKET_
#define __CMD_PACKET_

#include <string>
#include <map>
#include "net_common.h"

const int UP_CODE = 0x01CCF0FF;
const int CMD_CODE = 0x03CCF0FF;
const int RES_CODE = 0x05CCF0FF;
const int DATA_CODE = 0x04CCF0FF;

const int MESSAGE_HEAD_SIZE = 16;
typedef struct Request_ {
	int cmd_code;         // 命令同步字
	int length;           // 命令数据长度
	unsigned short count; // 命令计数器
	unsigned short index; // 命令类型
	unsigned short time;  // 时间戳
	unsigned short crc;   // 校验和
}Request;
typedef struct Response_ {
	int cmd_code;
	int length;
	unsigned short count;
	unsigned short index;
	unsigned short time;
	unsigned short crc;
}Response;
typedef std::map<int,std::string> typedef_map_int_string;
class ClassRequest {
	public:
		ClassRequest(Request * request){ request_=request; }
		~ClassRequest(){ if(request_){request_=NULL;} }
		int GetCount(){ return request_->count; }
		int GetPayloadSize(){ return request_->length; }
		int GetPayload(void * dest,int offset,int size){
			if(offset+size > GetPayloadSize() )return -1;
			char * payload_addr = (char *)(request_+1);
			memcpy(dest,payload_addr+offset,size);return 0;
		}
		void * GetPayloadAddr(void){ return (void *)(request_+1); }
	private:
		ClassRequest();
		Request * request_;
};
class ClassResponse {
	public:
		ClassResponse(int payload_size) { 
			response_ = (Response *)malloc(sizeof(Response)+payload_size);
			if(response_!=NULL) {
				memset(response_,0,sizeof(Response)+payload_size);
				response_->length = htonl(payload_size);
				response_->cmd_code=htonl(RES_CODE);
			}else{ assert(0); }		
		}
		~ClassResponse(){ if(response_){ free(response_); } response_=NULL; }

		int WriteType(unsigned short status){ response_->index = htons(status); return 0; }
		int WriteReqcode(int cmd_code){ response_->cmd_code = htonl(cmd_code);return 0; }
		int WriteCount(int count){ response_->count=htons(count); return 0; }
		int GetResSize(void){ return sizeof(Response)+response_->length; }    
		int WritePayload(void * source, int offset, int size){
			char * payload_addr = (char*)(response_+1);
			memcpy(payload_addr+offset,source,size);return 0;		
		}
		Response * GetResponse(){ return response_; }
	private:
		ClassResponse();
		Response *response_; 
};

#endif // cmd_packet
