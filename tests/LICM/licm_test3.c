#include <stdio.h>

int run(int a, int b, int n) {
    int sum = 0;

    for (int i = 0; i < n; i++) {
        int t = a * b;   // invariant, inside loop body

        if (i == 0)
            sum += t;
        else
            sum += i;
    }

    return sum;
}

int main() {
    printf("n=0:  %d\n", run(5, 3, 0));
    printf("n=10: %d\n", run(5, 3, 10));
    return 0;
}
