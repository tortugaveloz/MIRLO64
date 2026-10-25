/* pad_test.py's host side: n64rt_pad_map() (../n64rt_pad.h) over the lines it is given on stdin
 * ("keys joy set0 set1", hex) -> "button stick_x stick_y" */
#include <stdio.h>
#include "../n64rt_pad.h"

int main(void)
{
    unsigned k, j, s0, s1;
    while (scanf("%x %x %x %x", &k, &j, &s0, &s1) == 4) {
        uint16_t b; int8_t x, y;
        n64rt_pad_map(k, j, s0, s1, &b, &x, &y);
        printf("%04x %d %d\n", b, x, y);
    }
    return 0;
}
