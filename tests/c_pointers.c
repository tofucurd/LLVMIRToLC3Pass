#include "LC3.h"

void set_val(int *p, int v) {
  *p = v;
}

int get_val(int *p) {
  return *p;
}

int LC3_main() {
  int x = 5;
  int *p = &x;
  *p = 10;
  set_val(&x, 99);
  int z = get_val(&x);  // 99
  printf("%d", z);
  return 0;
}
