// DS BitTorrent client (experimental).
//
// - Reads a .torrent (downloaded over HTTP; the DS does not support HTTPS).
// - Asks the torrent's http:// and udp:// trackers for peers (no DHT).
// - Connects to peers (outgoing connections only) and downloads the chosen
//   file in 16 KB blocks, piece by piece and in order.
// - Verifies each piece against its SHA-1 before accepting it.
// - Does not upload data to anyone (never unchokes other peers).
// - Resumable: progress is saved in <file>.part.info.
#pragma once
#include <nds.h>

#define TOR_MAX_FILES 64
#define TOR_MAX_TRACKERS 12

typedef struct {
	char path[128];	// path within the torrent (UTF-8, with '/')
	u64 offset;	// position within the torrent data
	u64 size;
} TorFile;

typedef struct {
	u8 info_hash[20];
	char name[96];
	u32 piece_len, npieces;
	u64 total;
	u8 *hashes;	// npieces * 20
	TorFile files[TOR_MAX_FILES];
	int nfiles;
	char trackers[TOR_MAX_TRACKERS][128];
	int ntrackers;
} TorMeta;

// Loads an already downloaded .torrent. Returns NULL or the failure reason.
const char *tor_load(const u8 *buf, int len, TorMeta *m);
void tor_free(TorMeta *m);

typedef struct {
	int peers;	// connected peers
	int known;	// known peers
	int choked;	// connected but not letting us download
	u32 rate;	// bytes per second (approx.)
	u64 done, size;	// of the chosen file
	const char *status;	// what it is doing now
} TorStats;

// Starts (or resumes) downloading files[file] into dest (full path of the final file).
bool tor_start(TorMeta *m, int file, const char *dest);
int tor_step(void);	// call every frame: 0..1000 progress, 1000 = done, -1 = error
void tor_stop(bool keep_partial);
void tor_stats(TorStats *s);
const char *tor_error(void);
