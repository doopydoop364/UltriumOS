#include "ultrium.h"
/* Blocks inside the sleep system call for a minute. Used to end a process while it is inside the kernel. */
int main(void) { (void)ultrium_sleep(ULTRIUM_MAX_SLEEP_TICKS); return 7; }
