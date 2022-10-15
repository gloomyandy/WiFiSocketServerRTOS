/*
 * Listener.h
 *
 *  Created on: 12 Apr 2017
 *      Author: David
 */

#ifndef SRC_LISTENER_H_
#define SRC_LISTENER_H_

#include <cstdint>
#include <cstddef>

#include "lwip/api.h"

class Listener
{
public:
	static bool Start(uint16_t port, uint32_t ip, int protocol, int maxConns, struct netconn* conn);
	static Listener* List() { return activeList; }

	uint32_t GetIp() { return ip; }
	uint16_t GetPort() { return port; }
	uint8_t GetProtocol() { return protocol; }
	uint16_t GetMaxConnections() { return maxConnections; }
	Listener* GetNext() { return next; }

	void Stop();

private:
	Listener *next;
	struct netconn *conn;

	uint32_t ip;
	uint16_t port;
	uint16_t maxConnections;
	uint8_t protocol;

	static Listener *activeList;
	static Listener *freeList;
};

#endif /* SRC_LISTENER_H_ */
