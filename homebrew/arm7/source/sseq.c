// SSEQ/SSAR sequence player (Nitro sound format) for the ARM7.
//
// Plays the original DSi Shop SDAT: sequences, instrument banks
// (SBNK) and wave archives (SWAR/SWAV), and drives the 16 hardware sound
// channels at ~192 Hz, like Nintendo's sound driver.
#include <nds.h>
#include <string.h>
#include "sndcmd.h"
#include "snd_tables.h"

#define NCHAN    16
#define NTRACK   32
#define NPLAYER  16
#define AMP_MIN  (-723 << 7)

enum { CH_FREE, CH_START, CH_ON, CH_RELEASE };
enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
enum { INST_PCM = 1, INST_PSG = 2, INST_NOISE = 3, INST_DRUMS = 16, INST_SPLIT = 17 };

typedef struct {
	u8 active, player;
	const u8 *pos, *base;
	const u8 *stack[3];
	u8 loopcnt[3], sp;
	s32 wait;
	u8 prg, vol, expr, prio, bendrange;
	s8 pan, transpose, bend;
	u8 notewait, tie, porta, portakey, portatime;
	u8 moddepth, modspeed, modtype, modrange;
	u16 moddelay;
	s16 sweep;
	u8 a, d, s, r;
	s8 tiechan;
	u8 cmp;
} Track;

typedef struct {
	u8 active, vol, mastervol, prio, ntracks;
	u16 tempo;
	s32 tempocnt;
	u8 tracks[16];
	const u8 *bank;
	const u8 *swar[4];
	s16 var[16];
} Player;

typedef struct {
	u8 state, type, env, track, player, key, basekey, vel, prio;
	u8 pan_inst, duty, fmt, loop;
	s32 len, amp, sustain;
	u16 attack, decay, release;
	u16 basetimer, loopstart;
	u32 looplen;
	const u8 *data;
	s32 sweep_pitch, sweep_len, sweep_cnt;
	u16 modcnt, moddelaycnt;
	// copy of the track parameters (in case the track ends first)
	u8 t_vol, t_expr, t_mvol, t_pvol, t_moddepth, t_modspeed, t_modtype, t_modrange;
	s8 t_pan, t_bend;
	u8 t_bendrange;
	u16 t_moddelay;
} Chan;

static const u8 *sdat;
static Track tracks[NTRACK];
static Player players[NPLAYER];
static Chan chans[NCHAN];
static s16 gvar[16];
static u32 rng = 0x12345678;

#define CMDQ 32
static volatile u32 cmdq[CMDQ];
static volatile u8 cmdq_r, cmdq_w;

static inline u16 rd16(const u8 *p) { return p[0] | (p[1] << 8); }
static inline u32 rd24(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16); }
static inline u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

static u32 rand_u32(void) { rng = rng * 1664525 + 1013904223; return rng >> 8; }

// ---------------------------------------------------------------- SDAT
static const u8 *sdat_info(int rec, int idx) {
	const u8 *info = sdat + rd32(sdat + 0x18);
	const u8 *r = info + rd32(info + 8 + rec * 4);
	if ((u32)idx >= rd32(r)) return NULL;
	u32 off = rd32(r + 4 + idx * 4);
	return off ? info + off : NULL;
}

static const u8 *sdat_file(u32 id) {
	const u8 *fat = sdat + rd32(sdat + 0x20);
	if (id >= rd32(fat + 8)) return NULL;
	return sdat + rd32(fat + 12 + id * 16);
}

enum { REC_SEQ, REC_SEQARC, REC_BANK, REC_WAVEARC };

static void load_bank(Player *p, int bank) {
	const u8 *bi = sdat_info(REC_BANK, bank);
	p->bank = bi ? sdat_file(rd16(bi)) : NULL;
	for (int i = 0; i < 4; i++) {
		p->swar[i] = NULL;
		if (!bi) continue;
		u16 w = rd16(bi + 4 + i * 2);
		if (w == 0xFFFF) continue;
		const u8 *wi = sdat_info(REC_WAVEARC, w);
		if (wi) p->swar[i] = sdat_file(rd32(wi) & 0xFFFFFF);
	}
}

// ---------------------------------------------------------------- channels
static u16 timer_adjust(u16 base, int pitch) {
	int shift = 0;
	pitch = -pitch;
	while (pitch < 0) { shift--; pitch += 768; }
	while (pitch >= 768) { shift++; pitch -= 768; }
	u64 t = (u64)base * (pitch_tbl[pitch] + 0x10000);
	shift -= 16;
	if (shift <= 0) t >>= -shift;
	else if (shift < 32) { if (t & (~0ULL << (32 - shift))) return 0xFFFF; t <<= shift; }
	else return 0x10;
	if (t < 0x10) return 0x10;
	if (t > 0xFFFF) return 0xFFFF;
	return t;
}

static u16 attack_rate(u8 a) { return a >= 109 ? attack_tbl[127 - a] : 255 - a; }
static u16 fall_rate(u8 x) {
	if (x == 127) return 0xFFFF;
	if (x == 126) return 0x3C00;
	if (x < 50) return x * 2 + 1;
	return 0x1E00 / (126 - x);
}

static void chan_kill(int i) {
	SCHANNEL_CR(i) = 0;
	chans[i].state = CH_FREE;
}

static void chan_release(int i) {
	Chan *c = &chans[i];
	if (c->state == CH_FREE) return;
	if (c->release == 0xFFFF) { chan_kill(i); return; }
	c->state = CH_RELEASE;
	c->env = ENV_RELEASE;
}

static int chan_alloc(int type, int prio) {
	int lo = 0, hi = 15;
	if (type == INST_PSG) { lo = 8; hi = 13; }
	else if (type == INST_NOISE) { lo = 14; hi = 15; }
	int best = -1;
	for (int i = lo; i <= hi; i++) {
		Chan *c = &chans[i];
		if (c->state == CH_FREE) return i;
		if (best < 0) { best = i; continue; }
		Chan *b = &chans[best];
		if (c->prio < b->prio || (c->prio == b->prio && c->amp < b->amp)) best = i;
	}
	if (best >= 0 && chans[best].prio > prio) return -1;
	if (best >= 0) chan_kill(best);
	return best;
}

static int lfo_value(Chan *c) {
	if (!c->t_moddepth) return 0;
	if (c->moddelaycnt < c->t_moddelay) { c->moddelaycnt++; return 0; }
	int idx = (c->modcnt >> 8) & 127;
	c->modcnt = (c->modcnt + (c->t_modspeed << 6)) & 0x7FFF;
	int s;
	if (idx < 32) s = sine_tbl[idx];
	else if (idx < 64) s = sine_tbl[64 - idx];
	else if (idx < 96) s = -sine_tbl[idx - 64];
	else s = -sine_tbl[128 - idx];
	return s * c->t_moddepth * c->t_modrange;
}

static void chan_update(int i) {
	Chan *c = &chans[i];
	if (c->state == CH_FREE) return;
	if (c->state != CH_START && !(SCHANNEL_CR(i) & SCHANNEL_ENABLE)) { c->state = CH_FREE; return; }

	// live track parameters
	if (c->track < NTRACK && tracks[c->track].active) {
		Track *t = &tracks[c->track];
		Player *p = &players[t->player];
		c->t_vol = t->vol; c->t_expr = t->expr; c->t_pan = t->pan; c->t_bend = t->bend;
		c->t_bendrange = t->bendrange; c->t_mvol = p->mastervol; c->t_pvol = p->vol;
		c->t_moddepth = t->moddepth; c->t_modspeed = t->modspeed; c->t_modtype = t->modtype;
		c->t_modrange = t->modrange; c->t_moddelay = t->moddelay;
	}

	// envelope
	switch (c->env) {
	case ENV_ATTACK:
		c->amp = (c->amp * (s32)c->attack) / 256;
		if (c->amp == 0) c->env = ENV_DECAY;
		break;
	case ENV_DECAY:
		c->amp -= c->decay;
		if (c->amp <= c->sustain) { c->amp = c->sustain; c->env = ENV_SUSTAIN; }
		break;
	case ENV_RELEASE:
		c->amp -= c->release;
		if (c->amp <= AMP_MIN) { chan_kill(i); return; }
		break;
	}

	int lfo = lfo_value(c);
	int db = dbsq_tbl[c->t_pvol] + dbsq_tbl[c->t_mvol] + dbsq_tbl[c->t_vol] + dbsq_tbl[c->t_expr]
	       + dbsq_tbl[c->vel] + (c->amp >> 7);
	int pitch = (c->key - c->basekey) * 64 + ((c->t_bend * c->t_bendrange * 64) >> 7);
	int pan = 64 + c->t_pan + (c->pan_inst - 64);

	if (c->sweep_pitch && c->sweep_cnt < c->sweep_len) {
		pitch += (s64)c->sweep_pitch * (c->sweep_len - c->sweep_cnt) / c->sweep_len;
		c->sweep_cnt++;
	}
	if (c->t_modtype == 0) pitch += (lfo * 64) >> 14;
	else if (c->t_modtype == 1) db += (lfo * 60) >> 14;
	else pan += (lfo * 64) >> 14;

	if (db < -723) db = -723;
	if (db > 0) db = 0;
	if (pan < 0) pan = 0;
	if (pan > 127) pan = 127;
	u8 vol = vol_tbl[db + 723];
	u8 div = db < -240 ? 3 : db < -120 ? 2 : db < -60 ? 1 : 0;
	u16 timer = timer_adjust(c->basetimer, pitch);

	if (c->state == CH_START) {
		SCHANNEL_CR(i) = 0;
		SCHANNEL_TIMER(i) = -timer;
		u32 cr = vol | (div << 8) | (pan << 16) | SCHANNEL_ENABLE;
		if (c->type == INST_PCM) {
			SCHANNEL_SOURCE(i) = (u32)c->data;
			SCHANNEL_REPEAT_POINT(i) = c->loopstart;
			SCHANNEL_LENGTH(i) = c->looplen;
			cr |= (c->loop ? SOUND_REPEAT : SOUND_ONE_SHOT) | ((u32)c->fmt << 29);
		} else {
			cr |= SOUND_FORMAT_PSG | ((u32)(c->duty & 7) << 24);
		}
		SCHANNEL_CR(i) = cr;
		c->state = CH_ON;
	} else {
		*(vu8 *)(0x04000400 + (i << 4)) = vol;
		*(vu8 *)(0x04000401 + (i << 4)) = div;
		SCHANNEL_PAN(i) = pan;
		SCHANNEL_TIMER(i) = -timer;
	}
}

// ---------------------------------------------------------------- notes
// Returns the instrument record (10 bytes: swav, swar, key, a, d, s, r, pan) and its type.
static const u8 *find_inst(Player *p, int prg, int key, u8 *type) {
	if (!p->bank) return NULL;
	u32 n = rd32(p->bank + 0x38);
	if ((u32)prg >= n) return NULL;
	const u8 *rec = p->bank + 0x3C + prg * 4;
	*type = rec[0];
	const u8 *d = p->bank + rd16(rec + 1);
	if (*type == INST_DRUMS) {
		u8 lo = d[0], hi = d[1];
		if (key < lo || key > hi) return NULL;
		const u8 *e = d + 2 + (key - lo) * 12;
		*type = e[0];
		return e + 2;
	}
	if (*type == INST_SPLIT) {
		const u8 *e = d + 8;
		for (int r = 0; r < 8 && d[r]; r++, e += 12) {
			if (key <= d[r]) { *type = e[0]; return e + 2; }
		}
		return NULL;
	}
	return (*type >= INST_PCM && *type <= INST_NOISE) ? d : NULL;
}

static void note_on(int ti, int key, int vel, s32 len) {
	Track *t = &tracks[ti];
	Player *p = &players[t->player];
	key += t->transpose;
	if (key < 0) key = 0;
	if (key > 127) key = 127;

	// tie mode: legato on the same channel
	if (t->tie && t->tiechan >= 0) {
		Chan *c = &chans[(int)t->tiechan];
		if (c->state != CH_FREE && c->track == ti) {
			if (t->porta) { c->sweep_pitch = (c->key - key) << 6; c->sweep_cnt = 0; }
			c->key = key; c->vel = vel;
			t->portakey = key;
			return;
		}
	}

	u8 type;
	const u8 *in = find_inst(p, t->prg, key, &type);
	if (!in) return;
	int prio = t->prio + p->prio;
	if (prio > 255) prio = 255;
	int ci = chan_alloc(type, prio);
	if (ci < 0) return;
	Chan *c = &chans[ci];

	c->type = type;
	if (type == INST_PCM) {
		const u8 *swar = p->swar[rd16(in + 2) & 3];
		if (!swar) return;
		u16 wv = rd16(in);
		if (wv >= rd32(swar + 0x38)) return;
		const u8 *w = swar + rd32(swar + 0x3C + wv * 4);
		c->fmt = w[0]; c->loop = w[1];
		c->basetimer = rd16(w + 4);
		c->loopstart = rd16(w + 6);
		c->looplen = rd32(w + 8);
		c->data = w + 12;
	} else {
		c->duty = rd16(in);
		c->basetimer = 8006;
	}
	c->basekey = in[4];
	c->attack = attack_rate(t->a != 0xFF ? t->a : in[5]);
	c->decay = fall_rate(t->d != 0xFF ? t->d : in[6]);
	c->sustain = dbsq_tbl[t->s != 0xFF ? t->s : in[7]] << 7;
	c->release = fall_rate(t->r != 0xFF ? t->r : in[8]);
	c->pan_inst = in[9];
	c->key = key; c->vel = vel; c->prio = prio;
	c->track = ti; c->player = t->player;
	c->len = t->tie ? -1 : len;
	c->amp = AMP_MIN; c->env = ENV_ATTACK;
	c->modcnt = 0; c->moddelaycnt = 0;
	c->sweep_pitch = t->sweep; c->sweep_cnt = 0; c->sweep_len = 0;
	if (t->porta) {
		c->sweep_pitch += (t->portakey - key) << 6;
		if (t->portatime == 0) c->sweep_len = len > 0 ? len * 240 / p->tempo : 1;
		else {
			s32 sp = c->sweep_pitch < 0 ? -c->sweep_pitch : c->sweep_pitch;
			c->sweep_len = (t->portatime * t->portatime * sp) >> 11;
		}
	} else if (c->sweep_pitch) {
		c->sweep_len = len > 0 ? len * 240 / p->tempo : 1;
	}
	t->portakey = key;
	if (t->tie) t->tiechan = ci;
	c->state = CH_START;
}

// ---------------------------------------------------------------- tracks
static u32 vlv(const u8 **p) {
	u32 v = 0;
	u8 b;
	do { b = *(*p)++; v = (v << 7) | (b & 0x7F); } while (b & 0x80);
	return v;
}

static void track_init(Track *t, int player, const u8 *base, const u8 *pos) {
	memset(t, 0, sizeof(*t));
	t->active = 1; t->player = player; t->base = base; t->pos = pos;
	t->vol = 127; t->expr = 127; t->prio = 64; t->bendrange = 2;
	t->notewait = 1; t->modspeed = 16; t->modrange = 1; t->portakey = 60;
	t->a = t->d = t->s = t->r = 0xFF;
	t->tiechan = -1;
}

static void player_stop(int pi, int hard) {
	Player *p = &players[pi];
	for (int i = 0; i < p->ntracks; i++) tracks[p->tracks[i]].active = 0;
	for (int i = 0; i < NCHAN; i++) {
		if (chans[i].state != CH_FREE && chans[i].player == pi) {
			if (hard) chan_kill(i); else chan_release(i);
		}
	}
	p->active = 0;
	p->ntracks = 0;
}

static int track_new(int pi, const u8 *base, const u8 *pos) {
	Player *p = &players[pi];
	if (p->ntracks >= 16) return -1;
	for (int i = 0; i < NTRACK; i++) {
		if (!tracks[i].active) {
			track_init(&tracks[i], pi, base, pos);
			p->tracks[p->ntracks++] = i;
			return i;
		}
	}
	return -1;
}

enum { ARG_U8, ARG_S8, ARG_S16, ARG_VLV };
enum { MODE_NORMAL, MODE_RANDOM, MODE_VAR };

static s16 *var_ptr(Player *p, int idx) { return idx < 16 ? &p->var[idx] : &gvar[(idx - 16) & 15]; }

static s32 read_arg(Track *t, int type, int mode) {
	Player *p = &players[t->player];
	if (mode == MODE_RANDOM) {
		s16 lo = rd16(t->pos), hi = rd16(t->pos + 2);
		t->pos += 4;
		if (hi < lo) { s16 x = lo; lo = hi; hi = x; }
		return lo + (s32)(rand_u32() % (u32)(hi - lo + 1));
	}
	if (mode == MODE_VAR) return *var_ptr(p, *t->pos++);
	switch (type) {
	case ARG_U8: return *t->pos++;
	case ARG_S8: return (s8)*t->pos++;
	case ARG_S16: { s16 v = rd16(t->pos); t->pos += 2; return v; }
	default: return vlv(&t->pos);
	}
}

static void track_end(int ti) {
	tracks[ti].active = 0;
	for (int i = 0; i < NCHAN; i++)
		if (chans[i].state != CH_FREE && chans[i].track == ti && chans[i].len < 0) chan_release(i);
}

static void track_tick(int ti) {
	Track *t = &tracks[ti];
	Player *p = &players[t->player];
	if (t->wait > 0 && --t->wait > 0) return;

	int guard = 0;
	while (t->active && t->wait == 0 && guard++ < 512) {
		int mode = MODE_NORMAL, cond = 1;
		u8 cmd = *t->pos++;
		if (cmd == 0xA2) { cond = t->cmp; cmd = *t->pos++; }
		if (cmd == 0xA0) { mode = MODE_RANDOM; cmd = *t->pos++; }
		else if (cmd == 0xA1) { mode = MODE_VAR; cmd = *t->pos++; }

		if (cmd < 0x80) {
			int vel = *t->pos++;
			s32 len = read_arg(t, ARG_VLV, mode);
			if (!cond) continue;
			note_on(ti, cmd, vel, len);
			if (t->notewait) t->wait = len;
			continue;
		}
		switch (cmd) {
		case 0x80: { s32 v = read_arg(t, ARG_VLV, mode); if (cond) t->wait = v; break; }
		case 0x81: { s32 v = read_arg(t, ARG_VLV, mode); if (cond) t->prg = v; break; }
		case 0x93: {
			u8 n = *t->pos++; u32 off = rd24(t->pos); t->pos += 3;
			(void)n;
			if (cond) track_new(t->player, t->base, t->base + off);
			break;
		}
		case 0x94: { u32 off = rd24(t->pos); t->pos += 3; if (cond) t->pos = t->base + off; break; }
		case 0x95: {
			u32 off = rd24(t->pos); t->pos += 3;
			if (cond && t->sp < 3) { t->stack[t->sp++] = t->pos; t->pos = t->base + off; }
			break;
		}
		case 0xB0 ... 0xBD: {
			u8 vi = *t->pos++;
			s32 v = read_arg(t, ARG_S16, mode);
			if (!cond) break;
			s16 *x = var_ptr(p, vi);
			switch (cmd) {
			case 0xB0: *x = v; break;
			case 0xB1: *x += v; break;
			case 0xB2: *x -= v; break;
			case 0xB3: *x *= v; break;
			case 0xB4: if (v) *x /= v; break;
			case 0xB5: *x = v >= 0 ? *x << v : *x >> -v; break;
			case 0xB6: *x = v >= 0 ? (s16)(rand_u32() % (u32)(v + 1)) : -(s16)(rand_u32() % (u32)(1 - v)); break;
			case 0xB8: t->cmp = *x == v; break;
			case 0xB9: t->cmp = *x >= v; break;
			case 0xBA: t->cmp = *x > v; break;
			case 0xBB: t->cmp = *x <= v; break;
			case 0xBC: t->cmp = *x < v; break;
			case 0xBD: t->cmp = *x != v; break;
			}
			break;
		}
		case 0xC0 ... 0xD6: {
			int st = (cmd == 0xC3 || cmd == 0xC4) ? ARG_S8 : ARG_U8;
			s32 v = read_arg(t, st, mode);
			if (!cond) break;
			switch (cmd) {
			case 0xC0: t->pan = v - 64; break;
			case 0xC1: t->vol = v & 127; break;
			case 0xC2: p->mastervol = v & 127; break;
			case 0xC3: t->transpose = v; break;
			case 0xC4: t->bend = v; break;
			case 0xC5: t->bendrange = v; break;
			case 0xC6: t->prio = v; break;
			case 0xC7: t->notewait = v; break;
			case 0xC8: t->tie = v; t->tiechan = -1; break;
			case 0xC9: t->portakey = v + t->transpose; t->porta = 1; break;
			case 0xCA: t->moddepth = v; break;
			case 0xCB: t->modspeed = v; break;
			case 0xCC: t->modtype = v; break;
			case 0xCD: t->modrange = v; break;
			case 0xCE: t->porta = v; break;
			case 0xCF: t->portatime = v; break;
			case 0xD0: t->a = v; break;
			case 0xD1: t->d = v; break;
			case 0xD2: t->s = v; break;
			case 0xD3: t->r = v; break;
			case 0xD4: if (t->sp < 3) { t->stack[t->sp] = t->pos; t->loopcnt[t->sp] = v; t->sp++; } break;
			case 0xD5: t->expr = v & 127; break;
			}
			break;
		}
		case 0xE0: { s32 v = read_arg(t, ARG_S16, mode); if (cond) t->moddelay = v; break; }
		case 0xE1: { s32 v = read_arg(t, ARG_S16, mode); if (cond) p->tempo = v; break; }
		case 0xE3: { s32 v = read_arg(t, ARG_S16, mode); if (cond) t->sweep = v; break; }
		case 0xFC:
			if (!cond || !t->sp) break;
			if (t->loopcnt[t->sp - 1] == 0) t->pos = t->stack[t->sp - 1];	// infinite
			else if (--t->loopcnt[t->sp - 1]) t->pos = t->stack[t->sp - 1];
			else t->sp--;
			break;
		case 0xFD: if (cond && t->sp) t->pos = t->stack[--t->sp]; break;
		case 0xFE: t->pos += 2; break;
		case 0xFF: if (cond) track_end(ti); break;
		default: track_end(ti); break;	// unknown command
		}
	}
}

static void player_tick(int pi) {
	Player *p = &players[pi];
	for (int i = 0; i < NCHAN; i++) {
		Chan *c = &chans[i];
		if (c->state != CH_FREE && c->state != CH_RELEASE && c->player == pi && c->len > 0) {
			if (--c->len == 0) chan_release(i);
		}
	}
	for (int i = 0; i < p->ntracks; i++) {
		int ti = p->tracks[i];
		if (tracks[ti].active) track_tick(ti);
	}
	int alive = 0;
	for (int i = 0; i < p->ntracks; i++) alive |= tracks[p->tracks[i]].active;
	if (!alive) {
		p->active = 0;
		p->ntracks = 0;
	}
}

static void player_start(int pi, const u8 *base, const u8 *start, int bank, int vol, int prio) {
	player_stop(pi, 1);
	Player *p = &players[pi];
	memset(p->var, 0xFF, sizeof(p->var));
	p->vol = vol & 127;
	p->mastervol = 127;
	p->prio = prio;
	p->tempo = 120;
	p->tempocnt = 240;
	load_bank(p, bank);
	if (track_new(pi, base, start) < 0) return;
	p->active = 1;
}

// ---------------------------------------------------------------- commands
static void do_cmd(u32 m) {
	if (!sdat) return;
	int cmd = m >> 28, pi = (m >> 24) & 15, arg = m & 0xFFFFFF;
	switch (cmd) {
	case SND_CMD_PLAY_SEQ: {
		const u8 *si = sdat_info(REC_SEQ, arg);
		if (!si) break;
		const u8 *f = sdat_file(rd16(si));
		const u8 *base = f + rd32(f + 0x18);
		player_start(pi, base, base, rd16(si + 4), si[6], si[8]);
		break;
	}
	case SND_CMD_PLAY_SE: {
		const u8 *ai = sdat_info(REC_SEQARC, arg >> 16);
		if (!ai) break;
		const u8 *f = sdat_file(rd16(ai));
		u32 idx = arg & 0xFFFF;
		if (idx >= rd32(f + 0x1C)) break;
		const u8 *e = f + 0x20 + idx * 12;
		u32 off = rd32(e);
		if (off == 0xFFFFFFFF) break;
		const u8 *base = f + rd32(f + 0x18);
		player_start(pi, base, base + off, rd16(e + 4), e[6], e[8]);
		break;
	}
	case SND_CMD_STOP: player_stop(pi, arg); break;
	case SND_CMD_STOP_ALL: for (int i = 0; i < NPLAYER; i++) player_stop(i, 1); break;
	case SND_CMD_VOLUME: players[pi].mastervol = arg & 127; break;
	}
}

static void sseq_update(void) {
	while (cmdq_r != cmdq_w) {
		do_cmd(cmdq[cmdq_r]);
		cmdq_r = (cmdq_r + 1) % CMDQ;
	}
	for (int pi = 0; pi < NPLAYER; pi++) {
		Player *p = &players[pi];
		if (!p->active) continue;
		while (p->tempocnt >= 240 && p->active) {
			p->tempocnt -= 240;
			player_tick(pi);
		}
		p->tempocnt += p->tempo;
	}
	for (int i = 0; i < NCHAN; i++) chan_update(i);
}

static void fifo_value(u32 v, void *ud) {
	u8 next = (cmdq_w + 1) % CMDQ;
	if (next == cmdq_r) return;	// queue full: dropped
	cmdq[cmdq_w] = v;
	cmdq_w = next;
}

static void fifo_addr(void *a, void *ud) {
	sdat = a;
}

void sseq_init(void) {
	for (int i = 0; i < NCHAN; i++) chan_kill(i);
	for (int i = 0; i < NCHAN; i++) chans[i].track = 0xFF;
	fifoSetValue32Handler(FIFO_SND, fifo_value, NULL);
	fifoSetAddressHandler(FIFO_SND, fifo_addr, NULL);
	// 33.513982 MHz / 64 / 2728 = 191.97 Hz, the same rate as the original driver
	timerStart(1, ClockDivider_64, (u16)-2728, sseq_update);
}
