#include "ultrium.h"
/* Never exits. Used to demonstrate Ctrl+C on a foreground job and `terminate` on a background one. */
int main(void) {
    for (;;) for (volatile uint64_t i = 0; i < 1000000; ++i) { }
}
