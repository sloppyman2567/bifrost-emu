/* test_fabs2.c — narrow the fabsf-on-subtraction anomaly.
 *
 * test_fabs showed fabsf works on literals/stored values. The GL test
 * showed fabsf(cv[0]-0.1f) returns garbage (0xfffffaa0) even though the
 * subtraction yields +0.0f. This tests whether the bug is the inline
 * "fabsf(a - b)" call pattern vs a stored intermediate.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

static uint32_t fb(float f){ uint32_t u; memcpy(&u,&f,4); return u; }
static int fails=0;
#define CK(c,m) do{ if(!(c)){printf("FAIL: %s\n",m);fails++;} else printf("ok: %s\n",m);}while(0)

volatile float vsink;

int main(void){
    float a = 0.1f, b = 0.1f;

    /* Pattern A: subtract into a variable, then fabsf the variable. */
    float d = a - b;
    float fa = fabsf(d);
    printf("A: d bits=0x%08x fabsf(d)=0x%08x\n", fb(d), fb(fa));
    CK(fb(fa)==0x00000000u, "A: fabsf(stored 0.0f) == +0.0f");

    /* Pattern B: fabsf(a - b) inline (no intermediate variable). */
    float fbv = fabsf(a - b);
    printf("B: fabsf(a-b)=0x%08x\n", fb(fbv));
    CK(fb(fbv)==0x00000000u, "B: fabsf(a-b inline) == +0.0f");

    /* Ground truth established on host x86 subss (IEEE-754):
     *   0.3f - 0.1f = 0x3e4cccce  (NOT 0x3e4ccccd = literal 0.2f,
     *   because 0.3f/0.1f are inexact and round differently than the
     *   0.2f literal). So we compare against the true subtraction
     *   result, not the 0.2f literal bits.
     */
    uint32_t true_sub;
    { float t = 0.3f - 0.1f; memcpy(&true_sub, &t, 4); }
    /* fabsf just clears the sign, so |0.1f-0.3f| == |0.3f-0.1f| == true_sub. */

    /* Pattern C: subtract non-equal, fabsf inline. */
    float fc = fabsf(0.3f - 0.1f);
    printf("C: fabsf(0.3-0.1)=0x%08x (true_sub=0x%08x)\n", fb(fc), true_sub);
    CK(fb(fc)==true_sub, "C: fabsf(0.3f-0.1f) == true sub result");

    /* Pattern D: fabsf of negative inline subtraction. */
    float fd = fabsf(0.1f - 0.3f);
    printf("D: fabsf(0.1-0.3)=0x%08x\n", fb(fd));
    CK(fb(fd)==true_sub, "D: fabsf(0.1f-0.3f) == |true sub| (sign cleared)");

    /* Pattern E: force via volatile so FABS insn isn't folded.
     * This is the key FABS-sign-bit regression: the input is NEGATIVE
     * (0.1f - 0.3f < 0), so FABS must clear the sign bit. A buggy
     * single-precision FABS (double-width sign mask) leaves it negative.
     */
    volatile float vv = 0.1f - 0.3f;     /* negative */
    float fe = fabsf(vv);
    vsink = fe;
    printf("E: fabsf(volatile 0.1-0.3)=0x%08x (input was negative)\n", fb(fe));
    CK(fb(fe)==true_sub, "E: fabsf(negative volatile) clears sign bit");

    /* Pattern F: dump the literal operand bits + direct subtraction. */
    float f3 = 0.3f, f1 = 0.1f;
    float ff = f3 - f1;
    printf("F: 0.3f bits=0x%08x  0.1f bits=0x%08x  0.3f-0.1f=0x%08x\n",
           fb(f3), fb(f1), fb(ff));
    CK(fb(f3)==0x3e99999au, "F: 0.3f literal bits == 0x3e99999a");
    CK(fb(f1)==0x3dcccccdu, "F: 0.1f literal bits == 0x3dcccccd");
    CK(fb(ff)==true_sub, "F: 0.3f - 0.1f == true IEEE-754 sub result");

    if(fails){printf("test_fabs2: FAIL (%d)\n",fails);return 1;}
    printf("test_fabs2: ALL PASS\n");
    return 0;
}
