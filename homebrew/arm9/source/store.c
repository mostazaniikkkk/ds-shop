#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "net.h"
#include "store.h"
#include "text.h"

Source sources[MAX_SOURCES];
int nsources, cur_source;
Title *titles;
int ntitles;
Category categories[MAX_CATEGORIES];
int ncategories;
char store_name[64], store_message[320];
bool store_merge = true;
bool store_torrents = false;
char store_torrent_url[256];

// Category by name (case-insensitive); creates it if it does not exist.
static int category_index(const char *name) {
	if (!name[0]) return 0;
	for (int c = 1; c < ncategories; c++)
		if (!strcasecmp(categories[c].name, name)) return c;
	if (ncategories >= MAX_CATEGORIES) return 0;
	int c = ncategories++;
	snprintf(categories[c].name, sizeof(categories[c].name), "%s", name);
	categories[c].count = 0;
	return c;
}

// The same game in two stores is shown once (the one from the first store
// in the list): same size and, also, same file or same title and code.
static bool duplicated(const Title *t) {
	for (int i = 0; i < ntitles; i++) {
		const Title *o = &titles[i];
		if (o->size != t->size) continue;
		if (!strcasecmp(o->file, t->file)) return true;
		if (!strcmp(o->title[0], t->title[0]) && !strcmp(o->gamecode, t->gamecode)) return true;
	}
	return false;
}

// Downloads are saved next to the store's .nds (argv[0]); if the
// launcher does not provide it (some old kernels), at the SD root.
static char dest_dir[192] = "/";

static void set_dest_from_rom(const char *rom) {
	if (!rom || !*rom) return;
	const char *p = strchr(rom, ':');	// "fat:/nds/DSShop.nds" -> "/nds/DSShop.nds"
	if (p && p[1] == '/') rom = p + 1;
	const char *slash = strrchr(rom, '/');
	if (!slash || rom[0] != '/') return;
	int n = slash - rom + 1;
	if (n >= (int)sizeof(dest_dir)) return;
	memcpy(dest_dir, rom, n);
	dest_dir[n] = 0;
}

extern void snd_set_music_enabled(bool on);
extern bool snd_music_enabled(void);

static void load_config(void) {
	FILE *f = fopen(CONFIG_FILE, "r");
	if (!f) return;
	char line[300];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "music=", 6)) {
			snd_set_music_enabled(line[6] != '0');
		} else if (!strncmp(line, "mezclar=", 8)) {
			store_merge = line[8] != '0';
		} else if (!strncmp(line, "torrents=", 9)) {
			store_torrents = line[9] == '1';
		} else if (!strncmp(line, "torrent_url=", 12)) {
			snprintf(store_torrent_url, sizeof(store_torrent_url), "%s", line + 12);
		}
	}
	fclose(f);
}

static void mkdirs(const char *path) {
	char tmp[192];
	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = 0;
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
	}
	mkdir(tmp, 0777);
}

void store_save_config(void) {
	mkdirs(DSSTORE_DIR);
	FILE *f = fopen(CONFIG_FILE, "w");
	if (!f) return;
	fprintf(f, "music=%d\nmezclar=%d\ntorrents=%d\n", snd_music_enabled() ? 1 : 0, store_merge ? 1 : 0, store_torrents ? 1 : 0);
	if (store_torrent_url[0]) fprintf(f, "torrent_url=%s\n", store_torrent_url);
	fclose(f);
}

// Older versions stored the settings in sd:/dsishop/
static void migrate_old_config(void) {
	static const char *const files[] = { "fuentes.txt", "config.ini", "log.txt" };
	struct stat st;
	if (stat("/dsishop", &st)) return;
	mkdirs(DSSTORE_DIR);
	for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		char from[64], to[64];
		snprintf(from, sizeof(from), "/dsishop/%s", files[i]);
		snprintf(to, sizeof(to), DSSTORE_DIR "/%s", files[i]);
		if (!stat(from, &st) && stat(to, &st)) rename(from, to);
		else remove(from);
	}
	rmdir("/dsishop");	// only if it is now empty
}

bool store_init(const char *rom_path) {
	if (!titles) titles = malloc(sizeof(Title) * MAX_TITLES);
	set_dest_from_rom(rom_path);
	if (!fatInitDefault()) return false;
	migrate_old_config();
	load_config();
	return true;
}

void catalog_clear(void) {
	ntitles = 0;
	ncategories = 1;
	categories[0].name[0] = 0;
	categories[0].count = 0;
	store_name[0] = store_message[0] = 0;
}

// One persistent connection per store (see net.h)
static Http conns[MAX_SOURCES];

static void close_conns(void) {
	for (int i = 0; i < MAX_SOURCES; i++) http_close(&conns[i]);
}

// ------------------------------------------------------------------ sources
static const char sources_example[] =
	"# Tiendas de la Tienda DS: una por linea, con el formato  Nombre|URL\n"
	"# Solo HTTP (la DS no admite HTTPS). Ejemplo:\n"
	"# Mi servidor|http://192.168.1.10:8080\n";

void sources_load(void) {
	close_conns();
	nsources = 0;
	cur_source = 0;

	FILE *f = fopen(SOURCES_FILE, "r");
	if (!f) {
		mkdirs(DSSTORE_DIR);
		f = fopen(SOURCES_FILE, "w");
		if (f) { fputs(sources_example, f); fclose(f); }
		return;
	}
	char line[200];
	while (fgets(line, sizeof(line), f) && nsources < MAX_SOURCES) {
		line[strcspn(line, "\r\n")] = 0;
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (!*p || *p == '#') continue;
		Source *s = &sources[nsources];
		char *bar = strchr(p, '|');
		const char *url = bar ? bar + 1 : p;
		while (*url == ' ') url++;
		if (bar) {
			*bar = 0;
			char *e = bar;
			while (e > p && e[-1] == ' ') *--e = 0;
		}
		snprintf(s->url, sizeof(s->url), "%s", url);
		snprintf(s->name, sizeof(s->name), "%s", bar && *p ? p : url);
		if (s->url[0]) nsources++;
	}
	fclose(f);
}


// ------------------------------------------------------------------ remote catalog (PROTOCOLO.md)
#define CATALOG_MAX (512 * 1024)
static Http *cat_http;
static u8 *cat_buf;
static int cat_len, cat_frames, cat_source;

bool catalog_fetch_begin(int source) {
	cat_source = source;
	if (!cat_buf) cat_buf = malloc(CATALOG_MAX);
	if (!cat_buf) return false;
	cat_len = 0;
	cat_frames = 0;
	char path[48];
	snprintf(path, sizeof(path), "/ds/v1/catalog?lang=%d", text_lang());
	cat_http = &conns[source];
	return http_get(cat_http, sources[source].url, path);
}

typedef struct { const u8 *p, *end; bool bad; } Rd;

static u32 rd_n(Rd *r, int n) {
	if (r->p + n > r->end) { r->bad = true; return 0; }
	u32 v = 0;
	for (int i = 0; i < n; i++) v |= (u32)r->p[i] << (i * 8);
	r->p += n;
	return v;
}

static void rd_str(Rd *r, int lenbytes, char *dst, int dstlen) {
	u32 n = rd_n(r, lenbytes);
	if (r->bad || r->p + n > r->end) { r->bad = true; dst[0] = 0; return; }
	int c = n < (u32)dstlen - 1 ? (int)n : dstlen - 1;
	memcpy(dst, r->p, c);
	dst[c] = 0;
	r->p += n;
}

static void rd_bytes(Rd *r, void *dst, int n) {
	if (r->p + n > r->end) { r->bad = true; return; }
	memcpy(dst, r->p, n);
	r->p += n;
}

static bool parse_catalog(const u8 *b, int len) {
	Rd r = { b, b + len, false };
	if (len < 8 || memcmp(b, "DSSC", 4) || b[4] != 1) return false;
	r.p += 8;
	rd_n(&r, 4);	// revision
	rd_str(&r, 2, store_name, sizeof(store_name));
	rd_str(&r, 2, store_message, sizeof(store_message));

	// server categories -> local categories (by name)
	u16 server_id[64];
	u8 local_cat[64];
	int ncat = rd_n(&r, 2), nmap = 0;
	for (int i = 0; i < ncat && !r.bad; i++) {
		u16 id = rd_n(&r, 2);
		char name[64];
		rd_str(&r, 1, name, sizeof(name));
		if (nmap < 64) {
			server_id[nmap] = id;
			local_cat[nmap++] = category_index(name);
		}
	}
	int n = rd_n(&r, 2);
	for (int i = 0; i < n && !r.bad; i++) {
		Title tmp, *t = ntitles < MAX_TITLES ? &titles[ntitles] : &tmp;
		memset(t, 0, sizeof(*t));
		t->remote_id = rd_n(&r, 4);
		u16 cat = rd_n(&r, 2);
		t->size = rd_n(&r, 4);
		t->mtime = rd_n(&r, 4);
		rd_bytes(&r, t->gamecode, 4);
		t->gamecode[4] = 0;
		u8 flags = rd_n(&r, 1);
		int lines = rd_n(&r, 1);
		for (int l = 0; l < lines; l++) {
			char skip[64];
			rd_str(&r, 1, l < 3 ? t->title[l] : skip, 64);
		}
		rd_str(&r, 1, t->file, sizeof(t->file));
		if (flags & 1) {
			rd_bytes(&r, t->icon, 512);
			rd_bytes(&r, t->pal, 32);
			t->has_icon = true;
		}
		t->has_desc = flags & 2;
		// the file name comes from the server: keep it inside the folder
		for (char *c = t->file; *c; c++)
			if (*c == '/' || *c == '\\' || *c == ':') *c = '_';
		if (!t->file[0] || t->file[0] == '.') snprintf(t->file, sizeof(t->file), "%lu.nds", t->remote_id);
		t->category = 0;
		for (int c = 0; c < nmap; c++)
			if (server_id[c] == cat) t->category = local_cat[c];
		t->source = cat_source;
		if (t != &tmp && !r.bad && !duplicated(t)) {
			categories[t->category].count++;
			ntitles++;
		}
	}
	return !r.bad;
}

int catalog_fetch_poll(void) {
	if (++cat_frames > 60 * 30) { net_error = "timeout"; http_close(cat_http); return CAT_ERR_TIMEOUT; }
	for (int i = 0; i < 16; i++) {
		int r = http_read(cat_http, cat_buf + cat_len, CATALOG_MAX - cat_len);
		if (r < 0) { net_error = "recv"; http_close(cat_http); return CAT_ERR_CONNECT; }
		cat_len += r;
		if (cat_http->eof || cat_len >= CATALOG_MAX || r == 0) break;
	}
	if (!cat_http->eof) {
		if (cat_len < CATALOG_MAX) return CAT_PENDING;
		http_close(cat_http);
		return CAT_ERR_DATA;
	}
	if (cat_http->status != 200) { net_error = "HTTP"; return CAT_ERR_DATA; }
	if (!parse_catalog(cat_buf, cat_len)) { net_error = "DSSC"; return CAT_ERR_DATA; }
	net_log("catalogo: %d bytes, %d titulos, %d categorias", cat_len, ntitles, ncategories - 1);
	return CAT_OK;
}

const char *store_dest(void) { return dest_dir; }

static void dest_path(const Title *t, char *buf, int len) {
	snprintf(buf, len, "%s%s", dest_dir, t->file);
}

bool store_installed(const Title *t) {
	char p[256];
	struct stat st;
	dest_path(t, p, sizeof(p));
	return !stat(p, &st) && (u32)st.st_size == t->size;
}

u32 store_blocks(u32 bytes) { return (bytes + 131071) / 131072; }

u64 store_free_bytes(void) {
	struct statvfs st;
	if (statvfs("/", &st)) return 0;
	return (u64)st.f_bavail * st.f_bsize;
}

bool store_read_desc(const Title *t, char *buf, int len, bool allow_network) {
	if (!allow_network || !t->has_desc) return false;
	char path[48];
	snprintf(path, sizeof(path), "/ds/v1/title/%lu/desc", t->remote_id);
	int n = http_fetch(&conns[t->source], sources[t->source].url, path, (u8 *)buf, len - 1, 60 * 8);
	buf[n > 0 ? n : 0] = 0;
	return n > 0;
}

// ------------------------------------------------------------------ copy
// The ROM is requested in chunks of REQ_SIZE bytes ("Range: bytes=a-b"), one
// connection per chunk, always reading each response in full. Closing a dswifi
// socket while the server is still sending leaves the old connection
// receiving data nobody reads and ends up clogging the network for the rest; with
// chunks, cancelling only needs to finish reading the current chunk.
//
// Written to <file>.part. If interrupted, the .part is kept along with
// <file>.part.info (store, id and size) to resume later.
#define CHUNK (32 * 1024)
#define REQ_SIZE (256 * 1024)
static FILE *dst_f;
static u32 copied, total, resume_from, req_end;
static char cur_dest[256], tmp_dest[260], info_dest[264], info_text[200];
static u8 *chunk;
static bool hdr_checked, req_open;
static Http *dl;	// the connection of the download's store
static int dl_idle, retries, retry_wait, dl_source;
static bool retry_pending;
static u32 synced;
static char dl_path[48];

// If the connection drops, it retries from where it left off
#define MAX_RETRIES 5
#define RETRY_FRAMES (60 * 3)

static u32 file_size(const char *p) {
	struct stat st;
	return stat(p, &st) ? 0 : (u32)st.st_size;
}

// Returns the bytes already downloaded in a valid .part for this title, or 0.
static u32 partial_bytes(void) {
	FILE *f = fopen(info_dest, "r");
	if (!f) return 0;
	char line[200] = "";
	fgets(line, sizeof(line), f);
	fclose(f);
	line[strcspn(line, "\r\n")] = 0;
	if (strcmp(line, info_text)) return 0;
	u32 n = file_size(tmp_dest);
	return n < total ? n : 0;
}

u32 store_partial(const Title *t) {
	dest_path(t, cur_dest, sizeof(cur_dest));
	snprintf(tmp_dest, sizeof(tmp_dest), "%s.part", cur_dest);
	snprintf(info_dest, sizeof(info_dest), "%s.part.info", cur_dest);
	snprintf(info_text, sizeof(info_text), "%s|%lu|%lu", sources[t->source].url, t->remote_id, t->size);
	total = t->size;
	return partial_bytes();
}

// Requests the next chunk, starting at copied
static bool start_request(void) {
	req_end = copied + REQ_SIZE < total ? copied + REQ_SIZE : total;
	hdr_checked = false;
	dl_idle = 0;
	dl = &conns[dl_source];
	net_wifi_stalled();
	net_log_stats("tramo");
	req_open = http_get_range(dl, sources[dl_source].url, dl_path, copied, req_end - 1);
	return req_open;
}

static void close_request(void) {
	// a partial response makes the connection unusable; a complete one is reused
	if (req_open && !dl->eof) http_close(dl);
	req_open = false;
}

bool store_copy_begin(const Title *t) {
	if (!chunk) chunk = malloc(CHUNK);
	if (!chunk) return false;
	mkdirs(dest_dir);
	dl_source = t->source;
	resume_from = store_partial(t);	// also sets up the paths
	snprintf(dl_path, sizeof(dl_path), "/ds/v1/title/%lu/file", t->remote_id);
	retries = retry_wait = 0;
	retry_pending = false;
	copied = synced = resume_from;
	if (!resume_from) {
		FILE *f = fopen(info_dest, "w");
		if (f) { fprintf(f, "%s\n", info_text); fclose(f); }
	}
	dst_f = fopen(tmp_dest, resume_from ? "ab" : "wb");
	if (!dst_f) { store_copy_abort(false); return false; }
	setvbuf(dst_f, NULL, _IONBF, 0);
	if (!start_request()) { store_copy_abort(false); return false; }
	if (resume_from) net_log("reanudando desde %lu de %lu bytes", resume_from, total);
	return true;
}

u32 store_copy_resumed(void) { return resume_from; }
bool store_copy_retrying(void) { return retry_pending; }

static int finish_copy(void) {
	fclose(dst_f);
	dst_f = NULL;
	close_request();
	remove(info_dest);
	remove(cur_dest);
	if (rename(tmp_dest, cur_dest)) return -1;
	return 1000;
}

// Updates the .part size in the FAT: if the console is turned off, what
// was downloaded so far can still be used to resume.
static void sync_part(void) {
	if (!dst_f) return;
	fflush(dst_f);
	fsync(fileno(dst_f));
	synced = copied;
}

static int progress(void) {
	int p = total ? (int)((u64)copied * 1000 / total) : 0;
	return p > 999 ? 999 : p;
}

// Network error: retry; if there is no way, the downloaded data is kept
static int net_fail(void) {
	close_request();
	sync_part();
	if (retries < MAX_RETRIES) {
		retries++;
		retry_wait = RETRY_FRAMES;
		retry_pending = true;
		// if the Wi-Fi has gone silent (neither receives nor sends), reconnect
		if (net_wifi_stalled()) {
			close_conns();
			net_wifi_restart();
		}
		net_log("conexion perdida en %lu/%lu (%s); reintento %d en 3 s", copied, total, net_error, retries);
		return progress();
	}
	store_copy_abort(copied > 0);
	return -1;
}

int store_copy_step(void) {
	if (!dst_f) return -1;
	if (retry_pending) {
		if (retry_wait > 0 && --retry_wait) return progress();
		switch (net_wifi_poll()) {
		case WIFI_CONNECTING: return progress();
		case WIFI_FAILED: store_copy_abort(copied > 0); return -1;
		case WIFI_OK: break;
		}
		retry_pending = false;
		if (!start_request()) return net_fail();
		net_log("reintentando desde %lu de %lu bytes", copied, total);
	}
	// whatever arrived this frame, up to one block
	int got = 0;
	while (got < CHUNK) {
		int r = http_read(dl, chunk + got, CHUNK - got);
		if (r < 0) return net_fail();
		if (r == 0) break;
		got += r;
	}
	if (dl->headers_done && !hdr_checked) {
		hdr_checked = true;
		if (dl->status == 200) {
			// the server does not support Range: the whole file arrives
			if (copied) {
				net_log("el servidor no admite Range; se empieza de cero");
				fclose(dst_f);
				dst_f = fopen(tmp_dest, "wb");
				if (!dst_f) { store_copy_abort(false); return -1; }
				setvbuf(dst_f, NULL, _IONBF, 0);
				copied = synced = resume_from = 0;
			}
			req_end = total;
		} else if (dl->status != 206) {
			net_log("descarga: estado HTTP %d", dl->status);
			store_copy_abort(false);
			return -1;
		}
	}
	if (got) {
		dl_idle = 0;
		retries = 0;
		net_wifi_stalled();	// marker for "data was still arriving here"
		if (copied + got > req_end) got = req_end - copied;	// never more than requested
		if (fwrite(chunk, 1, got, dst_f) != (size_t)got) { store_copy_abort(false); return -1; }
		copied += got;
		if (copied - synced >= 512 * 1024) sync_part();
	} else if (++dl_idle > 60 * 15) {	// 15 s without receiving anything
		net_error = "15 s sin datos";
		net_log_stats("atasco");
		return net_fail();
	}
	if (dl->eof || copied >= req_end) {
		close_request();
		if (copied < req_end) return net_fail();	// the chunk arrived incomplete
		if (copied >= total) {
			net_log("descarga completa: %lu bytes", copied);
			return finish_copy();
		}
		if (!start_request()) return net_fail();
	}
	return progress();
}

bool store_copy_kept_partial;

void store_copy_abort(bool keep_partial) {
	// finish reading the current chunk (at most REQ_SIZE, 4 s) so that
	// the connection closes cleanly
	if (req_open && !dl->error) {
		for (int f = 0; f < 60 * 4 && !dl->eof; f++) {
			int r;
			while ((r = http_read(dl, chunk, CHUNK)) > 0) {}
			if (r < 0) break;
			swiWaitForVBlank();
			net_tick();
		}
	}
	close_request();
	if (dst_f) fclose(dst_f);
	dst_f = NULL;
	store_copy_kept_partial = keep_partial;
	if (!store_copy_kept_partial) {
		remove(tmp_dest);
		remove(info_dest);
	}
}

const char *store_copy_dest_path(void) { return cur_dest; }

// ------------------------------------------------------------------ save sources
bool sources_save(void) {
	close_conns();	// the store indices may have changed
	mkdirs(DSSTORE_DIR);
	FILE *f = fopen(SOURCES_FILE, "w");
	if (!f) return false;
	fputs(sources_example, f);
	for (int i = 0; i < nsources; i++) {
		char name[48];
		snprintf(name, sizeof(name), "%s", sources[i].name);
		for (char *c = name; *c; c++)
			if (*c == '|') *c = '/';
		fprintf(f, "%s|%s\n", name, sources[i].url);
	}
	fclose(f);
	return true;
}
