#include "LC3.h"

int factorial(int n) {
  if (n <= 1)
    return 1;
  return n * factorial(n - 1);
}

void LC3_main() {
  int x = 0;
  if (x > 0) {
    x = 1;
  } else {
    x = 2;
  }
  for (int i = 0; i < 5; i++) {
    x = x + i;  // 2+0+1+2+3+4 = 12
  }
  int f = factorial(5);  // 120
  printf("%d", f);
  return;
}
