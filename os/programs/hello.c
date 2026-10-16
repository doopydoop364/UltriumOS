#include "ultrium.h"
int main(void) {
    uputs("Hello from Ultrium userspace! pid=");
    uputdec(ultrium_get_pid());
    uputs(" abi=");
    uputhex((uint64_t)ultrium_abi_version());
    uputs("\n");
    return 0;
}
