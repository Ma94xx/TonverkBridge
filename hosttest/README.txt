Host-Tests für den nativen Treiber (ohne Android):
  ./run_all.sh

  test_iso_fill    OUT-Paketbefüllung (kumulative usbfs-Offsets) + Sinc-Interpolator
  test_capture     Capture-Ring, ISO-IN-Ernte, Pegel
  test_stress      Langlauf/Überlauf des Capture-Rings
  test_stream_eps  Regression "driver error -4" (kein Submit auf fremde EPs)
  test_generic     generischer UAC1/UAC2-Treiber gegen simulierte Geräte:
                   Clock-/Raten-Requests, Alt-Auswahl, Feedback-Skalierung, 16/24/32 Bit,
                   Mono/Mehrkanal, 44,1-kHz-Resampling, Capture-only, kein Audio-IF
  fixtures/        echte `lsusb -v`-Dumps; lsusb2bin.py rekonstruiert die Rohbytes,
                   parse_dump zeigt die Auswahl des Treibers, expected/ hält das Soll.

Fehlerbericht eines Nutzers prüfen ("Dump USB descriptors" in der App, RAW-Zeilen):
  gcc -O1 -I../app/src/main/cpp parse_dump.c -o parse_dump && ./parse_dump < dump.txt
