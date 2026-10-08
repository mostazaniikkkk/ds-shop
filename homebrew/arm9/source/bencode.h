// Minimal bencode reader (the format of .torrent files and trackers).
// Works on the original buffer, without copying or allocating memory.
#pragma once
#include <nds/ndstypes.h>
#include <stdbool.h>

typedef struct {
	const u8 *p, *end;	// the whole value: p points to 'd', 'l', 'i' or the first digit
} BVal;

bool bv_parse(const u8 *buf, int len, BVal *out);	// the value starting at buf
const u8 *bv_skip(const u8 *p, const u8 *end);	// end of the value starting at p (NULL if malformed)
bool bv_dict_get(BVal d, const char *key, BVal *out);
bool bv_int(BVal v, s64 *out);
bool bv_str(BVal v, const u8 **s, int *len);
bool bv_str_copy(BVal v, char *dst, int dstlen);	// 0-terminated string (truncated)
// Walks a list: it starts at zero; returns false at the end
bool bv_list_next(BVal list, const u8 **it, BVal *item);
