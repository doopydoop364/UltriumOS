#include "ultrium.h"
/* Reads kernel memory: the CPU faults and only this process is stopped. */
int main(void) {
    uputs("crash: about to read kernel memory\n");
    volatile uint64_t *p = (volatile uint64_t *)0xffffffff80000000ULL;
    return (int)*p;
}
