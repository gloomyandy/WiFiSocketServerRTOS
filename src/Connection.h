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
	ConnState GetState() const { return state; }
	void GetStatus(ConnStatusResponse& resp) const;

	void Close();
	void Terminate(bool external);
	size_t Write(const uint8_t *data, size_t length, bool doPush, bool closeAfterSending);
	size_t CanWrite() const;
	size_t Read(uint8_t *data, size_t length);
	size_t CanRead() const;
	void Poll();

	// Callback functions
	int Accept(struct netconn *pcb, int dir);
	void ConnError(int err);
	int ConnRecv(pbuf *p, int err);
	int ConnSent(uint16_t len);

	// Static functions
	static void Init();
	static bool Connect(uint32_t remoteIp, uint16_t remotePort, uint16_t localPort);
	static Connection *Allocate();
	static Connection& Get(uint8_t num) { return *connectionList[num]; }
	static uint16_t CountConnectionsOnPort(uint16_t port);
	static void ReportConnections();
	static void GetSummarySocketStatus(uint16_t& connectedSockets, uint16_t& otherEndClosedSockets);
	static void TerminateAll();

private:
	void FreePbuf();
	void Report();

	void SetState(ConnState st)
	{
		state = st;
	}

	uint8_t number;
	uint8_t direction;
	volatile ConnState state;

	uint16_t localPort;
	uint16_t remotePort;

	uint32_t remoteIp;
	size_t readIndex;			// how much data we have already read from the current pbuf
	size_t alreadyRead;			// how much data we read from previous pbufs and didn't tell LWIP about yet
	struct netconn *ownPcb;		// the pcb that corresponds to this connection
	pbuf *pb;					// the buffers holding data we have received that has not yet been taken

	static QueueHandle_t closeQueue;
	static void connCloseTask(void* data);

	static void netconnCb(struct netconn *conn, enum netconn_evt evt, u16_t len);

	static Connection *connectionList[MaxConnections];
};

#endif /* SRC_CONNECTION_H_ */
