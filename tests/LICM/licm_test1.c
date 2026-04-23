/**
 * licm_test1.c
 * LICM microbenchmark 1: basic hoist.
 *
 * x*2 is loop-invariant (x is a function argument, not modified
 * in the loop).  It should be hoisted to the preheader so it
 * executes only once instead of 10000 times.
 *
 * Expected speedup: ~10000x reduction in mul instructions.
 */
#include <stdio.h>

int test(int x) {
    int p = 0;
    for (int i = 0; i < 10000; i++) {
        p = x * 2;   /* loop-invariant: x never changes */
    }
    return p;
}

int main() {
    int r = test(21);
    printf("result: %d\n", r);
    return 0;
}
