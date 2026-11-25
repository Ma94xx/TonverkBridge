// Generischer Parser für USB-Audio-Class-Deskriptoren (UAC1 + UAC2).
//
// Liest den rohen Config-Deskriptor und liefert pro Richtung (Playback = OUT,
// Capture = IN) das am besten passende AudioStreaming-Alt-Setting samt Format,
// Endpoints, Sync-Art, Feedback-EP und (UAC2) der zuständigen Clock-Source.
// Bewusst ohne libusb-/JNI-Abhängigkeit, damit er auf dem Host testbar ist.

#ifndef UAC_DESC_H
#define UAC_DESC_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define UAC_MAX_RATES    16
#define UAC_MAX_ENTITIES 64

#define UAC_SYNC_NONE     0
#define UAC_SYNC_ASYNC    1
#define UAC_SYNC_ADAPTIVE 2
#define UAC_SYNC_SYNC     3

typedef struct {
    int valid;
    int uac;             // UAC-Version dieses Streams (1/2)
    int ac_if;           // zugehöriges AudioControl-Interface (das letzte davor)
    int ifnum, alt;
    int ep;              // ISO-Daten-Endpoint (Bit 7 = Richtung)
    int max_pkt;         // effektive Bytes pro Service-Intervall (inkl. HS-High-Bandwidth)
    int interval;        // bInterval (roh)
    int sync;            // UAC_SYNC_*
    int fb_ep;           // expliziter Feedback-EP (0 = keiner)
    int fb_max_pkt;
    int channels;        // Kanäle im Stream
    int subslot;         // Bytes pro Sample-Slot (2, 3, 4)
    int bits;            // gültige Bits (MSB-bündig im Slot)
    int terminal;        // bTerminalLink
    int clock_id;        // UAC2: aufgelöste Clock-Source (0 = unbekannt)
    int nrates;          // UAC1: diskrete Raten aus FORMAT_TYPE ...
    int rates[UAC_MAX_RATES];
    int rate_min, rate_max;   // ... bzw. kontinuierlicher Bereich (nrates == 0)
} uac_stream;

typedef struct {
    int uac;             // 1 oder 2 (0 = kein Audio-Gerät) — vom gewählten Stream
    int ac_if;           // AudioControl-Interface der gewählten Audio-Funktion
    uac_stream out, in;
} uac_dev;

// ---- intern ----

typedef struct {
    int ac;              // AudioControl-Interface, zu dem die Entity gehört
    int id, type;        // type: 0x02 IT, 0x03 OT, 0x0A Clock-Source, 0x0B Selector, 0x0C Multiplier
    int src;             // IT/OT/Multiplier: bCSourceID; Selector: erster Pin
} uac_entity;

typedef struct {
    uac_stream s;
    int has_out, has_in;      // Daten-EPs je Richtung in diesem Alt-Setting
    int pcm;                  // Format ist PCM (Typ I)
    int in_ep, in_max_pkt, in_interval, in_sync;
    int in_attr_usage;        // Usage-Bits des IN-EPs (1 = Feedback)
    int uac1_synch_addr;      // UAC1: bSynchAddress des OUT-EPs
} uac_cand;

static inline int uac_le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline int uac_le24(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16); }

// wMaxPacketSize: Bits 0-10 Größe, Bits 11-12 zusätzliche Transaktionen (HS).
static inline int uac_eff_mps(int w) { return (w & 0x7ff) * (1 + ((w >> 11) & 3)); }

// Bewertung eines Kandidaten (höher = besser).
static inline int uac_score(const uac_stream *s, int want_out) {
    int sc = 0;
    switch (s->bits) {          // 24 Bit bevorzugt, dann 32, dann 16
    case 24: sc += 300; break;
    case 32: sc += 200; break;
    case 16: sc += 100; break;
    default: sc += 50; break;
    }
    if (want_out) {
        // Playback: genau Stereo ideal, sonst wenig Kanäle, Mono zuletzt.
        if (s->channels == 2) sc += 5000;
        else if (s->channels > 2) sc += 4000 - s->channels;
        else sc += 1000;
    } else {
        sc += 100 * s->channels;  // Capture: alle Kanäle mitnehmen
    }
    // UAC1: 48 kHz unterstützt?
    if (s->nrates > 0) {
        for (int i = 0; i < s->nrates; i++) if (s->rates[i] == 48000) { sc += 20000; break; }
    } else if (s->rate_max > 0) {
        if (s->rate_min <= 48000 && s->rate_max >= 48000) sc += 20000;
    } else {
        sc += 20000;              // UAC2: Raten stehen nicht im Deskriptor
    }
    return sc;
}

static inline void uac_commit(uac_dev *dev, uac_cand *c, int *best_out, int *best_in) {
    if (!c->pcm || c->s.subslot < 2 || c->s.subslot > 4 || c->s.channels < 1) return;
    if (c->has_out) {
        uac_stream s = c->s;
        // Jeder IN-EP neben einem OUT-Daten-EP ist dessen Feedback-EP
        // (UAC2: Usage=Feedback; UAC1: bSynchAddress, Usage-Bits oft 0).
        if (c->in_ep) { s.fb_ep = c->in_ep; s.fb_max_pkt = c->in_max_pkt; }
        int sc = uac_score(&s, 1);
        if (sc > *best_out) { *best_out = sc; dev->out = s; dev->out.valid = 1; }
    } else if (c->has_in && c->in_attr_usage != 1) {
        uac_stream s = c->s;
        s.ep = c->in_ep; s.max_pkt = c->in_max_pkt; s.interval = c->in_interval; s.sync = c->in_sync;
        int sc = uac_score(&s, 0);
        if (sc > *best_in) { *best_in = sc; dev->in = s; dev->in.valid = 1; }
    }
}

static inline int uac_resolve_clock(const uac_entity *e, int ne, int ac, int id) {
    for (int depth = 0; depth < 8 && id > 0; depth++) {
        int k = -1;
        for (int i = 0; i < ne; i++) if (e[i].ac == ac && e[i].id == id) { k = i; break; }
        if (k < 0) return 0;
        if (e[k].type == 0x0A) return id;                 // Clock-Source
        if (e[k].type == 0x0B || e[k].type == 0x0C || e[k].type == 0x02 || e[k].type == 0x03)
            id = e[k].src;                                // Selector (Pin 1) / Multiplier / Terminal
        else return 0;
    }
    return 0;
}

// Liefert 0, wenn mindestens eine Richtung gefunden wurde.
static inline int uac_parse(const uint8_t *d, size_t len, uac_dev *dev) {
    memset(dev, 0, sizeof *dev);
    dev->ac_if = -1;
    uac_entity ent[UAC_MAX_ENTITIES]; int ne = 0;
    uac_cand c; memset(&c, 0, sizeof c);
    int in_cand = 0;              // c gehört zu einem AS-Alt-Setting
    int cur_cls = 0, cur_sub = 0, cur_if = -1;
    // Ein Gerät kann mehrere Audio-Funktionen haben (z. B. Elektron: UAC1-AudioControl
    // für MIDI vor der UAC2-Audio-Funktion). Jedes AS-Interface gehört zum letzten
    // AudioControl-Interface davor; die UAC-Version kommt aus dem AS-Interface selbst.
    int cur_ac_if = -1, cur_ac_uac = 0;
    int best_out = -1, best_in = -1;

    size_t i = 0;
    while (i + 1 < len) {
        int l = d[i], t = d[i + 1];
        if (l < 2 || i + (size_t) l > len) break;
        const uint8_t *p = d + i;

        if (t == 0x04 && l >= 9) {                        // Interface
            if (in_cand) { uac_commit(dev, &c, &best_out, &best_in); in_cand = 0; }
            cur_if = p[2]; cur_cls = p[5]; cur_sub = p[6];
            if (cur_cls == 0x01 && cur_sub == 0x01) {
                cur_ac_if = cur_if;
                cur_ac_uac = (p[7] == 0x20) ? 2 : 1;
            }
            if (cur_cls == 0x01 && cur_sub == 0x02 && p[4] > 0) {
                memset(&c, 0, sizeof c);
                c.s.ifnum = cur_if; c.s.alt = p[3];
                c.s.ac_if = cur_ac_if;
                c.s.uac = (p[7] == 0x20) ? 2 : (cur_ac_uac ? cur_ac_uac : 1);
                in_cand = 1;
            }
        } else if (t == 0x24 && cur_cls == 0x01 && cur_sub == 0x01) {   // CS AudioControl
            int st = l >= 3 ? p[2] : 0;
            if (ne < UAC_MAX_ENTITIES && l >= 4) {
                uac_entity *e = &ent[ne];
                e->ac = cur_ac_if; e->id = p[3]; e->type = st; e->src = 0;
                int ok = 1;
                if (cur_ac_uac == 2) {
                    if (st == 0x02 && l >= 8) e->src = p[7];          // IT.bCSourceID
                    else if (st == 0x03 && l >= 9) e->src = p[8];     // OT.bCSourceID
                    else if (st == 0x0B && l >= 6) e->src = p[5];     // Selector: Pin 1
                    else if (st == 0x0C && l >= 5) e->src = p[4];     // Multiplier
                    else if (st != 0x0A) ok = 0;
                } else ok = 0;
                if (ok) ne++;
            }
        } else if (t == 0x24 && in_cand) {                // CS AudioStreaming
            int st = l >= 3 ? p[2] : 0;
            if (st == 0x01) {                              // AS_GENERAL
                c.s.terminal = l >= 4 ? p[3] : 0;
                if (c.s.uac == 2 && l >= 16) {
                    c.pcm = (p[5] == 0x01) && (p[6] & 0x01);  // Format-Typ I, PCM-Bit
                    c.s.channels = p[10];
                } else if (l >= 7) {
                    c.pcm = (uac_le16(p + 5) == 0x0001);   // wFormatTag PCM
                }
            } else if (st == 0x02 && l >= 4 && p[3] == 0x01) {    // FORMAT_TYPE I
                if (c.s.uac == 2 && l >= 6) {
                    c.s.subslot = p[4]; c.s.bits = p[5];
                } else if (l >= 8) {
                    c.s.channels = p[4]; c.s.subslot = p[5]; c.s.bits = p[6];
                    int nf = p[7];
                    if (nf == 0 && l >= 14) {
                        c.s.rate_min = uac_le24(p + 8); c.s.rate_max = uac_le24(p + 11);
                    } else {
                        for (int k = 0; k < nf && k < UAC_MAX_RATES && 8 + 3 * k + 3 <= l; k++)
                            c.s.rates[c.s.nrates++] = uac_le24(p + 8 + 3 * k);
                    }
                }
            }
        } else if (t == 0x05 && in_cand && l >= 7) {      // Endpoint
            int addr = p[2], attr = p[3];
            if ((attr & 0x03) == 0x01) {                   // isochron
                int mps = uac_eff_mps(uac_le16(p + 4));
                if (!(addr & 0x80)) {
                    if (!c.has_out) {
                        c.has_out = 1; c.s.ep = addr; c.s.max_pkt = mps;
                        c.s.interval = p[6]; c.s.sync = (attr >> 2) & 0x03;
                        if (l >= 9) c.uac1_synch_addr = p[8];
                    }
                } else if (!c.in_ep) {
                    c.has_in = 1; c.in_ep = addr; c.in_max_pkt = mps;
                    c.in_interval = p[6]; c.in_sync = (attr >> 2) & 0x03;
                    c.in_attr_usage = (attr >> 4) & 0x03;
                }
            }
        }
        i += (size_t) l;
    }
    if (in_cand) uac_commit(dev, &c, &best_out, &best_in);

    if (dev->out.valid && dev->out.uac == 2)
        dev->out.clock_id = uac_resolve_clock(ent, ne, dev->out.ac_if, dev->out.terminal);
    if (dev->in.valid && dev->in.uac == 2)
        dev->in.clock_id = uac_resolve_clock(ent, ne, dev->in.ac_if, dev->in.terminal);
    const uac_stream *main = dev->out.valid ? &dev->out : dev->in.valid ? &dev->in : NULL;
    dev->uac = main ? main->uac : 0;
    dev->ac_if = main ? main->ac_if : -1;
    return main ? 0 : -1;
}

// Wählt aus den UAC1-Ratenangaben die zu `want` passendste Rate.
static inline int uac1_pick_rate(const uac_stream *s, int want) {
    if (s->nrates > 0) {
        int best = s->rates[0];
        for (int i = 0; i < s->nrates; i++) {
            if (s->rates[i] == want) return want;
            int db = best - want, di = s->rates[i] - want;
            if (db < 0) db = -db;
            if (di < 0) di = -di;
            if (di < db) best = s->rates[i];
        }
        return best;
    }
    if (s->rate_max > 0) {
        if (want < s->rate_min) return s->rate_min;
        if (want > s->rate_max) return s->rate_max;
    }
    return want;
}

// Dauer eines Service-Intervalls in µs (FS: Frames à 1 ms, HS/SS: Microframes à 125 µs).
static inline int uac_period_us(int interval, int high_speed) {
    if (interval < 1) interval = 1;
    if (interval > 16) interval = 16;
    if (!high_speed) return 1000 << (interval - 1 > 4 ? 4 : interval - 1);   // FS-ISO: bInterval 1..4 üblich
    return 125 << (interval - 1);
}

#endif
