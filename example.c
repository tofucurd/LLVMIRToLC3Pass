#include "LC3.h"
int f(int a, int b) {
  int res = 1;
  for (int i = a; i <= b; i++) {
    res *= i;
  }
  res *= -2;
  return res;
}
int main() {
  int f27 = f(2, 7);
  return 0;
}