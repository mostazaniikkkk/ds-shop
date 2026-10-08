#include <string.h>
#include "sha1.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

// Compiled as ARM (not Thumb) and in ITCM so it is fast on the DS
ITCM_CODE static void block(u32 h[5], const u8 *p) {
	u32 w[80];
	for (int i = 0; i < 16; i++)
		w[i] = (u32)p[i * 4] << 24 | (u32)p[i * 4 + 1] << 16 | (u32)p[i * 4 + 2] << 8 | p[i * 4 + 3];
	for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
	for (int i = 0; i < 80; i++) {
		u32 f, k;
		if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
		else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
		else { f = b ^ c ^ d; k = 0xCA62C1D6; }
		u32 t = ROL(a, 5) + f + e + k + w[i];
		e = d; d = c; c = ROL(b, 30); b = a; a = t;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_init(Sha1 *s) {
	s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE; s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
	s->len = 0;
	s->n = 0;
}

void sha1_update(Sha1 *s, const void *data, u32 len) {
	const u8 *p = data;
	s->len += len;
	if (s->n) {
		u32 k = 64 - s->n < len ? 64 - s->n : len;
		memcpy(s->buf + s->n, p, k);
		s->n += k; p += k; len -= k;
		if (s->n == 64) { block(s->h, s->buf); s->n = 0; }
	}
	while (len >= 64) { block(s->h, p); p += 64; len -= 64; }
	if (len) { memcpy(s->buf, p, len); s->n = len; }
}

void sha1_final(Sha1 *s, u8 out[20]) {
	u64 bits = s->len * 8;
	u8 pad = 0x80;
	sha1_update(s, &pad, 1);
	u8 z = 0;
	while (s->n != 56) sha1_update(s, &z, 1);
	u8 lenb[8];
	for (int i = 0; i < 8; i++) lenb[i] = bits >> (56 - i * 8);
	sha1_update(s, lenb, 8);
	for (int i = 0; i < 5; i++) {
		out[i * 4] = s->h[i] >> 24; out[i * 4 + 1] = s->h[i] >> 16;
		out[i * 4 + 2] = s->h[i] >> 8; out[i * 4 + 3] = s->h[i];
	}
}

void sha1(const void *data, u32 len, u8 out[20]) {
	Sha1 s;
	sha1_init(&s);
	sha1_update(&s, data, len);
	sha1_final(&s, out);
}
