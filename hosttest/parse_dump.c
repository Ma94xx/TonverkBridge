// Zeigt, was der Treiber aus einem Deskriptor-Dump auswählen würde.
//   gcc -O1 -Wall -I../app/src/main/cpp parse_dump.c -o parse_dump
//   ./parse_dump < dump.txt
// Eingabe: Hex-Bytes. Erkannt werden die "RAW ..."-Zeilen aus "Dump USB descriptors"
// der App (beliebiger Präfix davor) oder eine reine Hex-Datei (z. B. von lsusb2bin.py).

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uac_desc.h"

static const char *sync_name(int s) {
    return s == UAC_SYNC_ASYNC ? "async" : s == UAC_SYNC_ADAPTIVE ? "adaptive" : s == UAC_SYNC_SYNC ? "sync" : "none";
}

static void show(const char *dir, const uac_stream *s) {
    if (!s->valid) { printf("%s: -\n", dir); return; }
    printf("%s: IF%d Alt%d EP 0x%02X mps=%d bInterval=%d %s, %dch %d/%dbit, clock=%d",
           dir, s->ifnum, s->alt, s->ep, s->max_pkt, s->interval, sync_name(s->sync),
           s->channels, s->bits, s->subslot * 8, s->clock_id);
    if (s->fb_ep) printf(", feedback EP 0x%02X", s->fb_ep);
    if (s->nrates) { printf(", rates"); for (int i = 0; i < s->nrates; i++) printf(" %d", s->rates[i]); }
    else if (s->rate_max) printf(", rates %d-%d", s->rate_min, s->rate_max);
    printf("\n");
}

int main(void) {
    static uint8_t buf[65536];
    size_t n = 0;
    char line[8192];
    int saw_raw = 0;
    while (fgets(line, sizeof line, stdin)) {
        char *p = strstr(line, "RAW ");
        if (p) { saw_raw = 1; p += 4; }
        else if (saw_raw) continue;
        else p = line;
        if (*p == '#') continue;
        while (*p) {
            while (*p && !isxdigit((unsigned char) *p)) p++;
            if (!isxdigit((unsigned char) p[0]) || !isxdigit((unsigned char) p[1])) break;
            unsigned v; if (sscanf(p, "%2x", &v) != 1) break;
            if (n < sizeof buf) buf[n++] = (uint8_t) v;
            p += 2;
        }
    }
    // Device-Deskriptor (Typ 1) am Anfang überspringen; Parser läuft über Längenfelder.
    uac_dev d;
    int r = uac_parse(buf, n, &d);
    printf("%zu bytes, UAC%d, AudioControl IF%d -> %s\n", n, d.uac, d.ac_if, r == 0 ? "OK" : "NO AUDIO STREAMING");
    show("playback", &d.out);
    show("capture ", &d.in);
    return r == 0 ? 0 : 1;
}
