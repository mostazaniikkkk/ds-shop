// Store catalog: the titles from one or more servers
// (http://host:port, see server/ and PROTOCOLO.md). Stores are saved in
// sd:/data/dsstore/fuentes.txt, one per line: Name|URL
// Downloads go to the same folder as the store's .nds.
#pragma once
#include <nds.h>
#include <time.h>

#define DSSTORE_DIR "/data/dsstore"
#define CONFIG_FILE DSSTORE_DIR "/config.ini"
#define SOURCES_FILE DSSTORE_DIR "/fuentes.txt"
#define LOG_FILE DSSTORE_DIR "/log.txt"
#define MAX_TITLES 512
#define MAX_CATEGORIES 24
#define MAX_SOURCES 16

typedef struct {
	char name[48];
	char url[128];
} Source;

typedef struct {
	char file[64];	// file name when saving it
	char title[3][64];	// title lines (UTF-8)
	u32 size;
	time_t mtime;
	u32 remote_id;	// id on the server (remote source)
	u8 category;	// index into categories[]; 0 = uncategorized
	u8 source;	// index into sources[] it comes from
	bool has_icon, has_desc;
	u8 icon[512];
	u16 pal[16];
	char gamecode[5];
} Title;

typedef struct {
	char name[48];
	int count;
} Category;

extern Source sources[MAX_SOURCES];
extern int nsources, cur_source;
extern Title *titles;
extern int ntitles;
extern Category categories[MAX_CATEGORIES];
extern int ncategories;	// categories[0] = "sin categoria"
extern char store_name[64], store_message[320];
extern bool store_merge;	// true: titles from all stores are merged
extern char store_torrent_url[256];	// last .torrent link (to resume)
extern bool store_torrents;	// BitTorrent downloads (experimental, disabled by default)

bool store_init(const char *rom_path);	// mounts the SD and reads the settings; false if there is no DLDI
void sources_load(void);
bool sources_save(void);

// Catalog loading. Cleared with catalog_clear() and each source is added to
// what is already there (categories with the same name are merged and duplicate
// titles -same size and same file or title- appear only once).
void catalog_clear(void);
enum { CAT_PENDING = 0, CAT_OK = 1, CAT_ERR_CONNECT = -1, CAT_ERR_DATA = -2, CAT_ERR_TIMEOUT = -3 };
bool catalog_fetch_begin(int source);	// adds the catalog of a remote store
int catalog_fetch_poll(void);

const char *store_dest(void);	// downloads folder (that of the store's .nds)
void store_save_config(void);
bool store_installed(const Title *t);	// already exists in the destination folder
u32 store_blocks(u32 bytes);	// 128 KB blocks, like on the DSi
u64 store_free_bytes(void);
bool store_read_desc(const Title *t, char *buf, int len, bool allow_network);

// HTTP download, in parts so it can be animated
bool store_copy_begin(const Title *t);
int store_copy_step(void);	// returns 0..1000; -1 = error
void store_copy_abort(bool keep_partial);	// keep_partial: keep the .part to resume
u32 store_partial(const Title *t);	// bytes of a partial download that can be resumed
u32 store_copy_resumed(void);	// where the current download resumes from (0 = from the start)
bool store_copy_retrying(void);	// the connection dropped and will be retried
extern bool store_copy_kept_partial;	// after an error: the downloaded data was kept
const char *store_copy_dest_path(void);
