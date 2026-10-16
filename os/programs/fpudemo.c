#include "ultrium.h"
/* Floating-point state must survive preemption and never leak between processes. Each copy sums values
   that depend on its pid in double precision for a while, yielding and being preempted along the way. */
int main(void) {
    double pid = (double)ultrium_get_pid();
    double sum = 0.0;
    for (int i = 1; i <= 200000; ++i) {
        sum += pid * 0.5 + (double)(i & 7);
        if ((i & 0xffff) == 0) (void)ultrium_yield();
    }
    /* sum = 200000 * pid * 0.5 + (sum of i & 7) ; (i & 7) averages 3.5 over 25000 full cycles */
    double expected = 200000.0 * pid * 0.5 + 25000.0 * 28.0;
    if (sum != expected) { uputs("fpudemo: wrong result\n"); return 1; }
    uputs("fpudemo: ok\n");
    return 0;
}
