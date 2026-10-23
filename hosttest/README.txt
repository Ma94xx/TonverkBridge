Host-Tests für den nativen Treiber (ohne Android):
  gcc -O1 -Wall -I. -I../app/src/main/cpp test_iso_fill.c -o test_iso_fill -lm -lpthread && ./test_iso_fill
  gcc -O1 -Wall -I. -I../app/src/main/cpp test_capture.c  -o test_capture  -lm -lpthread && ./test_capture
