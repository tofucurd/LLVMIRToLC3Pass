#include "LC3.h"
int main() {
  int st;
  readLabelAddr(&st, "FONT_DATA");
  for (int i = 0; i < 16; i++) {
    for (int j = 0x5002, c; loadAddr(&c, j), c; j++) {
      int t;
      loadAddr(&t, st + c * 16 + i);
      for (int k = 15; k > 7; k--) {
        printCharAddr(t & (1 << k) ? 0x5001 : 0x5000);
      }
    }
    printCharImm('\n');
  }
  return 0;
}