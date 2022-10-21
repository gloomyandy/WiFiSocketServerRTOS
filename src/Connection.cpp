/*
 * Socket.cpp
 *
 *  Created on: 11 Apr 2017
 *      Author: David
 */
#include <cstring> 			// memcpy
#include <algorithm>			// for std::min

#include "lwip/tcp.h"

#include "Listener.h"
#include "Connection.h"
#include "Misc.h"				// for millis
#include "Config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define CONNECTION_ACCEPT		(1 << 0)
#define CONNECTION_CLOSE		(1 << 1)

static const uint32_t MaxAckTime = 4000;		// how long we wait for a connection to acknowledge the remaining data before it is closed

// Public interface
Connection::Connection(uint8_t num)
	: number(num), localPort(0), remotePort(0), remoteIp(0), direction(Incoming), conn(nullptr), state(ConnState::free),
	readBuf(nullptr), readIndex(0), alreadyRead(0)
{
}

size_t Connection::Read(uint8_t *data, size_t length)
{
	size_t lengthRead = 0;
	if (readBuf != nullptr && length != 0 && (state == ConnState::connected || state == ConnState::otherEndClosed))
	{
		do
		{
			const size_t toRead = std::min<size_t>(readBuf->len - readIndex, length);
			memcpy(data + lengthRead, (uint8_t *)readBuf->payload + readIndex, toRead);
			lengthRead += toRead;
			readIndex += toRead;
			length -= toRead;
			if (readIndex != readBuf->len)
			{
				break;
			}
			pbuf * const currentPb = readBuf;
			readBuf = readBuf->next;
			currentPb->next = nullptr;
			pbuf_free(currentPb);
			readIndex = 0;
		} while (readBuf != nullptr && length != 0);

		alreadyRead += lengthRead;
		if (readBuf == nullptr || alreadyRead >= TCP_MSS)
		{
			netconn_tcp_recvd(conn, alreadyRead);
			alreadyRead = 0;
		}
	}
	return lengthRead;
}

size_t Connection::CanRead() const
{
	return ((state == ConnState::connected || state == ConnState::otherEndClosed) && readBuf != nullptr)
			? readBuf->tot_len - readIndex : 0;
}

// Write data to the connection. The amount of data may be zero.
// A note about writing:
// - LWIP is compiled with option LWIP_NETIF_TX_SINGLE_PBUF set. A comment says this is mandatory for the ESP8266.
// - A side effect of this is that when we call tcp_write, the data is always copied even if we don't set the TCP_WRITE_FLAG_COPY flag.
// - The PBUFs used to copy the outgoing data into are always large enough to accommodate the MSS. The total allocation size per PBUF is 1560 bytes.
// - Sending a full 2K of data may require 2 of these PBUFs to be allocated.
// - Due to memory fragmentation and other pending packets, this allocation is sometimes fails if we are serving more than 2 files at a time.
// - The result returned by tcp_sndbuf doesn't take account of the possibility that this allocation may fail.
// - When it receives a write request from the Duet main processor, our socket server has to say how much data it can accept before accepting it.
// - So in version 1.21 it sometimes happened that we accept some data based on the amount that tcp_sndbuf say we can, but we can't actually send it.
// - We then terminate the connection, and the client request fails.
// To mitigate this we could:
// - Have one overflow write buffer, shared between all connections
// - Only accept write data from the Duet main processor if the overflow buffer is free
// - If after accepting data from the Duet main processor we find that we can't send it, we send some of it if we can and store the rest in the overflow buffer
// - Then we push any pending data that we already have, and in Poll() we try to send the data in overflow buffer
// - When the overflow buffer is empty again, we can start accepting write data from the Duet main processor again.
// A further mitigation would be to restrict the amount of data we accept so some amount that will fit in the MSS, then tcp_write will need to allocate at most one PBUF.
// However, another reason why tcp_write can fail is because MEMP_NUM_TCP_SEG is set too low in Lwip. It now appears that this is the maoin cause of files tcp_write
// call in version 1.21. So I have increased it from 10 to 16, which seems to have fixed the problem..
size_t Connection::Write(const uint8_t *data, size_t length, bool doPush, bool closeAfterSending)
{
	if (state != ConnState::connected)
	{
		return 0;
	}

	// Try to send all the data
	const bool push = doPush || closeAfterSending;

	u8_t flag = NETCONN_COPY | (push ? NETCONN_MORE : 0);

	size_t total = 0;
	size_t written = 0;
	err_t rc = ERR_OK;

	for( ; total < length; total += written) {
		written = 0;
		rc = netconn_write_partly(conn, data + total, length - total, flag, &written);

		if (rc != ERR_OK && rc != ERR_WOULDBLOCK) {
			break;
		}
	}

	if (rc != ERR_OK)
	{
		if (rc == ERR_RST || rc == ERR_CLSD)
		{
			SetState(ConnState::otherEndClosed);
		}
		else
		{
			// We failed to write the data. See above for possible mitigations. For now we just terminate the connection.
			debugPrintfAlways("Write fail len=%u err=%d\n", total, (int)rc);
			Terminate(false);		// chrishamm: Not sure if this helps with LwIP v1.4.3 but it is mandatory for proper error handling with LwIP 2.0.3
			return 0;
		}
	}

	// Close the connection again when we're done
	if (closeAfterSending)
	{
		Close();
	}

	return length;
}

size_t Connection::CanWrite() const
{
	// Return the amount of free space in the write buffer
	// Note: we cannot necessarily write this amount, because it depends on memory allocations being successful.
	return ((state == ConnState::connected) && conn->pcb.tcp) ?
		std::min((size_t)tcp_sndbuf(conn->pcb.tcp), MaxDataLength) : 0;
}

void Connection::Poll()
{
	if (state == ConnState::connected || state == ConnState::otherEndClosed)
	{
		struct pbuf *data = nullptr;
		err_t rc = netconn_recv_tcp_pbuf_flags(conn, &data, NETCONN_NOAUTORCVD);

		while(rc == ERR_OK) {
			if (readBuf == nullptr) {
				readBuf = data;
				readIndex = alreadyRead = 0;
			} else {
				pbuf_cat(readBuf, data);
			}
			data = nullptr;
			rc = netconn_recv_tcp_pbuf_flags(conn, &data, NETCONN_NOAUTORCVD);
		}

		if (rc != ERR_WOULDBLOCK)
		{
			if (rc == ERR_RST || rc == ERR_CLSD || rc == ERR_CONN)
			{
				SetState(ConnState::otherEndClosed);
			}
			else
			{
				Terminate(false);
			}
		}
	}
	else if (state == ConnState::closePending)
	{
		Close();
	}
}

// Close the connection.
// If 'external' is true then the Duet main processor has requested termination, so we free up the connection.
// Otherwise it has failed because of an internal error, and we set the state to 'aborted'. The Duet main processor will see this and send a termination request,
// which will free it up.
void Connection::Close()
{
	if (state == ConnState::otherEndClosed ||  state == ConnState::connected)
	{
		SetState(ConnState::closePending);
		FreePbuf();
	}

	// Closing and deleting the connection is handled in a separate task.
	// While transmission has been shut down, only offically set this
	// connection free when the connection has been sent to this task.
	if (closePendingCnt < MaxConnections)
	{
		for (int i = 0; i < MaxConnections; i++)
		{
			if (closePending[i] == nullptr)
			{
				conn->socket = i;
				closePendingCnt++;
				break;
			}
		}

		netconn_shutdown(conn, false, true);

		closePending[conn->socket] = conn;
		closeTimer[conn->socket] = MaxAckTime;

		// Shut down the transmission, sending FIN to peer.
		SetState(ConnState::free);
		conn = nullptr;
	}
}

void Connection::Terminate(bool external)
{
	if (conn) {
		// Terminate immediately
		netconn_close(conn);
		netconn_delete(conn);
		conn = nullptr;
	}
	FreePbuf();
	SetState((external) ? ConnState::free : ConnState::aborted);
}

void Connection::SetConnection(struct netconn* conn, bool direction)
{
	this->conn = conn;
	localPort = conn->pcb.tcp->local_port;
	remotePort = conn->pcb.tcp->remote_port;
	remoteIp = conn->pcb.tcp->remote_ip.u_addr.ip4.addr;
	readIndex = alreadyRead = 0;
	this->direction = direction;
	SetState(ConnState::connected);
}

void Connection::GetStatus(ConnStatusResponse& resp) const
{
	resp.socketNumber = number;
	resp.state = state;
	resp.bytesAvailable = CanRead();
	resp.writeBufferSpace = CanWrite();
	resp.localPort = localPort;
	resp.remotePort = remotePort;
	resp.remoteIp = remoteIp;
	resp.direction = direction;
}

void Connection::FreePbuf()
{
	if (readBuf != nullptr)
	{
		pbuf_free(readBuf);
		readBuf = nullptr;
	}
}

void Connection::Report()
{
	// The following must be kept in the same order as the declarations in class ConnState
	static const char* const connStateText[] =
	{
		"free",
		"connecting",			// socket is trying to connect
		"connected",			// socket is connected
		"remoteClosed",			// the other end has closed the connection

		"aborted",				// an error has occurred
		"closePending",			// close this socket when sending is complete
		"closeReady"			// about to be closed
	};

	const unsigned int st = (int)state;
	ets_printf("%s", (st < ARRAY_SIZE(connStateText)) ? connStateText[st]: "unknown");
	if (state != ConnState::free)
	{
		ets_printf(" %u, %u, %u.%u.%u.%u", localPort, remotePort, remoteIp & 255, (remoteIp >> 8) & 255, (remoteIp >> 16) & 255, (remoteIp >> 24) & 255);
	}
}

// Static functions

/*static*/ void Connection::Init()
{
	xTaskCreate(ConnectionTask, "ConnectionTask", CONNECTION_TASK, NULL, CONNECTION_PRIO, &connectionTask);

	for (size_t i = 0; i < MaxConnections; ++i)
	{
		connectionList[i] = new Connection((uint8_t)i);
	}
}

/* static */ bool Connection::Connect(uint32_t remoteIp, uint16_t remotePort, uint16_t localPort)
{
	struct netconn * tempPcb = netconn_new_with_callback(NETCONN_TCP, ConnectCallback);
	if (tempPcb == nullptr)
	{
		debugPrintAlways("can't allocate connection\n");
		return false;
	}
	netconn_set_nonblocking(tempPcb, 1);
	ip_set_option(tempPcb->pcb.tcp, SOF_REUSEADDR); 			// not sure we need this, but the Arduino HTTP server does it

	err_t rc = 0;

	if (localPort)
	{
		rc = netconn_bind(tempPcb, IP_ADDR_ANY, localPort);

		if (rc != ERR_OK)
		{
			netconn_close(tempPcb);
			netconn_delete(tempPcb);
			debugPrintAlways("can't bind connection\n");
			return false;
		}
	}

	Connection * const conn = Connection::Allocate();

	if (conn == nullptr)
	{
		netconn_close(tempPcb);
		netconn_delete(tempPcb);
		debugPrintfAlways("can't connect: %d\n", (int)rc);
		return false;
	}
	conn->conn = tempPcb;

	// Since the member 'socket' is not used, use it to store
	// reference to the owning Connection of the netconn.
	static_assert(sizeof(tempPcb->socket) == sizeof(conn));
	tempPcb->socket = reinterpret_cast<int>(conn);

	ip_addr_t tempIp;
	memset(&tempIp, 0, sizeof(tempIp));
	tempIp.u_addr.ip4.addr = remoteIp;
	rc = netconn_connect(tempPcb, &tempIp, remotePort);

	if (!(rc == ERR_OK || rc == ERR_INPROGRESS))
	{
		conn->Terminate(true);
		debugPrintfAlways("can't connect: %d\n", (int)rc);
		return false;
	}

	conn->SetState(ConnState::connecting);
	return true;
}

bool Connection::Listen(uint16_t port, uint32_t ip, uint8_t protocol, uint16_t maxConns)
{
	// See if we are already listing for this
	for (Listener *p = Listener::List(); p != nullptr; )
	{
		Listener *n = p->GetNext();
		if (p->GetPort() == port)
		{
			if (maxConns != 0 && (p->GetIp() == IPADDR_ANY || p->GetIp() == ip))
			{
				// already listening, so nothing to do
				debugPrintf("already listening on port %u\n", port);
				return true;
			}
			if (maxConns == 0 || ip == IPADDR_ANY)
			{
				p->Stop();
				debugPrintf("stopped listening on port %u\n", port);
			}
		}
		p = n;
	}

	if (maxConns == 0)
	{
		return true;
	}

	// Setup LWIP listening connection.
	struct netconn * tempPcb = netconn_new_with_callback(NETCONN_TCP, ListenCallback);
	if (tempPcb == nullptr)
	{
		debugPrintAlways("can't allocate PCB\n");
		return false;
	}
	netconn_set_nonblocking(tempPcb, 1);

	ip_addr_t tempIp;
	memset(&tempIp, 0, sizeof(tempIp));
	tempIp.u_addr.ip4.addr = ip;
	ip_set_option(tempPcb->pcb.tcp, SOF_REUSEADDR); 			// not sure we need this, but the Arduino HTTP server does it
	err_t rc = netconn_bind(tempPcb, &tempIp, port);
	if (rc != ERR_OK)
	{
		netconn_close(tempPcb);
		netconn_delete(tempPcb);
		debugPrintfAlways("can't bind PCB: %d\n", (int)rc);
		return false;
	}

	return Listener::Start(port, ip, protocol, maxConns, tempPcb);
}

void Connection::Dismiss(uint16_t port)
{
	for (Listener *p = Listener::List(); p != nullptr; )
	{
		Listener *n = p->GetNext();
		if (port == 0 || port == p->GetPort())
		{
			p->Stop();
		}
		p = n;
	}
}

/*static*/ void Connection::TerminateAll()
{
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		Connection::Get(i).Terminate(true);
	}
}

/*static*/ uint16_t Connection::GetPortByProtocol(uint8_t protocol)
{
	for (Listener *p = Listener::List(); p != nullptr; p = p->GetNext())
	{
		if (p->GetProtocol() == protocol)
		{
			return p->GetPort();
		}
	}
	return 0;
}

/*static*/ void Connection::ReportConnections()
{
	ets_printf("Conns");
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		ets_printf("%c %u:", (i == 0) ? ':' : ',', i);
		connectionList[i]->Report();
	}
	ets_printf("\n");
}

/*static*/ void Connection::GetSummarySocketStatus(uint16_t& connectedSockets, uint16_t& otherEndClosedSockets)
{
	connectedSockets = 0;
	otherEndClosedSockets = 0;
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		if (Connection::Get(i).GetState() == ConnState::connected)
		{
			connectedSockets |= (1 << i);
		}
		else if (Connection::Get(i).GetState() == ConnState::otherEndClosed)
		{
			otherEndClosedSockets |= (1 << i);
		}
	}
}

/*static*/ Connection *Connection::Allocate()
{
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		if (connectionList[i]->state == ConnState::free)
		{
			return connectionList[i];
		}
	}
	return nullptr;
}

/*static*/ uint16_t Connection::CountConnectionsOnPort(uint16_t port)
{
	uint16_t count = 0;
	for (size_t i = 0; i < MaxConnections; ++i)
	{
		if (connectionList[i]->localPort == port)
		{
			const ConnState state = connectionList[i]->state;
			if (state == ConnState::connected || state == ConnState::otherEndClosed || state == ConnState::closePending)
			{
				++count;
			}
		}
	}
	return count;
}

/*static*/ void Connection::ConnectionTask(void* p)
{
	for (int i = 0; i < MaxConnections; i++)
	{
		closeTimer[i] = MaxAckTime;
		closePending[i] = nullptr;
	}

	uint32_t notifyWait = portMAX_DELAY;

	uint32_t lastLoop, lastClose;
	lastLoop = millis();
	lastClose = 0;

	while (true)
	{
		uint32_t notifyVal = 0;
		BaseType_t res = xTaskNotifyWait(0x00, ULONG_MAX, &notifyVal, 
					notifyWait == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(notifyWait));

		uint32_t currentLoop = millis();

		if (res == pdTRUE && (notifyVal & CONNECTION_ACCEPT))
		{
			for (int i = 0; i < MaxConnections; i++)
			{
				if (acceptPending[i])
				{
					struct netconn *newConn;
					err_t rc = netconn_accept(acceptPending[i], &newConn);
					if (rc == ERR_OK)
					{
						Listener* p = reinterpret_cast<Listener*>(acceptPending[i]->socket);
						const uint16_t numConns = Connection::CountConnectionsOnPort(p->GetPort());
						if (numConns < p->GetMaxConnections())
						{
							Connection * const conn = Connection::Allocate();
							if (conn != nullptr)
							{
								netconn_set_nonblocking(newConn, 1);
								conn->SetConnection(newConn, Incoming);
								if (p->GetProtocol() == protocolFtpData)
								{
									debugPrintf("accept conn, stop listen on port %u\n", port);
									p->Stop();	// don't listen for further connections
								}
							}
							else
							{
								netconn_close(newConn);
								netconn_delete(newConn);
								debugPrintfAlways("refused conn on port %u no free conn\n", p->GetPort());
							}
						}
						else
						{
							netconn_close(newConn);
							netconn_delete(newConn);
							debugPrintfAlways("refused conn on port %u already %u conns\n", p->GetPort(), numConns);
						}
					}
					else
					{
						debugPrintfAlways("Connection accept failed: %d\n", rc);
					}
					acceptPending[i] = nullptr;
					acceptPendingCnt--;
				}
			}

			if (notifyWait != portMAX_DELAY)
			{
				uint32_t diff = currentLoop - lastLoop;
				notifyWait = diff >= notifyWait ? 0 : notifyWait - diff;
			}
		}

		if (res == pdFALSE || notifyWait == 0 || (res == pdTRUE && (notifyVal & CONNECTION_CLOSE)))
		{
			// Close if ready, and re-evaluate the timers
			int nextCheck = MaxAckTime;

			for (int i = 0; i < MaxConnections; i++)
			{
				if (closePending[i])
				{
					closeTimer[i] -= lastClose ? currentLoop - lastClose : 0;

					struct pbuf* buf;
					err_t rc = netconn_recv_tcp_pbuf(closePending[i], &buf);

					if (closeTimer[i] <= 0 || rc != ERR_OK || buf->tot_len == 0)
					{
						netconn_close(closePending[i]);
						netconn_delete(closePending[i]);
						closeTimer[i] = MaxAckTime;
						closePending[i] = nullptr;
						closePendingCnt--;
					}
					else
					{
						if (closeTimer[i] < nextCheck)
						{
							nextCheck = closeTimer[i];
						}
					}
				}
			}

			if (closePendingCnt == 0)
			{
				nextCheck = portMAX_DELAY;
			}

			lastClose = currentLoop;
			notifyWait = nextCheck;
		}

		lastLoop = currentLoop;
	}
}

/*static*/ void Connection::ListenCallback(struct netconn *conn, enum netconn_evt evt, u16_t len)
{
	if ((conn->socket >= 0 && conn->socket < MaxConnections)
			 && closePending[conn->socket] == conn)
	{
		xTaskNotify(connectionTask, CONNECTION_CLOSE, eSetBits);
	}
	else
	{
		if (evt == NETCONN_EVT_RCVPLUS && len == 0)
		{
			if (conn->socket > 0 && acceptPendingCnt < MaxConnections)
			{
				for (int i = 0; i < MaxConnections; i++)
				{
					if (acceptPending[i] == nullptr)
					{
						acceptPendingCnt++;
						acceptPending[i] = conn;
						break;
					}
				}
				xTaskNotify(connectionTask, CONNECTION_ACCEPT, eSetBits);
			}
		}
	}
}

/*static*/ void Connection::ConnectCallback(struct netconn *conn, enum netconn_evt evt, u16_t len)
{
	if ((conn->socket >= 0 && conn->socket < MaxConnections)
			 && closePending[conn->socket] == conn)
	{
		xTaskNotify(connectionTask, CONNECTION_CLOSE, eSetBits);
	}
	else
	{
		if (conn->socket > 0)
		{
			Connection *_conn = reinterpret_cast<Connection*>(conn->socket);
			if (_conn->state == ConnState::connecting)
			{
				if (evt == NETCONN_EVT_SENDPLUS)
				{
					_conn->SetConnection(conn, Outgoing);
				}
				else if (evt == NETCONN_EVT_ERROR)
				{
					_conn->SetState(ConnState::otherEndClosed);
				}
			}
		}
	}
}

// Static data
TaskHandle_t Connection::connectionTask = nullptr;
int Connection::closePendingCnt = 0;
int Connection::acceptPendingCnt = 0;
int Connection::closeTimer[MaxConnections];
netconn * Connection::closePending[MaxConnections];
netconn * Connection::acceptPending[MaxConnections];
Connection *Connection::connectionList[MaxConnections];

// End
