/**
 * dce_test1.c
 * DCE microbenchmark 1.
 *
 * 'a', 'b', and intermediate values are computed but 'result'
 * (the only truly used value) comes only from x.  All the
 * intermediate arithmetic involving a and b is dead.
 *
 * Expected: the dead add/sub/mul/div instructions are removed.
 * Dynamic instruction count decreases significantly.
 */
int main(int argc, const char *argv[]) {
    int x = 1;
    int a = x + 50;   /* dead — a never used in output */
    int b = a + 96;   /* dead */
    int c;
    int d;

    if (a > 50) {
        c = a - 50;   /* dead */
        d = a * 96;   /* dead */
    } else {
        c = a + 50;   /* dead */
        d = a * 96;   /* dead */
    }

    /* e and f are also dead because their result is never printed
       or returned.  Only x's value matters. */
    int e = 50 - 96;  /* dead */
    int f = e + c;    /* dead */

    return x;         /* only x is live */
}
