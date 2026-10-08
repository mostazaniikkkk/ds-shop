#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bencode.h"
#include "net.h"
#include "sha1.h"
#include "torrent.h"

#define MAX_PEERS 6
#define MAX_KNOWN 96
#define PIPELINE 6
#define RXMAX (32 * 1024 + 64)
#define MAX_FILE_SIZE (512u * 1024 * 1024)	// the largest that makes sense on a DS
#define MAX_BOUNDARY_PIECE (2u * 1024 * 1024)	// pieces shared with other files: kept in RAM

#define SECS(n) ((n) * 60)

static const char *err_msg = "";
const char *tor_error(void) { return err_msg; }

static void hexstr(const u8 *b, int n, char *out) {
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < n; i++) { out[i * 2] = hx[b[i] >> 4]; out[i * 2 + 1] = hx[b[i] & 15]; }
	out[n * 2] = 0;
}

// big endian and without requiring alignment (the ARM9 does not allow unaligned 32-bit reads)
static void put32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static u32 get32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

// ------------------------------------------------------------------ .torrent

void tor_free(TorMeta *m) {
	free(m->hashes);
	m->hashes = NULL;
}

static void add_tracker(TorMeta *m, BVal v) {
	char url[128];
	if (!bv_str_copy(v, url, sizeof(url)) || m->ntrackers >= TOR_MAX_TRACKERS) return;
	if (strncmp(url, "http://", 7) && strncmp(url, "udp://", 6)) return;	// https is not possible
	for (int i = 0; i < m->ntrackers; i++)
		if (!strcmp(m->trackers[i], url)) return;
	strcpy(m->trackers[m->ntrackers++], url);
}

const char *tor_load(const u8 *buf, int len, TorMeta *m) {
	memset(m, 0, sizeof(*m));
	BVal root, info, v;
	if (!bv_parse(buf, len, &root) || *root.p != 'd') return "el archivo no es un .torrent";
	if (!bv_dict_get(root, "info", &info) || *info.p != 'd') return "el .torrent no tiene la seccion info";
	sha1(info.p, info.end - info.p, m->info_hash);

	if (bv_dict_get(info, "name.utf-8", &v) || bv_dict_get(info, "name", &v)) bv_str_copy(v, m->name, sizeof(m->name));
	s64 n;
	if (!bv_dict_get(info, "piece length", &v) || !bv_int(v, &n) || n <= 0 || n > 64 * 1024 * 1024) return "tamano de pieza no valido";
	m->piece_len = n;
	const u8 *ph;
	int phl;
	if (!bv_dict_get(info, "pieces", &v) || !bv_str(v, &ph, &phl) || phl % 20) return "lista de piezas no valida";
	m->npieces = phl / 20;

	if (bv_dict_get(info, "length", &v)) {	// single file
		if (!bv_int(v, &n) || n <= 0) return "tamano no valido";
		snprintf(m->files[0].path, sizeof(m->files[0].path), "%s", m->name);
		m->files[0].size = n;
		m->nfiles = 1;
		m->total = n;
	} else if (bv_dict_get(info, "files", &v)) {
		const u8 *it = NULL;
		BVal f;
		u64 off = 0;
		while (bv_list_next(v, &it, &f)) {
			BVal fl, fp;
			if (!bv_dict_get(f, "length", &fl) || !bv_int(fl, &n) || n < 0) return "lista de archivos no valida";
			if (m->nfiles < TOR_MAX_FILES) {
				TorFile *tf = &m->files[m->nfiles++];
				tf->offset = off;
				tf->size = n;
				tf->path[0] = 0;
				if (bv_dict_get(f, "path.utf-8", &fp) || bv_dict_get(f, "path", &fp)) {
					const u8 *pit = NULL;
					BVal part;
					while (bv_list_next(fp, &pit, &part)) {
						char seg[96];
						bv_str_copy(part, seg, sizeof(seg));
						int l = strlen(tf->path);
						snprintf(tf->path + l, sizeof(tf->path) - l, "%s%s", l ? "/" : "", seg);
					}
				}
			}
			off += n;
		}
		m->total = off;
	} else {
		return "el .torrent no indica los archivos";
	}
	if ((m->total + m->piece_len - 1) / m->piece_len != m->npieces) return "el numero de piezas no cuadra";

	if (bv_dict_get(root, "announce", &v)) add_tracker(m, v);
	if (bv_dict_get(root, "announce-list", &v)) {
		const u8 *it = NULL;
		BVal tier;
		while (bv_list_next(v, &it, &tier)) {
			const u8 *it2 = NULL;
			BVal u;
			while (bv_list_next(tier, &it2, &u)) add_tracker(m, u);
		}
	}
	m->hashes = malloc(phl);
	if (!m->hashes) return "no hay memoria para el torrent";
	memcpy(m->hashes, ph, phl);
	return NULL;
}

// ------------------------------------------------------------------ download state

typedef struct {
	u32 ip;
	u16 port;
	u8 fails, inuse;
	u32 retry_at;
} Known;

enum { P_FREE, P_CONNECT, P_HANDSHAKE, P_ACTIVE };

typedef struct {
	u32 piece, begin, len, t;
} Req;

typedef struct {
	int sock, known;
	u8 state;
	bool choking, interested;
	u32 t_state, t_rx;
	u8 *bits;
	u8 *rx;
	int rxlen;
	Req req[PIPELINE];
	int nreq;
	u8 out[512];
	int outlen;
} Peer;

static struct {
	bool active;
	TorMeta *m;
	int file;
	u64 fs, fe;	// range of the file within the torrent
	u32 fp, lp;	// needed pieces
	u32 blk, bpp;	// block size and blocks per piece
	u8 *pstate;	// 1 = piece verified
	u8 *bstate;	// 0 missing, 1 requested, 2 received
	u8 *bowner;	// peer that requested it
	u32 *btime;
	u8 *bound[2];	// pieces sharing bytes with other files (first and last)
	u32 bound_piece[2];
	FILE *f;
	char dest[256], part[260], info[264], info_line[160];
	u64 prealloc;	// bytes already allocated (0 = not needed)
	bool preallocating;
	u64 recv_bytes;	// file bytes already received
	u32 pieces_left;
	Known known[MAX_KNOWN];
	int nknown;
	Peer peers[MAX_PEERS];
	u8 peer_id[20];
	u32 now;
	// trackers
	int tr_index;
	u32 tr_next;	// when to ask again
	int tr_state;	// 0 stopped, 1 http, 2 udp connect, 3 udp announce
	Http tr_http;
	u8 *tr_buf;
	int tr_len;
	int udp_sock;
	u32 udp_ip, udp_tid, udp_t;
	u16 udp_port;
	u8 udp_conn[8];
	bool started_event;
	// statistics
	u32 rate_bytes, rate, rate_t;
	int since_save;
	const char *status;
} T;

static u32 piece_size(u32 p) {
	u64 end = (u64)(p + 1) * T.m->piece_len;
	if (end > T.m->total) end = T.m->total;
	return end - (u64)p * T.m->piece_len;
}

static u32 nblocks(u32 p) { return (piece_size(p) + T.blk - 1) / T.blk; }
static u32 bidx(u32 p, u32 b) { return (p - T.fp) * T.bpp + b; }
static int bound_slot(u32 p) {
	for (int i = 0; i < 2; i++)
		if (T.bound[i] && T.bound_piece[i] == p) return i;
	return -1;
}

static void save_info(void) {
	FILE *f = fopen(T.info, "w");
	if (!f) return;
	fprintf(f, "%s\n", T.info_line);
	for (u32 p = T.fp; p <= T.lp; p++) fputc(T.pstate[p - T.fp] ? '1' : '0', f);
	fputc('\n', f);
	fclose(f);
	T.since_save = 0;
}

static bool load_info(void) {
	FILE *f = fopen(T.info, "r");
	if (!f) return false;
	static char line[160];
	int il = strlen(T.info_line);
	bool ok = fgets(line, sizeof(line), f) && !strncmp(line, T.info_line, il) && (line[il] == '\n' || line[il] == '\r');
	u32 n = T.lp - T.fp + 1;
	for (u32 i = 0; ok && i < n; i++) {
		int c = fgetc(f);
		if (c != '0' && c != '1') { ok = false; break; }
		T.pstate[i] = c == '1';
	}
	fclose(f);
	return ok;
}

// ------------------------------------------------------------------ start and end

bool tor_start(TorMeta *m, int file, const char *dest) {
	memset(&T, 0, sizeof(T));
	err_msg = "";
	T.m = m;
	T.file = file;
	T.udp_sock = -1;
	TorFile *tf = &m->files[file];
	if (tf->size == 0) { err_msg = "el archivo esta vacio"; return false; }
	if (tf->size > MAX_FILE_SIZE) { err_msg = "el archivo es demasiado grande para la DS"; return false; }
	if (!m->ntrackers) { err_msg = "el torrent no tiene trackers http:// o udp:// (la DS no admite DHT ni HTTPS)"; return false; }
	T.fs = tf->offset;
	T.fe = tf->offset + tf->size;
	T.fp = T.fs / m->piece_len;
	T.lp = (T.fe - 1) / m->piece_len;
	T.blk = m->piece_len < 16384 ? m->piece_len : 16384;
	if (m->piece_len % T.blk) { err_msg = "tamano de pieza no admitido"; return false; }
	T.bpp = m->piece_len / T.blk;
	u32 np = T.lp - T.fp + 1, nb = np * T.bpp;
	T.pstate = calloc(np, 1);
	T.bstate = calloc(nb, 1);
	T.bowner = calloc(nb, 1);
	T.btime = calloc(nb, sizeof(u32));
	if (!T.pstate || !T.bstate || !T.bowner || !T.btime) { err_msg = "no hay memoria suficiente"; tor_stop(false); return false; }
	// pieces sharing bytes with other files
	u32 cand[2] = { T.fp, T.lp };
	for (int i = 0; i < 2; i++) {
		u32 p = cand[i];
		u64 ps = (u64)p * m->piece_len, pe = ps + piece_size(p);
		if (ps >= T.fs && pe <= T.fe) continue;
		if (i == 1 && cand[1] == cand[0] && T.bound[0]) continue;
		if (piece_size(p) > MAX_BOUNDARY_PIECE) { err_msg = "las piezas de este torrent son demasiado grandes"; tor_stop(false); return false; }
		T.bound[i] = malloc(piece_size(p));
		T.bound_piece[i] = p;
		if (!T.bound[i]) { err_msg = "no hay memoria suficiente"; tor_stop(false); return false; }
	}

	snprintf(T.dest, sizeof(T.dest), "%s", dest);
	snprintf(T.part, sizeof(T.part), "%s.part", dest);
	snprintf(T.info, sizeof(T.info), "%s.part.info", dest);
	char hx[41];
	hexstr(m->info_hash, 20, hx);
	snprintf(T.info_line, sizeof(T.info_line), "torrent|%s|%d|%llu", hx, file, (unsigned long long)tf->size);

	struct stat st;
	bool resume = !stat(T.part, &st) && (u64)st.st_size == tf->size && load_info();
	if (resume) {
		T.f = fopen(T.part, "r+b");
	} else {
		remove(T.info);
		T.f = fopen(T.part, "wb");
		T.preallocating = true;
	}
	if (!T.f) { err_msg = "no se ha podido crear el archivo en la SD"; tor_stop(false); return false; }
	setvbuf(T.f, NULL, _IONBF, 0);

	T.pieces_left = 0;
	for (u32 p = T.fp; p <= T.lp; p++) {
		if (T.pstate[p - T.fp]) {
			u64 ps = (u64)p * m->piece_len, pe = ps + piece_size(p);
			if (ps < T.fs) ps = T.fs;
			if (pe > T.fe) pe = T.fe;
			T.recv_bytes += pe - ps;
			for (u32 b = 0; b < nblocks(p); b++) T.bstate[bidx(p, b)] = 2;
		} else {
			T.pieces_left++;
		}
	}
	memcpy(T.peer_id, "-DS0100-", 8);
	for (int i = 8; i < 20; i++) T.peer_id[i] = "0123456789abcdefghijklmnopqrstuvwxyz"[rand() % 36];
	T.tr_buf = malloc(64 * 1024);
	if (!T.tr_buf) { err_msg = "no hay memoria suficiente"; tor_stop(false); return false; }
	T.active = true;
	T.status = "";
	net_log("torrent %s: archivo %d (%llu bytes), piezas %lu-%lu de %lu KB, %s",
		hx, file, (unsigned long long)tf->size, T.fp, T.lp, m->piece_len / 1024, resume ? "continuando" : "nuevo");
	return true;
}

static void drop_peer(Peer *pe, bool failed);

void tor_stop(bool keep_partial) {
	for (int i = 0; i < MAX_PEERS; i++)
		if (T.peers[i].state != P_FREE) drop_peer(&T.peers[i], false);
	if (T.tr_state == 1) http_close(&T.tr_http);
	if (T.udp_sock > 0) net_close(T.udp_sock);
	T.udp_sock = -1;
	if (T.f) {
		fclose(T.f);
		T.f = NULL;
		if (keep_partial && !T.preallocating) save_info();
		else { remove(T.part); remove(T.info); }
	}
	for (int i = 0; i < 2; i++) { free(T.bound[i]); T.bound[i] = NULL; }
	free(T.pstate); free(T.bstate); free(T.bowner); free(T.btime); free(T.tr_buf);
	T.pstate = T.bstate = T.bowner = NULL;
	T.btime = NULL;
	T.tr_buf = NULL;
	T.active = false;
}

// ------------------------------------------------------------------ trackers

static void add_known(u32 ip, u16 port) {
	if (!port || !ip) return;
	for (int i = 0; i < T.nknown; i++)
		if (T.known[i].ip == ip && T.known[i].port == port) return;
	if (T.nknown >= MAX_KNOWN) return;
	Known *k = &T.known[T.nknown++];
	memset(k, 0, sizeof(*k));
	k->ip = ip;
	k->port = port;
}

static void add_compact(const u8 *p, int n) {
	for (int i = 0; i + 6 <= n; i += 6) {
		u32 ip;
		memcpy(&ip, p + i, 4);
		add_known(ip, (p[i + 4] << 8) | p[i + 5]);
	}
}

static u64 left_bytes(void) { return (T.fe - T.fs) - T.recv_bytes; }

static void url_escape(const u8 *b, int n, char *out) {
	static const char hx[] = "0123456789ABCDEF";
	for (int i = 0; i < n; i++) { *out++ = '%'; *out++ = hx[b[i] >> 4]; *out++ = hx[b[i] & 15]; }
	*out = 0;
}

static bool split_host(const char *url, int skip, char *host, int hl, u16 *port, const char **rest) {
	const char *h = url + skip;
	const char *end = h + strcspn(h, ":/");
	int n = end - h;
	if (n <= 0 || n >= hl) return false;
	memcpy(host, h, n);
	host[n] = 0;
	*port = *end == ':' ? atoi(end + 1) : 80;
	*rest = end + strcspn(end, "/");
	return *port > 0;
}

static void tracker_start(void) {
	const char *url = T.m->trackers[T.tr_index % T.m->ntrackers];
	T.tr_len = 0;
	if (!strncmp(url, "http://", 7)) {
		static char path[400];
		char ih[61], pid[61];
		url_escape(T.m->info_hash, 20, ih);
		url_escape(T.peer_id, 20, pid);
		snprintf(path, sizeof(path), "%sinfo_hash=%s&peer_id=%s&port=6881&uploaded=0&downloaded=%llu&left=%llu&compact=1&numwant=60%s",
			strchr(url, '?') ? "&" : "?", ih, pid, (unsigned long long)T.recv_bytes, (unsigned long long)left_bytes(),
			T.started_event ? "" : "&event=started");
		memset(&T.tr_http, 0, sizeof(T.tr_http));
		if (http_get(&T.tr_http, url, path)) { T.tr_state = 1; T.status = "tracker"; return; }
	} else {
		char host[96];
		const char *rest;
		if (split_host(url, 6, host, sizeof(host), &T.udp_port, &rest) && net_resolve(host, &T.udp_ip)) {
			if (T.udp_sock <= 0) T.udp_sock = net_udp_open();
			if (T.udp_sock > 0) {
				u8 pkt[16];
				T.udp_tid = rand();
				put32(pkt, 0x417);	// protocol_id 0x41727101980
				put32(pkt + 4, 0x27101980);
				put32(pkt + 8, 0);	// connect
				put32(pkt + 12, T.udp_tid);
				if (net_udp_send(T.udp_sock, T.udp_ip, T.udp_port, pkt, 16)) {
					T.tr_state = 2;
					T.udp_t = T.now;
					T.status = "tracker";
					return;
				}
			}
		}
	}
	net_log("tracker %s: no se ha podido contactar", url);
	T.tr_index++;
	T.tr_next = T.now + SECS(5);
	T.tr_state = 0;
}

static void tracker_done(bool ok, u32 interval) {
	if (ok) T.started_event = true;
	if (!ok) T.tr_index++;
	if (interval < 60) interval = 60;
	if (interval > 1800) interval = 1800;
	// with few peers, ask again sooner
	T.tr_next = T.now + SECS(ok ? (T.nknown < 4 ? 30 : interval) : 5);
	T.tr_state = 0;
}

static void tracker_step(void) {
	if (T.tr_state == 0) {
		if (T.now >= T.tr_next) tracker_start();
		return;
	}
	if (T.tr_state == 1) {
		int r = http_read(&T.tr_http, T.tr_buf + T.tr_len, 64 * 1024 - T.tr_len);
		if (r > 0) T.tr_len += r;
		if (r < 0) { http_close(&T.tr_http); tracker_done(false, 0); return; }
		if (!T.tr_http.eof && T.tr_len < 64 * 1024) return;
		http_close(&T.tr_http);
		BVal root, v;
		u32 interval = 300;
		bool ok = T.tr_http.status == 200 && bv_parse(T.tr_buf, T.tr_len, &root) && *root.p == 'd';
		if (ok && bv_dict_get(root, "failure reason", &v)) {
			char why[96];
			bv_str_copy(v, why, sizeof(why));
			net_log("tracker: %s", why);
			ok = false;
		}
		if (ok) {
			s64 iv;
			if (bv_dict_get(root, "interval", &v) && bv_int(v, &iv)) interval = iv;
			if (bv_dict_get(root, "peers", &v)) {
				const u8 *p;
				int n;
				if (*v.p != 'l' && bv_str(v, &p, &n)) {
					add_compact(p, n);
				} else {
					const u8 *it = NULL;
					BVal d, ipv, portv;
					while (bv_list_next(v, &it, &d)) {
						char ips[48];
						s64 port;
						u32 ip;
						if (bv_dict_get(d, "ip", &ipv) && bv_str_copy(ipv, ips, sizeof(ips)) &&
							bv_dict_get(d, "port", &portv) && bv_int(portv, &port) && net_resolve(ips, &ip))
							add_known(ip, port);
					}
				}
			}
		}
		net_log("tracker %s: %s, %d pares conocidos", T.m->trackers[T.tr_index % T.m->ntrackers], ok ? "ok" : "error", T.nknown);
		tracker_done(ok, interval);
		return;
	}
	// UDP (BEP 15)
	u8 buf[1500];
	int n = net_udp_recv(T.udp_sock, buf, sizeof(buf));
	if (n == 0) {
		if (T.now - T.udp_t > SECS(15)) {
			net_log("tracker %s: sin respuesta", T.m->trackers[T.tr_index % T.m->ntrackers]);
			tracker_done(false, 0);
		}
		return;
	}
	if (n < 8 || get32(buf + 4) != T.udp_tid) return;
	u32 action = get32(buf);
	if (T.tr_state == 2 && action == 0 && n >= 16) {
		memcpy(T.udp_conn, buf + 8, 8);
		u8 pkt[98];
		memset(pkt, 0, sizeof(pkt));
		memcpy(pkt, T.udp_conn, 8);
		put32(pkt + 8, 1);
		T.udp_tid = rand();
		put32(pkt + 12, T.udp_tid);
		memcpy(pkt + 16, T.m->info_hash, 20);
		memcpy(pkt + 36, T.peer_id, 20);
		u64 dl = T.recv_bytes, left = left_bytes();
		for (int i = 0; i < 8; i++) { pkt[56 + i] = dl >> (56 - i * 8); pkt[64 + i] = left >> (56 - i * 8); }
		put32(pkt + 80, T.started_event ? 0 : 2);	// started
		put32(pkt + 88, rand());	// key
		put32(pkt + 92, 0xFFFFFFFF);	// num_want -1
		pkt[96] = 6881 >> 8;
		pkt[97] = 6881 & 0xFF;
		net_udp_send(T.udp_sock, T.udp_ip, T.udp_port, pkt, sizeof(pkt));
		T.tr_state = 3;
		T.udp_t = T.now;
	} else if (T.tr_state == 3 && action == 1 && n >= 20) {
		u32 interval = get32(buf + 8);
		add_compact(buf + 20, n - 20);
		net_log("tracker udp: ok, %d pares conocidos", T.nknown);
		tracker_done(true, interval);
	} else if (action == 3) {
		net_log("tracker udp: error");
		tracker_done(false, 0);
	}
}

// ------------------------------------------------------------------ peers

static void release_requests(Peer *pe) {
	int me = pe - T.peers;
	for (int i = 0; i < pe->nreq; i++) {
		Req *r = &pe->req[i];
		u32 b = bidx(r->piece, r->begin / T.blk);
		if (T.bstate[b] == 1 && T.bowner[b] == me) T.bstate[b] = 0;
	}
	pe->nreq = 0;
}

static void drop_peer(Peer *pe, bool failed) {
	release_requests(pe);
	net_close(pe->sock);
	if (pe->known >= 0) {
		Known *k = &T.known[pe->known];
		k->inuse = 0;
		if (failed) k->fails++;
		k->retry_at = T.now + SECS(failed ? 30 : 10);
	}
	free(pe->bits);
	free(pe->rx);
	memset(pe, 0, sizeof(*pe));
	pe->state = P_FREE;
	pe->known = -1;
}

static void queue(Peer *pe, const void *data, int n) {
	if (pe->outlen + n > (int)sizeof(pe->out)) return;
	memcpy(pe->out + pe->outlen, data, n);
	pe->outlen += n;
}

static void connect_peers(void) {
	for (int i = 0; i < MAX_PEERS; i++) {
		Peer *pe = &T.peers[i];
		if (pe->state != P_FREE) continue;
		// the known peer with the fewest failures that is not in use
		int best = -1;
		for (int k = 0; k < T.nknown; k++) {
			Known *kn = &T.known[k];
			if (kn->inuse || kn->fails >= 3 || kn->retry_at > T.now) continue;
			if (best < 0 || kn->fails < T.known[best].fails) best = k;
		}
		if (best < 0) return;
		int s = net_tcp_open(T.known[best].ip, T.known[best].port);
		if (s < 0) return;
		memset(pe, 0, sizeof(*pe));
		pe->sock = s;
		pe->known = best;
		pe->state = P_CONNECT;
		pe->t_state = pe->t_rx = T.now;
		pe->choking = true;
		pe->bits = calloc((T.m->npieces + 7) / 8, 1);
		pe->rx = malloc(RXMAX);
		T.known[best].inuse = 1;
		if (!pe->bits || !pe->rx) { drop_peer(pe, false); return; }
	}
}

static bool peer_has(Peer *pe, u32 p) { return pe->bits[p >> 3] & (0x80 >> (p & 7)); }

static void verify_piece(u32 p);

// Writes a received block (to the file or to a shared piece's buffer)
static void got_block(Peer *pe, u32 p, u32 begin, const u8 *data, u32 len) {
	if (p < T.fp || p > T.lp || begin % T.blk || T.pstate[p - T.fp]) return;
	u32 b = begin / T.blk;
	if (b >= nblocks(p)) return;
	u32 expect = piece_size(p) - begin < T.blk ? piece_size(p) - begin : T.blk;
	if (len != expect) return;
	u32 bi = bidx(p, b);
	for (int i = 0; i < pe->nreq; i++) {
		if (pe->req[i].piece == p && pe->req[i].begin == begin) {
			pe->req[i] = pe->req[--pe->nreq];
			break;
		}
	}
	if (T.bstate[bi] == 2) return;	// duplicate (endgame mode)
	u64 o = (u64)p * T.m->piece_len + begin;
	int slot = bound_slot(p);
	if (slot >= 0) {
		memcpy(T.bound[slot] + begin, data, len);
	} else {
		fseek(T.f, o - T.fs, SEEK_SET);
		if (fwrite(data, 1, len, T.f) != len) { err_msg = "error escribiendo en la SD"; return; }
	}
	u64 s = o < T.fs ? T.fs : o, e = o + len > T.fe ? T.fe : o + len;
	if (e > s) T.recv_bytes += e - s;
	T.bstate[bi] = 2;
	T.rate_bytes += len;
	for (u32 k = 0; k < nblocks(p); k++)
		if (T.bstate[bidx(p, k)] != 2) return;
	verify_piece(p);
}

static void reset_piece(u32 p) {
	u64 ps = (u64)p * T.m->piece_len, pe = ps + piece_size(p);
	if (ps < T.fs) ps = T.fs;
	if (pe > T.fe) pe = T.fe;
	T.recv_bytes -= pe - ps;
	for (u32 k = 0; k < nblocks(p); k++) T.bstate[bidx(p, k)] = 0;
}

static void verify_piece(u32 p) {
	u8 hash[20];
	u32 size = piece_size(p);
	int slot = bound_slot(p);
	if (slot >= 0) {
		sha1(T.bound[slot], size, hash);
	} else {
		static u8 tmp[16384];
		Sha1 s;
		sha1_init(&s);
		fflush(T.f);
		fseek(T.f, (u64)p * T.m->piece_len - T.fs, SEEK_SET);
		for (u32 done = 0; done < size;) {
			u32 n = size - done < sizeof(tmp) ? size - done : sizeof(tmp);
			if (fread(tmp, 1, n, T.f) != n) { reset_piece(p); return; }
			sha1_update(&s, tmp, n);
			done += n;
		}
		sha1_final(&s, hash);
	}
	if (memcmp(hash, T.m->hashes + p * 20, 20)) {
		net_log("pieza %lu: SHA-1 incorrecto, se vuelve a pedir", p);
		reset_piece(p);
		return;
	}
	if (slot >= 0) {	// only the part that belongs to this file
		u64 ps = (u64)p * T.m->piece_len, s = ps < T.fs ? T.fs : ps, e = ps + size > T.fe ? T.fe : ps + size;
		fseek(T.f, s - T.fs, SEEK_SET);
		fwrite(T.bound[slot] + (s - ps), 1, e - s, T.f);
	}
	T.pstate[p - T.fp] = 1;
	T.pieces_left--;
	if (++T.since_save >= 8) {
		fflush(T.f);
		fsync(fileno(T.f));
		save_info();
	}
}

// Picks the next block for a peer (in order; at the end, re-requests the slow ones)
static bool pick_block(Peer *pe, u32 *op, u32 *ob) {
	int me = pe - T.peers;
	for (int endgame = 0; endgame < 2; endgame++) {
		for (u32 p = T.fp; p <= T.lp; p++) {
			if (T.pstate[p - T.fp] || !peer_has(pe, p)) continue;
			for (u32 b = 0; b < nblocks(p); b++) {
				u32 bi = bidx(p, b);
				if (T.bstate[bi] == 0 || (endgame && T.bstate[bi] == 1 && T.bowner[bi] != me && T.now - T.btime[bi] > SECS(15))) {
					*op = p;
					*ob = b;
					return true;
				}
			}
		}
	}
	return false;
}

static void request_blocks(Peer *pe) {
	if (pe->choking) return;
	while (pe->nreq < PIPELINE && pe->outlen + 17 <= (int)sizeof(pe->out)) {
		u32 p, b;
		if (!pick_block(pe, &p, &b)) return;
		u32 begin = b * T.blk, len = piece_size(p) - begin < T.blk ? piece_size(p) - begin : T.blk;
		u8 msg[17];
		put32(msg, 13);
		msg[4] = 6;
		put32(msg + 5, p);
		put32(msg + 9, begin);
		put32(msg + 13, len);
		queue(pe, msg, 17);
		u32 bi = bidx(p, b);
		T.bstate[bi] = 1;
		T.bowner[bi] = pe - T.peers;
		T.btime[bi] = T.now;
		pe->req[pe->nreq++] = (Req){ p, begin, len, T.now };
	}
}

static bool handle_message(Peer *pe, const u8 *m, u32 len) {
	if (len == 0) return true;	// keep-alive
	switch (m[0]) {
	case 0: pe->choking = true; release_requests(pe); break;
	case 1: pe->choking = false; break;
	case 4:
		if (len >= 5) {
			u32 p = get32(m + 1);
			if (p < T.m->npieces) pe->bits[p >> 3] |= 0x80 >> (p & 7);
		}
		break;
	case 5: {
		u32 n = len - 1, max = (T.m->npieces + 7) / 8;
		memcpy(pe->bits, m + 1, n < max ? n : max);
		break;
	}
	case 7:
		if (len >= 9) got_block(pe, get32(m + 1), get32(m + 5), m + 9, len - 9);
		break;
	}
	return true;
}

static void peer_step(Peer *pe) {
	if (pe->state == P_CONNECT) {
		if (net_writable(pe->sock)) {
			u8 hs[68];
			hs[0] = 19;
			memcpy(hs + 1, "BitTorrent protocol", 19);
			memset(hs + 20, 0, 8);
			memcpy(hs + 28, T.m->info_hash, 20);
			memcpy(hs + 48, T.peer_id, 20);
			queue(pe, hs, 68);
			u8 interested[5] = { 0, 0, 0, 1, 2 };
			queue(pe, interested, 5);
			pe->state = P_HANDSHAKE;
			pe->t_state = pe->t_rx = T.now;
		} else if (T.now - pe->t_state > SECS(8)) {
			drop_peer(pe, true);
			return;
		}
	}
	if (pe->state == P_FREE) return;
	// send what is pending
	if (pe->outlen && pe->state != P_CONNECT) {
		int r = net_send(pe->sock, pe->out, pe->outlen);
		if (r < 0) { drop_peer(pe, true); return; }
		if (r > 0) { memmove(pe->out, pe->out + r, pe->outlen - r); pe->outlen -= r; }
	}
	if (pe->state == P_CONNECT) return;
	// receive
	for (int guard = 0; guard < 8 && pe->rxlen < RXMAX; guard++) {
		int r = net_recv(pe->sock, pe->rx + pe->rxlen, RXMAX - pe->rxlen);
		if (r < 0) { drop_peer(pe, pe->state == P_HANDSHAKE); return; }
		if (r == 0) break;
		pe->rxlen += r;
		pe->t_rx = T.now;
	}
	int pos = 0;
	if (pe->state == P_HANDSHAKE) {
		if (pe->rxlen < 68) {
			if (T.now - pe->t_state > SECS(10)) drop_peer(pe, true);
			return;
		}
		if (pe->rx[0] != 19 || memcmp(pe->rx + 1, "BitTorrent protocol", 19) || memcmp(pe->rx + 28, T.m->info_hash, 20)) {
			drop_peer(pe, true);
			return;
		}
		pe->state = P_ACTIVE;
		T.known[pe->known].fails = 0;
		pos = 68;
	}
	while (pe->rxlen - pos >= 4) {
		u32 len = get32(pe->rx + pos);
		if (len > RXMAX - 4) { drop_peer(pe, true); return; }
		if ((u32)(pe->rxlen - pos - 4) < len) break;
		handle_message(pe, pe->rx + pos + 4, len);
		pos += 4 + len;
	}
	if (pos) {
		memmove(pe->rx, pe->rx + pos, pe->rxlen - pos);
		pe->rxlen -= pos;
	}
	request_blocks(pe);
	// no news from the peer: leave it for another
	u32 quiet = T.now - pe->t_rx;
	if ((pe->nreq && quiet > SECS(30)) || quiet > SECS(120)) drop_peer(pe, false);
}

// ------------------------------------------------------------------ loop

static int progress(void) {
	u64 size = T.fe - T.fs;
	int p = size ? (int)(T.recv_bytes * 1000 / size) : 0;
	return p > 999 ? 999 : p;
}

int tor_step(void) {
	if (!T.active) return -1;
	T.now = net_frames;
	if (T.preallocating) {
		// allocates the file on the SD bit by bit (so it can later be written in any order)
		static u8 zero[32 * 1024];
		u64 size = T.fe - T.fs;
		for (int i = 0; i < 4 && T.prealloc < size; i++) {
			u32 n = size - T.prealloc < sizeof(zero) ? size - T.prealloc : sizeof(zero);
			if (fwrite(zero, 1, n, T.f) != n) { err_msg = "no hay espacio en la SD"; tor_stop(false); return -1; }
			T.prealloc += n;
		}
		T.status = "prepare";
		if (T.prealloc < size) return 0;
		fclose(T.f);
		T.f = fopen(T.part, "r+b");
		if (!T.f) { err_msg = "no se ha podido abrir el archivo"; tor_stop(false); return -1; }
		setvbuf(T.f, NULL, _IONBF, 0);
		T.preallocating = false;
		save_info();
	}
	if (!T.pieces_left) {
		fclose(T.f);
		T.f = NULL;
		remove(T.dest);
		remove(T.info);
		if (rename(T.part, T.dest)) { err_msg = "no se ha podido renombrar el archivo"; tor_stop(false); return -1; }
		net_log("torrent completo: %s", T.dest);
		tor_stop(false);
		return 1000;
	}
	tracker_step();
	connect_peers();
	for (int i = 0; i < MAX_PEERS; i++)
		if (T.peers[i].state != P_FREE) peer_step(&T.peers[i]);
	if (*err_msg && T.f) { tor_stop(true); return -1; }
	if (T.now - T.rate_t >= SECS(1)) {
		T.rate = T.rate_bytes * 60 / (T.now - T.rate_t);
		T.rate_bytes = 0;
		T.rate_t = T.now;
	}
	return progress();
}

void tor_stats(TorStats *s) {
	memset(s, 0, sizeof(*s));
	for (int i = 0; i < MAX_PEERS; i++) {
		if (T.peers[i].state == P_ACTIVE) {
			s->peers++;
			if (T.peers[i].choking) s->choked++;
		}
	}
	s->known = T.nknown;
	s->rate = T.rate;
	s->done = T.recv_bytes;
	s->size = T.fe - T.fs;
	s->status = T.preallocating ? "prepare" : T.tr_state ? "tracker" : s->peers ? "peers" : "search";
}
