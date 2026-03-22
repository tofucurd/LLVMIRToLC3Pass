#include "LC3.h"
int f(unsigned a, unsigned b) {
  unsigned c = a / b;
  unsigned d = a % b;
  unsigned e = a >> b;
  int x = a * (-b);
  return x;
}
int foo(int x) {
  return x > 0 ? foo(x - 1) * x : 1;
}
void LC3_main() {
  int x = f(7, 2);
  int y = foo(7);
  printStrIntStr("-x:", -x, "\n");
  printStrIntStr("y:", y, "\n");
  return;
}