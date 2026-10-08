#include <nds.h>
#include <dswifi9.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "net.h"

static bool wifi_inited, wifi_ok;
const char *net_error = "";
u32 net_frames;

#define CONNECT_TIMEOUT (60 * 8)	// 8 s to connect to the server
#define RESPONSE_TIMEOUT (60 * 12)	// 12 s until the headers are received

// Network log at sd:/data/dsstore/log.txt, for diagnosing connections
void net_log(const char *fmt, ...) {
	FILE *f = fopen("/data/dsstore/log.txt", "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static int wifi_frames;

// dswifi internals used by Wifi_InitDefault (they are globals)
extern void wifiValue32Handler(u32 value, void *userdata);
extern void Timer_50ms(void);
extern void arm9_synctoarm7(void);

// Like Wifi_InitDefault(INIT_ONLY), but with a 512 KB sgIP heap instead
// of 64 KB: each TCP connection reserves ~16 KB and 64 KB only fits 3 or 4,
// so after a few requests no more could be opened.
static void wifi_init(void) {
	fifoSetValue32Handler(FIFO_DSWIFI, wifiValue32Handler, 0);
	u32 pass = Wifi_Init(WIFIINIT_OPTION_USELED | WIFIINIT_OPTION_USEHEAP_512);
	if (!pass) return;
	irqSet(IRQ_TIMER3, Timer_50ms);
	irqEnable(IRQ_TIMER3);
	Wifi_SetSyncHandler(arm9_synctoarm7);
	TIMER3_DATA = -6553;	// 50 ms with divider 256
	TIMER3_CR = TIMER_ENABLE | TIMER_IRQ_REQ | TIMER_DIV_256;
	fifoSendAddress(FIFO_DSWIFI, (void *)pass);
	while (!Wifi_CheckInit()) swiWaitForVBlank();
}

void net_wifi_start(void) {
	if (wifi_ok && Wifi_AssocStatus() == ASSOCSTATUS_ASSOCIATED) return;
	if (!wifi_inited) {
		wifi_init();
		wifi_inited = true;
	}
	wifi_ok = false;
	wifi_frames = 0;
	Wifi_AutoConnect();
}

WifiState net_wifi_poll(void) {
	if (wifi_ok) return WIFI_OK;
	int s = Wifi_AssocStatus();
	if (s == ASSOCSTATUS_ASSOCIATED) { wifi_ok = true; net_log_stats("wifi conectado"); return WIFI_OK; }
	// ~25 s at most, like the original shop
	if (s == ASSOCSTATUS_CANNOTCONNECT || ++wifi_frames > 60 * 25) return WIFI_FAILED;
	return WIFI_CONNECTING;
}

bool net_wifi_ready(void) { return wifi_ok; }

// In melonDS the Wi-Fi sometimes stops receiving and sending altogether (the
// dswifi counters stay still). Detected by comparing the
// counters with the last time they were checked.
bool net_wifi_stalled(void) {
	static u32 last_rx, last_tx;
	u32 rx = Wifi_GetStats(WSTAT_RXPACKETS), tx = Wifi_GetStats(WSTAT_TXPACKETS);
	bool stalled = rx == last_rx && tx == last_tx;
	last_rx = rx;
	last_tx = tx;
	return stalled;
}

void net_wifi_restart(void) {
	net_log_stats("reiniciando wifi");
	Wifi_DisconnectAP();
	wifi_ok = false;
	wifi_frames = 0;
	Wifi_AutoConnect();
}

static bool parse_url(const char *url, char *host, int hostlen, int *port, char *prefix, int prelen) {
	if (!strncasecmp(url, "http://", 7)) url += 7;
	else if (strstr(url, "://")) return false;	// https is not supported
	const char *slash = strchr(url, '/');
	const char *end = slash ? slash : url + strlen(url);
	const char *colon = memchr(url, ':', end - url);
	int hl = (colon ? colon : end) - url;
	if (hl <= 0 || hl >= hostlen) return false;
	memcpy(host, url, hl);
	host[hl] = 0;
	*port = colon ? atoi(colon + 1) : 80;
	snprintf(prefix, prelen, "%s", slash ? slash : "");
	int n = strlen(prefix);
	while (n > 0 && prefix[n - 1] == '/') prefix[--n] = 0;
	return *port > 0 && *port < 65536;
}

static void reset_response(Http *h) {
	h->status = 0;
	h->length = h->received = 0;
	h->has_length = h->keepalive = false;
	h->headers_done = h->eof = h->error = false;
	h->headlen = h->npending = 0;
	h->pending = NULL;
	h->sent = 0;
}

static bool open_socket(Http *h) {
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(h->port);
	memcpy(&sa.sin_addr, &h->ip, 4);
	h->sock = socket(AF_INET, SOCK_STREAM, 0);
	if (h->sock <= 0) { h->sock = 0; net_error = "socket"; return false; }
	// non-blocking connect: a down server does not freeze the console
	int one = 1;
	ioctl(h->sock, FIONBIO, &one);
	connect(h->sock, (struct sockaddr *)&sa, sizeof(sa));
	h->reused = false;
	return true;
}

void http_close(Http *h) {
	if (h->sock > 0) {
		shutdown(h->sock, 0);
		closesocket(h->sock);
	}
	h->sock = 0;
	h->idle = false;
}

// extra_headers: additional headers, each terminated by CRLF, or NULL
static bool http_get_ex(Http *h, const char *base, const char *path, const char *extra_headers) {
	char host[96], prefix[128];
	int port;
	if (!parse_url(base, host, sizeof(host), &port, prefix, sizeof(prefix))) { net_error = "URL"; return false; }

	bool same = h->sock > 0 && h->port == port && !strcmp(h->host, host);
	if (!same || !h->idle) {
		http_close(h);
		unsigned a, b, c, d;
		char extra;
		if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) == 4 && a < 256 && b < 256 && c < 256 && d < 256) {
			u8 ip[4] = { a, b, c, d };
			memcpy(&h->ip, ip, 4);
		} else {
			struct hostent *he = gethostbyname(host);
			if (!he || !he->h_addr_list[0]) { net_error = "DNS"; return false; }
			memcpy(&h->ip, he->h_addr_list[0], 4);
		}
		snprintf(h->host, sizeof(h->host), "%s", host);
		h->port = port;
		if (!open_socket(h)) return false;
	} else {
		h->reused = true;
	}
	h->idle = false;
	reset_response(h);
	h->reqlen = snprintf(h->req, sizeof(h->req),
		"GET %s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: DSShop-NDS/1.0\r\n%sConnection: keep-alive\r\n\r\n",
		prefix, path, host, extra_headers ? extra_headers : "");
	h->connecting = true;
	h->t0 = net_frames;
	net_log("GET %s%s -> %s:%d%s", prefix, path, host, port, h->reused ? " (reutilizada)" : "");
	return true;
}

bool http_get(Http *h, const char *base, const char *path) {
	return http_get_ex(h, base, path, NULL);
}

bool http_get_range(Http *h, const char *base, const char *path, u32 from, u32 to) {
	char range[48];
	snprintf(range, sizeof(range), "Range: bytes=%lu-%lu\r\n", from, to);
	return http_get_ex(h, base, path, range);
}

static void connect_error(Http *h) {
	static char err[64];
	struct in_addr gw, mask, d1, d2;
	struct in_addr me = Wifi_GetIPInfo(&gw, &mask, &d1, &d2);
	u8 *m = (u8 *)&me, *g = (u8 *)&gw;
	snprintf(err, sizeof(err), "connect; IP %u.%u.%u.%u, GW %u.%u.%u.%u", m[0], m[1], m[2], m[3], g[0], g[1], g[2], g[3]);
	net_error = err;
	net_log("%s", err);
	h->error = true;
}

// Waits for the connection to be established and sends the request. true = done.
static bool finish_connect(Http *h) {
	if (!h->connecting) return true;
	fd_set wf;
	FD_ZERO(&wf);
	FD_SET(h->sock, &wf);
	struct timeval tv = { 0, 0 };
	if (select(h->sock + 1, NULL, &wf, NULL, &tv) > 0 && FD_ISSET(h->sock, &wf)) {
		while (h->sent < h->reqlen) {
			int r = send(h->sock, h->req + h->sent, h->reqlen - h->sent, 0);
			if (r <= 0) break;
			h->sent += r;
		}
		if (h->sent >= h->reqlen) { h->connecting = false; return true; }
	}
	if (net_frames - h->t0 > CONNECT_TIMEOUT) connect_error(h);
	return false;
}

// h->head must end in 0 right after the blank line
static void parse_headers(Http *h) {
	const char *sp = strchr(h->head, ' ');
	h->status = sp ? atoi(sp + 1) : 0;
	h->keepalive = !strncmp(h->head, "HTTP/1.1", 8);
	for (const char *l = strstr(h->head, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
		if (!strncasecmp(l + 2, "Content-Length:", 15)) {
			h->length = strtoul(l + 17, NULL, 10);
			h->has_length = true;
		} else if (!strncasecmp(l + 2, "Connection:", 11)) {
			const char *e = strstr(l + 2, "\r\n");
			const char *v = l + 13;
			while (*v == ' ') v++;
			if (!strncasecmp(v, "close", 5) && (!e || v < e)) h->keepalive = false;
		} else if (!strncasecmp(l + 2, "Transfer-Encoding:", 18)) {
			h->keepalive = false;	// unused: without Content-Length it reads until close
		}
	}
	if (!h->has_length) h->keepalive = false;
}

// non-blocking recv: 1..n bytes, 0 = nothing yet, -1 = connection closed/error
static int recv_nb(int sock, void *buf, int max) {
	fd_set rf;
	FD_ZERO(&rf);
	FD_SET(sock, &rf);
	struct timeval tv = { 0, 0 };
	if (select(sock + 1, &rf, NULL, NULL, &tv) <= 0 || !FD_ISSET(sock, &rf)) return 0;
	int r = recv(sock, buf, max, 0);
	return r > 0 ? r : -1;
}

// A reused connection the server had already closed: the request is
// retried on a new connection.
static bool retry_fresh(Http *h) {
	if (!h->reused || h->headlen || h->headers_done) return false;
	net_log("la conexion reutilizada estaba cerrada; se abre otra");
	if (h->sock > 0) { shutdown(h->sock, 0); closesocket(h->sock); }
	h->sock = 0;
	if (!open_socket(h)) return false;
	reset_response(h);
	h->connecting = true;
	h->t0 = net_frames;
	return true;
}

static void response_done(Http *h) {
	h->eof = true;
	h->idle = h->keepalive;
	if (!h->idle) http_close(h);
}

int http_read(Http *h, u8 *buf, int max) {
	if (h->error) return -1;
	if (h->eof) return 0;
	if (h->sock <= 0) { h->error = true; return -1; }
	if (!finish_connect(h)) {
		if (h->error && retry_fresh(h)) { h->error = false; return 0; }
		return h->error ? -1 : 0;
	}
	// the server accepts the connection but does not answer
	if (!h->headers_done && net_frames - h->t0 > RESPONSE_TIMEOUT) {
		net_error = "timeout";
		net_log("sin respuesta del servidor");
		h->error = true;
		http_close(h);
		return -1;
	}
	while (!h->headers_done) {
		int r = recv_nb(h->sock, h->head + h->headlen, sizeof(h->head) - 1 - h->headlen);
		if (r == 0) return 0;
		if (r < 0) {
			if (retry_fresh(h)) return 0;
			net_error = "recv";
			h->error = true;
			http_close(h);
			return -1;
		}
		h->headlen += r;
		h->head[h->headlen] = 0;
		char *end = strstr(h->head, "\r\n\r\n");
		if (end) {
			h->headers_done = true;
			int hl = end + 4 - h->head;
			h->npending = h->headlen - hl;
			h->headlen = hl;
			char save = h->head[hl];
			h->head[hl] = 0;
			parse_headers(h);
			h->head[hl] = save;
			h->pending = (u8 *)h->head + hl;
			if (h->has_length && h->length == 0) { response_done(h); return 0; }
		} else if (h->headlen >= (int)sizeof(h->head) - 1) {
			net_error = "cabeceras";
			h->error = true;
			http_close(h);
			return -1;
		}
	}
	// never read past this response (the connection stays open for the next one)
	if (h->has_length && (u32)max > h->length - h->received) max = h->length - h->received;
	int n = 0;
	if (h->npending) {
		n = h->npending < max ? h->npending : max;
		memcpy(buf, h->pending, n);
		h->pending += n;
		h->npending -= n;
	} else if (max > 0) {
		int r = recv_nb(h->sock, buf, max);
		if (r < 0) {
			// the server closed: end of response if there was no Content-Length
			if (h->has_length && h->received < h->length) {
				net_error = "recv";
				h->error = true;
				http_close(h);
				return -1;
			}
			h->keepalive = false;
			response_done(h);
			return 0;
		}
		n = r;
	}
	h->received += n;
	if (h->has_length && h->received >= h->length) response_done(h);
	return n;
}

int http_fetch(Http *h, const char *base, const char *path, u8 *buf, int max, int timeout) {
	if (!http_get(h, base, path)) { net_log("fetch %s: %s", path, net_error); return -1; }
	int total = 0;
	for (int f = 0; f < timeout && !h->eof; f++) {
		int r = http_read(h, buf + total, max - total);
		if (r < 0) return -1;
		total += r;
		if (total >= max) break;
		if (r == 0) { swiWaitForVBlank(); net_tick(); }
	}
	if (!h->eof) http_close(h);	// partial response: the connection cannot be reused
	net_log("fetch %s: status %d, %d bytes, eof %d", path, h->status, total, h->eof);
	if (!h->eof || h->status != 200) return -1;
	return total;
}

void net_log_stats(const char *when) {
	u32 ip = Wifi_GetIP();
	u8 *b = (u8 *)&ip;
	net_log("[%s] ip %u.%u.%u.%u, data rx %lu tx %lu", when, b[0], b[1], b[2], b[3], Wifi_GetStats(WSTAT_RXDATABYTES), Wifi_GetStats(WSTAT_TXDATABYTES));
	net_log("[%s] frame %lu, asoc %d, rx %lu pkts (%lu perdidos), tx %lu pkts (%lu rechazados), rxq %lu, txq %lu",
		when, net_frames, Wifi_AssocStatus(), Wifi_GetStats(WSTAT_RXPACKETS), Wifi_GetStats(WSTAT_RXQUEUEDLOST),
		Wifi_GetStats(WSTAT_TXPACKETS), Wifi_GetStats(WSTAT_TXQUEUEDREJECTED),
		Wifi_GetStats(WSTAT_RXQUEUEDPACKETS), Wifi_GetStats(WSTAT_TXQUEUEDPACKETS));
}

// ---------------------------------------------------------------- non-blocking sockets
// (for the BitTorrent client: peer connections and UDP trackers)

bool net_resolve(const char *host, u32 *ip) {
	unsigned a, b, c, d;
	char extra;
	if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) == 4 && a < 256 && b < 256 && c < 256 && d < 256) {
		u8 v[4] = { a, b, c, d };
		memcpy(ip, v, 4);
		return true;
	}
	struct hostent *he = gethostbyname(host);
	if (!he || !he->h_addr_list[0]) return false;
	memcpy(ip, he->h_addr_list[0], 4);
	return true;
}

static void make_addr(struct sockaddr_in *sa, u32 ip, u16 port) {
	memset(sa, 0, sizeof(*sa));
	sa->sin_family = AF_INET;
	sa->sin_port = htons(port);
	memcpy(&sa->sin_addr, &ip, 4);
}

int net_tcp_open(u32 ip, u16 port) {
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s <= 0) return -1;
	int one = 1;
	ioctl(s, FIONBIO, &one);
	struct sockaddr_in sa;
	make_addr(&sa, ip, port);
	connect(s, (struct sockaddr *)&sa, sizeof(sa));
	return s;
}

bool net_writable(int s) {
	fd_set wf;
	FD_ZERO(&wf);
	FD_SET(s, &wf);
	struct timeval tv = { 0, 0 };
	return select(s + 1, NULL, &wf, NULL, &tv) > 0 && FD_ISSET(s, &wf);
}

int net_send(int s, const void *buf, int len) {
	int r = send(s, buf, len, 0);
	if (r >= 0) return r;
	return (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS) ? 0 : -1;
}

int net_recv(int s, void *buf, int max) {
	return recv_nb(s, buf, max);
}

void net_close(int s) {
	if (s > 0) {
		shutdown(s, 0);
		closesocket(s);
	}
}

int net_udp_open(void) {
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s <= 0) return -1;
	int one = 1;
	ioctl(s, FIONBIO, &one);
	return s;
}

bool net_udp_send(int s, u32 ip, u16 port, const void *buf, int len) {
	struct sockaddr_in sa;
	make_addr(&sa, ip, port);
	return sendto(s, buf, len, 0, (struct sockaddr *)&sa, sizeof(sa)) == len;
}

int net_udp_recv(int s, void *buf, int max) {
	fd_set rf;
	FD_ZERO(&rf);
	FD_SET(s, &rf);
	struct timeval tv = { 0, 0 };
	if (select(s + 1, &rf, NULL, NULL, &tv) <= 0 || !FD_ISSET(s, &rf)) return 0;
	struct sockaddr_in from;
	int fl = sizeof(from);
	int r = recvfrom(s, buf, max, 0, (struct sockaddr *)&from, &fl);
	return r > 0 ? r : 0;
}
