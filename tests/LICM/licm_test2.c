/**
 * licm_test2.c
 * LICM microbenchmark 2: nested loops.
 *
 * The expression (a * b) is invariant with respect to BOTH the
 * inner loop and the outer loop.  It should bubble all the way
 * out to the outermost preheader and execute exactly once.
 *
 * Expected: (a*b) executed 1 time, not outer*inner = 200*300 times.
 */
#include <stdio.h>

int compute(int a, int b) {
    int sum = 0;
    for (int i = 0; i < 200; i++) {
        for (int j = 0; j < 300; j++) {
            int inv = a * b;   /* invariant w.r.t. both loops */
            sum += inv + i + j;
        }
    }
    return sum;
}

int main() {
    printf("result: %d\n", compute(3, 4));
    return 0;
}
