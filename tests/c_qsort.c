#include "LC3.h"

static int arr[8] = {30, 10, 50, 20, 60, 40, 0, 70};

/* swap a[i] and a[j] */
static void swap(int *a, int i, int j) {
  int t = a[i];
  a[i] = a[j];
  a[j] = t;
}

/* partition a[lo..hi]; return pivot index */
static int partition(int *a, int lo, int hi) {
  int pivot = a[hi];
  int i = lo;
  for (int j = lo; j < hi; j++) {
    if (a[j] < pivot) {
      swap(a, i, j);
      i++;
    }
  }
  swap(a, i, hi);
  return i;
}

/* recursive quicksort */
static void qsort(int *a, int lo, int hi) {
  if (lo < hi) {
    int p = partition(a, lo, hi);
    if (p > 0)
      qsort(a, lo, p - 1);
    qsort(a, p + 1, hi);
  }
}

void LC3_main() {
  qsort(arr, 0, 7);
  /* print each element */
  for (int i = 0; i < 8; i++) {
    printf("%d ", arr[i]);
  }
  printf("\n");
  return;
}
