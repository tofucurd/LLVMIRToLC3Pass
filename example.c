#include "LC3.h"
int f(int a) {
  return a > 0 ? f(a - 1) + a : 0;
}
int main() {
  int x = 7;
  int y = f(x);
  return 0;
}