// SHA-1 (for the info_hash and piece verification of torrents)
#pragma once
#include <nds/ndstypes.h>

typedef struct {
	u32 h[5];
	u64 len;
	u8 buf[64];
	int n;
} Sha1;

void sha1_init(Sha1 *s);
void sha1_update(Sha1 *s, const void *data, u32 len);
void sha1_final(Sha1 *s, u8 out[20]);
void sha1(const void *data, u32 len, u8 out[20]);
