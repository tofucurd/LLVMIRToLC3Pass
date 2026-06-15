#include "LC3.h"

int g_counter = 0;
int g_values[4] = {10, 20, 30, 40};

void LC3_main() {
  g_counter = 42;
  int x = g_counter;
  g_values[1] = x + 5;       // 47
  int y = g_values[2];       // 30
  int sum = x + g_values[1] + y;  // 42+47+30 = 119
  printf("%d", sum);
  return;
}
