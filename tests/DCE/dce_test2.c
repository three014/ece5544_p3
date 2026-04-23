/**
 * dce_test2.c
 * DCE microbenchmark 2: transitive faintness.
 *
 * p, q, r are each used only to compute the next one; none of
 * them feed into the return value.  The faint analysis should
 * recognise them all as faint (dead) and remove the chain.
 *
 * Expected: p, q, r, s assignments all removed.
 * Dynamic instruction count decreases.
 */
int helper(int v) {
    return v * 3;
}

int main() {
    int x = 7;

    /* Transitive dead chain — none of these reach a use. */
    int p = x * 2;        /* faint: used only by q */
    int q = p + 1;        /* faint: used only by r */
    int r = q - 3;        /* faint: used only by s */
    int s = r * r;        /* faint: no downstream use */

    /* Only this result is live. */
    int result = helper(x);
    return result;
}
