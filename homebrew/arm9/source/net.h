// Wi-Fi (dswifi, firmware connection settings) and a minimal HTTP/1.1
// client with persistent connections.
//
// dswifi reserves ~16 KB of its heap for each TCP connection and, when closed,
// takes a long time to free it; also, closing a connection while the server is still
// sending leaves it receiving data nobody reads and clogs the network. So
// each server uses a single connection (keep-alive) for all its requests
// and responses are always read in full.
#pragma once
#include <nds.h>

typedef enum { WIFI_CONNECTING, WIFI_OK, WIFI_FAILED } WifiState;

void net_wifi_start(void);	// starts the connection with the firmware WFC settings
WifiState net_wifi_poll(void);
bool net_wifi_ready(void);
bool net_wifi_stalled(void);	// nothing has come in or gone out since the last call
void net_wifi_restart(void);	// disconnects and reconnects (then net_wifi_poll)
extern const char *net_error;	// last phase that failed (to show in errors)
extern u32 net_frames;	// frame counter (incremented by main with net_tick)
static inline void net_tick(void) { net_frames++; }
void net_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void net_log_stats(const char *when);	// dswifi counters to the log

// A connection to a server and the current response. A zeroed Http is a
// closed connection.
typedef struct {
	int sock;	// 0 = not connected
	u32 ip;
	u16 port;
	char host[96];
	bool idle;	// open with no pending response: can be reused
	bool reused;	// the current request goes over a reused connection
	bool connecting;	// connecting or sending the request (non-blocking)
	u32 t0;	// net_frames when the request started
	char req[400];
	int reqlen, sent;
	// response
	int status;	// HTTP status code
	u32 length;	// Content-Length
	bool has_length, keepalive;
	u32 received;	// body bytes received
	bool headers_done, eof, error;
	char head[1024];
	int headlen;
	u8 *pending;	// body bytes that arrived along with the headers
	int npending;
} Http;

// Sends a GET to base_url + path (base_url = "http://host[:port][/prefix]"),
// reusing the connection if one is already open to that server.
bool http_get(Http *h, const char *base_url, const char *path);
// Same, requesting only bytes [from, to] (to inclusive)
bool http_get_range(Http *h, const char *base_url, const char *path, u32 from, u32 to);
// Reads whatever is available without blocking. Returns body bytes (0 = nothing yet), -1 = error.
// h->eof indicates the response is complete.
int http_read(Http *h, u8 *buf, int max);
// Closes the connection (required when abandoning a partial response).
void http_close(Http *h);
// Blocking request with a timeout, for small responses.
int http_fetch(Http *h, const char *base_url, const char *path, u8 *buf, int max, int timeout_frames);

// Non-blocking sockets for the BitTorrent client
bool net_resolve(const char *host, u32 *ip);	// IP in network byte order
int net_tcp_open(u32 ip, u16 port);	// non-blocking connect; -1 on failure
bool net_writable(int s);	// the connection is already established
int net_send(int s, const void *buf, int len);	// bytes sent, 0 = not now, -1 = error
int net_recv(int s, void *buf, int max);	// bytes received, 0 = nothing yet, -1 = closed
void net_close(int s);
int net_udp_open(void);
bool net_udp_send(int s, u32 ip, u16 port, const void *buf, int len);
int net_udp_recv(int s, void *buf, int max);	// 0 = nothing
