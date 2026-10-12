/* The compiler's 64-bit support routines, as a C compiler of the N64's time
 * expected a library to give them: one line each, written here from what each
 * must compute. tools/gen_llsigs.py compiles this file with the IDO 5.3 C
 * compiler and writes the instruction words of each function to
 * lang/n64rt/n64rt_llsigs.h, where the runtime uses them to recognise the same
 * routines in a game's code. This file is not part of any program. */
unsigned long long __ull_rshift(unsigned long long a, unsigned long long b) { return a >> b; }
unsigned long long __ull_rem(unsigned long long a, unsigned long long b) { return a % b; }
unsigned long long __ull_div(unsigned long long a, unsigned long long b) { return a / b; }
unsigned long long __ll_lshift(unsigned long long a, unsigned long long b) { return a << b; }
long long __ll_rem(unsigned long long a, long long b) { return a % b; }
long long __ll_div(long long a, long long b) { return a / b; }
unsigned long long __ll_mul(unsigned long long a, unsigned long long b) { return a * b; }
void __ull_divremi(unsigned long long *q, unsigned long long *r, unsigned long long a, unsigned short b)
{
    *q = a / b;
    *r = a % b;
}
long long __ll_mod(long long a, long long b)
{
    long long r = a % b;
    if ((r < 0 && b > 0) || (r > 0 && b < 0)) r += b;
    return r;
}
long long __ll_rshift(long long a, long long b) { return a >> b; }
