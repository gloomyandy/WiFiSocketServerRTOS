/*
 * Listener.cpp
 *
 *  Created on: 12 Apr 2017
 *      Author: David
 */
#include <cstring>
#include <new>

#include "lwip/tcp.h"

#include "Listener.h"
#include "Config.h"

// Static member data
Listener *Listener::activeList = nullptr;
Listener *Listener::freeList = nullptr;

/*static*/ bool Listener::Start(uint16_t port, uint32_t ip, int protocol, int maxConns, struct netconn * conn)
{
	Listener *res = freeList;

	if (res == nullptr)
	{
		res = new (std::nothrow) Listener;

		if (res)
		{
			res->next = freeList;
			freeList = res;
		}
	}

	if (res)
	{
		// Since the member 'socket' is not used, use it to store
		// reference to the owning Listener of the netconn.
		static_assert(sizeof(conn->socket) == sizeof(res));
		conn->socket = reinterpret_cast<int>(res);

		err_t rc = netconn_listen_with_backlog(conn, Backlog);

		if (rc != ERR_OK)
		{
			netconn_close(conn);
			netconn_delete(conn);
			debugPrintAlways("netconn_listen failed\n");
			return false;
		}

		res->ip = ip;
		res->port = port;
		res->protocol = protocol;
		res->maxConnections = maxConns;
		res->conn = conn;

		freeList = res->next; 
		res->next = activeList;
		activeList = res;
	}
	else
	{
		debugPrintAlways("can't allocate listener\n");
		return false;
	}

	return true;
}


/*static*/ void Listener::Stop()
{
	netconn_close(conn);
	netconn_delete(conn);

	Listener **pp = &activeList;
	while (*pp != nullptr)
	{
		if (*pp == this)
		{
			*pp = next;
			next = nullptr;
			return;
		}
	}

	next = freeList;
	freeList = this;
}
// End
