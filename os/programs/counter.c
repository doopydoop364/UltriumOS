#include "ultrium.h"
/* CPU-bound steps with no system call inside them: when several copies run, the timer interrupts each
   one mid-step, so their output interleaves. Each line shows the tick count at that moment. */
int main(void) {
    int64_t pid = ultrium_get_pid();
    for (int step = 1; step <= 5; ++step) {
        for (volatile uint64_t i = 0; i < 6000000; ++i) { }
        uputs("counter pid="); uputdec(pid);
        uputs(" step="); uputdec(step);
        uputs(" tick="); uputdec((int64_t)ultrium_ticks());
        uputs("\n");
    }
    return 0;
}
