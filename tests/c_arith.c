#include "LC3.h"

void LC3_main() {
  int a = 100, b = 7;
  int add = a + b;
  int sub = a - b;
  int mul = a * b;
  int div = a / b;
  int rem = a % b;
  int shl = 1 << 4;
  int shr = 128 >> 2;
  int sum = add + sub + mul + div + rem + shl + shr;
  // 107 + 93 + 700 + 14 + 2 + 16 + 32 = 964
  printf("%d", sum);
  return;
}
