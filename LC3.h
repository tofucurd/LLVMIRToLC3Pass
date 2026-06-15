// --- standard I/O ---
// printf with compile-time format string.  Supports %d, %s, %c.
int printf(const char *fmt, ...);
// sprintf: like printf but writes into buf (must be large enough).
int sprintf(char *buf, const char *fmt, ...);
// scanf with compile-time format string.  Supports %d, %s, %c.
int scanf(const char *fmt, ...);
// sscanf: like scanf but reads from buf instead of console.
int sscanf(const char *buf, const char *fmt, ...);

// --- legacy helpers (kept for backward compatibility) ---
void printStrAddr(unsigned addr);
void printStr(const char *str);
void printCharAddr(unsigned addr);
void printChar(unsigned c);
void integrateLC3Asm(const char *ins);
int loadLabel(const char *label);
int loadAddr(unsigned addr);
unsigned readLabelAddr(const char *label);
void storeLabel(int src, const char *label);
void storeAddr(int src, unsigned addr);

#ifndef Release

void printInt(unsigned x) {
  if (x > 9)
    printInt(x / 10);
  printChar('0' + x % 10);
}

#define printStrIntStr(spre, x, ssuf)                                          \
  printStr(spre), printInt(x), printStr(ssuf)

#define printStrCharStr(spre, c, ssuf)                                         \
  printStr(spre), printChar(c), printStr(ssuf)

#define printStrAddrStr(spre, a, ssuf)                                         \
  printStr(spre), printStrAddr(a), printStr(ssuf)

#endif