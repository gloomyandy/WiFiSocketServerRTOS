/*
 * Socket.h
 *
 *  Created on: 11 Apr 2017
 *      Author: David
 *
 * Simplified socket class to run in ESP8266 in Duet WiFi
 */

#ifndef SRC_CONNECTION_H_
#define SRC_CONNECTION_H_

#include <cstdint>
#include <cstddef>

#include "lwip/api.h"

#include "include/MessageFormats.h"			// for ConnState

class Connection
{
public:
	Connection(uint8_t num);

	// Public interface
	size_t Read(uint8_t *data, size_t length);
	size_t CanRead() const;
	size_t Write(const uint8_t *data, size_t length, bool doPush, bool closeAfterSending);
	size_t CanWrite() const;

	void Poll();
	void Close();
	void Terminate(bool external);
	void GetStatus(ConnStatusResponse& resp) const;

	// Static functions
	static void Init();
	static bool Connect(uint32_t remoteIp, uint16_t remotePort, uint16_t localPort);
	static bool Listen(uint16_t port, uint32_t ip, uint8_t protocol, uint16_t maxConns);
	static void Dismiss(uint16_t port);
	static void TerminateAll();

	static Connection& Get(uint8_t num) { return *connectionList[num]; }
	static uint16_t GetPortByProtocol(uint8_t protocol);
	static void GetSummarySocketStatus(uint16_t& connectedSockets, uint16_t& otherEndClosedSockets);
	static void ReportConnections();

private:
	void SetConnection(struct netconn *conn, bool direction);
	void SetState(ConnState st) { state = st; }
	ConnState GetState() const { return state; }

	void FreePbuf();
	void Report();

	static Connection *Allocate();
	static uint16_t CountConnectionsOnPort(uint16_t port);

	static void ConnectionTask(void* data);
	static void ListenCallback(struct netconn *conn, enum netconn_evt evt, u16_t len);
	static void ConnectCallback(struct netconn *conn, enum netconn_evt evt, u16_t len);

	uint8_t number;
	uint16_t localPort;
	uint16_t remotePort;
	uint32_t remoteIp;
	uint8_t direction;
	struct netconn *conn;		// the pcb that corresponds to this connection
	volatile ConnState state;

	pbuf *readBuf;				// the buffers holding data we have received that has not yet been taken
	size_t readIndex;			// how much data we have already read from the current pbuf
	size_t alreadyRead;			// how much data we read from previous pbufs and didn't tell LWIP about yet

	static QueueHandle_t connectionQueue;
	static int closePendingCnt;
	static int closeTimer[MaxConnections];
	static struct netconn *closePending[MaxConnections];
	static Connection *connectionList[MaxConnections];
};

#endif /* SRC_CONNECTION_H_ */
