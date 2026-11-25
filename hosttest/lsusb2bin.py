#!/usr/bin/env python3
"""Rekonstruiert den rohen Config-Deskriptor aus `lsusb -v`-Text (für Parser-Tests).

    python3 -I lsusb2bin.py dump.txt > dump.hex

Jeder Deskriptor-Block ("bLength N" + Felder) wird aus seinen Feldern wieder zu
Bytes zusammengesetzt. Feldbreiten folgen der USB-Namenskonvention (b=1, w=2,
bcd=2, d=4, t=3 Byte, bm*/bma* nach Anzahl Hex-Stellen). Jeder Block wird gegen
sein bLength geprüft; passt die Summe nicht, wird er übersprungen und gemeldet
(der Parser im Treiber überspringt unbekannte Blöcke ohnehin über bLength).
Nur die erste Konfiguration wird ausgegeben.
"""
import re
import sys

FIELD = re.compile(r'^(\s*)([A-Za-z][A-Za-z0-9]*(?:[(\[]\s*\d+[)\]])?)\s+(\S+)(.*)$')
SPECIAL = ('MaxPower', 'BaSourcePin')   # lsusb schreibt "BaSourcePin" groß


def width(name, val):
    base = re.sub(r'[(\[].*$', '', name)
    if base == 'BaSourcePin':
        return 1
    if base in ('bmaControls', 'bmControls', 'bmAttributes', 'bmChannelConfig', 'bmFormats',
                'bmaControl') or base.startswith('bm'):
        if val.lower().startswith('0x'):
            return max(1, (len(val) - 2 + 1) // 2)
        return 1
    if base.startswith('bcd'):
        return 2
    if base.startswith('ba') or base.startswith('b') or base.startswith('i'):
        return 1
    if base.startswith('w') or base.startswith('id'):
        return 2
    if base.startswith('d'):
        return 4
    if base.startswith('t'):
        return 3
    return None


def value(name, val):
    if name.startswith('bcd'):
        major, _, minor = val.partition('.')
        return (int(major, 16) << 8) | int(minor or '0', 16)
    if name == 'MaxPower':
        return 0
    m = re.match(r'^(0x[0-9a-fA-F]+|\d+)', val)
    if not m:
        raise ValueError(val)
    return int(m.group(1), 0)


def blocks(lines):
    """Zerlegt in Blöcke: jede 'bLength'-Zeile beginnt einen neuen Block auf ihrer Einrückung."""
    cur = None
    for ln in lines:
        m = FIELD.match(ln.rstrip('\n'))
        if not m:
            continue
        indent, name, val, _ = m.groups()
        if val.endswith(':') or ln.rstrip().endswith(':'):
            continue   # Überschrift ("Endpoint Descriptor:"), kein Feld
        if not (name[0].islower() or name.startswith(SPECIAL)):
            continue
        if name == 'bLength':
            if cur:
                yield cur
            cur = {'indent': len(indent), 'fields': [(name, val)]}
            continue
        if cur is None:
            continue
        if len(indent) != cur['indent']:
            continue   # tiefer eingerückte Erklärzeilen (z. B. "Transfer Type ...")
        cur['fields'].append((name, val))
    if cur:
        yield cur


def main():
    text = open(sys.argv[1], encoding='utf-8', errors='replace').read().splitlines()
    out = bytearray()
    configs = 0
    skipped = 0
    for blk in blocks(text):
        f = blk['fields']
        try:
            blen = int(f[0][1])
            dtype = int(f[1][1]) if len(f) > 1 and f[1][0] == 'bDescriptorType' else None
        except ValueError:
            skipped += 1
            continue
        if dtype in (1, 6, 7, 0x0F, 0x10):   # Device/Qualifier/BOS: nicht Teil der Config
            continue
        if dtype == 2:       # Configuration: nur die erste
            configs += 1
            if configs > 1:
                break
        if configs == 0:
            continue
        b = bytearray()
        ok = True
        for name, val in f:
            if name == 'MaxPower':
                b.append(0x32)
                continue
            w = width(name, val)
            if w is None:
                ok = False
                break
            try:
                v = value(name, val)
            except ValueError:
                ok = False
                break
            b += v.to_bytes(w, 'little', signed=False) if v >= 0 else v.to_bytes(w, 'little', signed=True)
        if not ok or len(b) != blen:
            skipped += 1
            sys.stderr.write(f'skip: type {dtype} bLength {blen}, rebuilt {len(b)} bytes: '
                             f'{[n for n, _ in f][:6]}\n')
            continue
        out += b
    if len(out) >= 4:
        out[2:4] = len(out).to_bytes(2, 'little')   # wTotalLength auf das Rekonstruierte setzen
    sys.stderr.write(f'{len(out)} bytes, {skipped} block(s) skipped\n')
    for i in range(0, len(out), 32):
        print(' '.join(f'{x:02X}' for x in out[i:i + 32]))


if __name__ == '__main__':
    main()
