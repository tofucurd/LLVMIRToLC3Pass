#include "LC3.h"

void LC3_main() {
  int arr[5];
  arr[0] = 10;
  arr[3] = 42;
  int x = arr[0] + arr[3];  // 52
  printf("%d", x);
  return;
}
