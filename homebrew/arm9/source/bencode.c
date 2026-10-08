#include <string.h>
#include "bencode.h"

static const u8 *str_end(const u8 *p, const u8 *end) {
	s64 n = 0;
	if (p >= end || *p < '0' || *p > '9') return NULL;
	while (p < end && *p >= '0' && *p <= '9') {
		n = n * 10 + (*p++ - '0');
		if (n > 0x7FFFFFFF) return NULL;
	}
	if (p >= end || *p != ':') return NULL;
	p++;
	return end - p >= n ? p + n : NULL;
}

const u8 *bv_skip(const u8 *p, const u8 *end) {
	if (p >= end) return NULL;
	switch (*p) {
	case 'i':
		while (++p < end && *p != 'e') {}
		return p < end ? p + 1 : NULL;
	case 'l':
	case 'd':
		p++;
		while (p < end && *p != 'e') {
			p = bv_skip(p, end);
			if (!p) return NULL;
		}
		return p < end ? p + 1 : NULL;
	default:
		return str_end(p, end);
	}
}

bool bv_parse(const u8 *buf, int len, BVal *out) {
	const u8 *e = bv_skip(buf, buf + len);
	if (!e) return false;
	out->p = buf;
	out->end = e;
	return true;
}

bool bv_str(BVal v, const u8 **s, int *len) {
	const u8 *e = str_end(v.p, v.end);
	if (!e) return false;
	const u8 *c = v.p;
	while (*c != ':') c++;
	*s = c + 1;
	*len = e - (c + 1);
	return true;
}

bool bv_str_copy(BVal v, char *dst, int dstlen) {
	const u8 *s;
	int n;
	if (!bv_str(v, &s, &n)) return false;
	if (n > dstlen - 1) n = dstlen - 1;
	memcpy(dst, s, n);
	dst[n] = 0;
	return true;
}

bool bv_int(BVal v, s64 *out) {
	const u8 *p = v.p;
	if (p >= v.end || *p != 'i') return false;
	p++;
	bool neg = false;
	if (*p == '-') { neg = true; p++; }
	s64 n = 0;
	while (p < v.end && *p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
	if (p >= v.end || *p != 'e') return false;
	*out = neg ? -n : n;
	return true;
}

bool bv_list_next(BVal list, const u8 **it, BVal *item) {
	if (list.p >= list.end || (*list.p != 'l' && *list.p != 'd')) return false;
	const u8 *p = *it ? *it : list.p + 1;
	if (p >= list.end || *p == 'e') return false;
	const u8 *e = bv_skip(p, list.end);
	if (!e) return false;
	item->p = p;
	item->end = e;
	*it = e;
	return true;
}

bool bv_dict_get(BVal d, const char *key, BVal *out) {
	if (d.p >= d.end || *d.p != 'd') return false;
	int klen = strlen(key);
	const u8 *p = d.p + 1;
	while (p < d.end && *p != 'e') {
		const u8 *ke = str_end(p, d.end);
		if (!ke) return false;
		const u8 *ks = p;
		while (*ks != ':') ks++;
		ks++;
		const u8 *ve = bv_skip(ke, d.end);
		if (!ve) return false;
		if (ke - ks == klen && !memcmp(ks, key, klen)) {
			out->p = ke;
			out->end = ve;
			return true;
		}
		p = ve;
	}
	return false;
}
