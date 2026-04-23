/**
 * dom_test.c
 * Dominators microbenchmark.
 *
 * This produces a CFG where:
 *   entry -> while.cond -> while.body -> while.cond (back edge)
 *                       -> while.end
 *
 * Expected output:
 *   while.cond is dominated by entry
 *   while.body is dominated by while.cond
 *   while.end  is dominated by while.cond
 */
int main() {
    int result = 0;
    int n = 1;

    while (n++ < 100) {
        result  = 0;
        result += 2;
        result += 3;
        result *= 4;
        result /= 2;
    }

    return result;
}
