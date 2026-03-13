/**
 * GTE macros
 * This file is meant to be used as an alternative to DMPSX
 *
 * This file includes volatile variants and register clobber list which may affect matching
 */


/*  RTPS    15      0x4A180001  Perspective transform */
#define gte_nRTPS() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4A180001": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_RTPS() { \
        __asm__ volatile (".word 0x4A180001": : :"$12","$13","$14","$15","memory"); \
    }

/*  RTPT    23      0x4A280030  Perspective transform on 3 points */
#define gte_nRTPT() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4A280030": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_RTPT() { \
        __asm__ volatile (".word 0x4A280030": : :"$12","$13","$14","$15","memory"); \
    }

/*  DPCL    8       0x4A680029  Depth Cue Color light */
#define gte_nDPCL() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4A680029": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_DPCL() { \
        __asm__ volatile (".word 0x4A680029": : :"$12","$13","$14","$15","memory"); \
    }

/*  DPCS    8       0x4A780010  Depth Cueing */
#define gte_nDPCS() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4A780010": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_DPCS() { \
        __asm__ volatile (".word 0x4A780010": : :"$12","$13","$14","$15","memory"); \
    }

/*  DPCT    17      0x4AF8002A  Depth cue color RGB0,RGB1,RGB2 */
#define gte_nDPCT() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4AF8002A": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_DPCT() { \
        __asm__ volatile (".word 0x4AF8002A": : :"$12","$13","$14","$15","memory"); \
    }

/*  INTPL   8       0x4A980011  Interpolation of vector and far color */
#define gte_nINTPL() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4A980011": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_INTPL() { \
        __asm__ volatile (".word 0x4A980011": : :"$12","$13","$14","$15","memory"); \
    }

/*  NCS     14      0x4AC8041E  Normal color v0 */
#define gte_nNCS() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4AC8041E": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_NCS() { \
        __asm__ volatile (".word 0x4AC8041E": : :"$12","$13","$14","$15","memory"); \
    }

/*  NCT     30      0x4AD80420  Normal color v0, v1, v2 */
#define gte_nNCT() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4AD80420": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_NCT() { \
        __asm__ volatile (".word 0x4AD80420": : :"$12","$13","$14","$15","memory"); \
    }

/*  NCDS    19      0x4AE80413  Normal color depth cuev0 */
#define gte_nNCDS() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4AE80413": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_NCDS() { \
        __asm__ volatile (".word 0x4AE80413": : :"$12","$13","$14","$15","memory"); \
    }

/*  NCDT    44      0x4AF80416  Normal color depth cue v0, v1, v2 */
#define gte_nNCDT() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4AF80416": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_NCDT() { \
        __asm__ volatile (".word 0x4AF80416": : :"$12","$13","$14","$15","memory"); \
    }

/*  NCCS    17      0x4B08041B  Normal color col. v0 */
#define gte_nNCCS() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4B08041B": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_NCCS() { \
        __asm__ volatile (".word 0x4B08041B": : :"$12","$13","$14","$15","memory"); \
    }

/*  NCCT    39      0x4B18043F  Normal color col.v0, v1, v2 */
#define gte_nNCCT() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4B18043F": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_NCCT() { \
        __asm__ volatile (".word 0x4B18043F": : :"$12","$13","$14","$15","memory"); \
    }

/*  CDP     13      0x4B280414  Color Depth Queue */
#define gte_nCDP() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4B280414": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_CDP() { \
        __asm__ volatile (".word 0x4B280414": : :"$12","$13","$14","$15","memory"); \
    }

/*  CC      11      0x4B38041C  Color Col. */
#define gte_CC() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4B38041C": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_CC() { \
        __asm__ volatile (".word 0x4B38041C": : :"$12","$13","$14","$15","memory"); \
    }

/*  NCLIP   8       0x4B400006  Normal clipping */
#define gte_nNCLIP() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4B400006": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_NCLIP() { \
        __asm__ volatile (".word 0x4B400006": : :"$12","$13","$14","$15","memory"); \
    }

/*  AVSZ3   5       0x4B58002D  Average of three Z values */
#define gte_nAVSZ3() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4B58002D": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_AVSZ3() { \
        __asm__ volatile (".word 0x4B58002D": : :"$12","$13","$14","$15","memory"); \
    }

/*  AVSZ4   6       0x4B68002E  Average of four Z values */
#define gte_nAVSZ4() { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word 0x4B68002E": : :"$12","$13","$14","$15","memory"); \
    }

#define gte_AVSZ4() { \
        __asm__ volatile (".word 0x4B68002E": : :"$12","$13","$14","$15","memory"); \
    }

/**
 * Instructions which take an argument
 *
 * sf : arg is 1 bit wide
 * mx : arg is 2 bit wide
 * v  : arg is 2 bit wide
 * cv : arg is 2 bit wide
 * lm : arg is 1 bit wide
 */

/*  MVMVA   8       0x4A400012  Multiply vector by matrix and vector addition. */
#define gte_nMVMVA(sf, mx, v, cv, lm) { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word %0": :"g"(0x4A400012 | ((sf) & 0x1) << 19 | ((mx) & 0x3) << 17 | ((v) & 0x3) << 15 | ((cv) & 0x3) << 13 | ((lm) & 0x1) << 10) :"$12","$13","$14","$15","memory") \
    }

#define gte_MVMVA(sf, mx, v, cv, lm) { \
        __asm__ volatile (".word %0": :"g"(0x4A400012 | ((sf) & 0x1) << 19 | ((mx) & 0x3) << 17 | ((v) & 0x3) << 15 | ((cv) & 0x3) << 13 | ((lm) & 0x1) << 10) :"$12","$13","$14","$15","memory") \
    }

/*  SQR     5       0x4AA00428  Square of vector */
#define gte_nSQR(sf) { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word %0": :"g"(0x4AA00428 | ((sf) & 0x1) << 19):"$12","$13","$14","$15","memory") \
    }

#define gte_SQR(sf) { \
        __asm__ volatile (".word %0": :"g"(0x4AA00428 | ((sf) & 0x1) << 19):"$12","$13","$14","$15","memory") \
    }

/*  OP      6       0x4B70000C  Outer Product */
#define gte_nOP(sf) { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word %0": :"g"(0x4B70000C | ((sf) & 0x1) << 19):"$12","$13","$14","$15","memory") \
    }

#define gte_OP(sf) { \
        __asm__ volatile (".word %0": :"g"(0x4B70000C | ((sf) & 0x1) << 19):"$12","$13","$14","$15","memory") \
    }

/*  GPF     6       0x4B90003D  General purpose interpolation */
#define gte_nGPF(sf) { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word %0": :"g"(0x4B90003D | ((sf) & 0x1) << 19):"$12","$13","$14","$15","memory") \
    }

#define gte_GPF(sf) { \
        __asm__ volatile (".word %0": :"g"(0x4B90003D | ((sf) & 0x1) << 19):"$12","$13","$14","$15","memory") \
    }

/*  GPL     5       0x4BA0003E  general purpose interpolation */
#define gte_nGPL(sf) { \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile ("nop             ": : :"$12","$13","$14","$15","memory"); \
        __asm__ volatile (".word %0": :"g"(0x4BA0003E | ((sf) & 0x1) << 19):"$12","$13","$14","$15","memory") \
    }

#define gte_GPL(sf) { \
        __asm__ volatile (".word %0": :"g"(0x4BA0003E | ((sf) & 0x1) << 19):"$12","$13","$14","$15","memory") \
    }


/**
 * Convenience macros
 */

/*  rtv0    -       0x4A486012  v0 * rotmatrix */
#define gte_nrtv0() gte_nMVMVA(1, 0, 0, 3, 0)

#define gte_rtv0() gte_MVMVA(1, 0, 0, 3, 0)

/*  rtv1    -       0x4A48E012  v1 * rotmatrix */
#define gte_nrtv1() gte_nMVMVA(1, 0, 1, 3, 0)

#define gte_rtv1() gte_MVMVA(1, 0, 1, 3, 0)

/*  rtv2    -       0x4A496012  v2 * rotmatrix */
#define gte_nrtv2() gte_nMVMVA(1, 0, 2, 3, 0)

#define gte_rtv2() gte_MVMVA(1, 0, 2, 3, 0)

/*  rtir12  -       0x4A49E012  ir * rotmatrix */
#define gte_nrtir12() gte_nMVMVA(1, 0, 3, 3, 0)

#define gte_rtir12() gte_MVMVA(1, 0, 3, 3, 0)

/*  rtir0   -       0x4A41E012  ir * rotmatrix */
#define gte_nrtir0() gte_nMVMVA(0, 0, 3, 3, 0)

#define gte_rtir0() gte_MVMVA(0, 0, 3, 3, 0)

/*  rtv0tr  -       0x4A480012  v0 * rotmatrix + tr vector */
#define gte_nrtv0tr() gte_nMVMVA(1, 0, 0, 0, 0)

#define gte_rtv0tr() gte_MVMVA(1, 0, 0, 0, 0)

/*  rtv1tr  -       0x4A488012  v1 * rotmatrix + tr vector */
#define gte_nrtv1tr() gte_nMVMVA(1, 0, 1, 0, 0)

#define gte_rtv1tr() gte_MVMVA(1, 0, 1, 0, 0)

/*  rtv2tr  -       0x4A490012  v2 * rotmatrix + tr vector */
#define gte_nrtv2tr() gte_nMVMVA(1, 0, 2, 0, 0)

#define gte_rtv2tr() gte_MVMVA(1, 0, 2, 0, 0)

/*  rtirtr  -       0x4A498012  ir * rotmatrix + tr vector */
#define gte_nrtirtr() gte_nMVMVA(1, 0, 3, 0, 0)

#define gte_rtirtr() gte_MVMVA(1, 0, 3, 0, 0)

/*  rtv0bk  -       0x4A482012  v0 * rotmatrix + bk vector */
#define gte_nrtv0bk() gte_nMVMVA(1, 0, 0, 1, 0)

#define gte_rtv0bk() gte_MVMVA(1, 0, 0, 1, 0)

/*  rtv1bk  -       0x4A48A012  v1 * rotmatrix + bk vector */
#define gte_nrtv1bk() gte_nMVMVA(1, 0, 1, 1, 0)

#define gte_rtv1bk() gte_MVMVA(1, 0, 1, 1, 0)

/*  rtv2bk  -       0x4A492012  v2 * rotmatrix + bk vector */
#define gte_nrtv2bk() gte_nMVMVA(1, 0, 2, 1, 0)

#define gte_rtv2bk() gte_MVMVA(1, 0, 2, 1, 0)

/*  rtirbk  -       0x4A49A012  ir * rotmatrix + bk vector */
#define gte_nrtirbk() gte_nMVMVA(1, 0, 3, 1, 0)

#define gte_rtirbk() gte_MVMVA(1, 0, 3, 1, 0)

/*  ll      -       0x4A4A6412  v0 * light matrix. Lower limit result to 0 */
#define gte_nll() gte_nMVMVA(1, 1, 0, 3, 1)

#define gte_ll() gte_MVMVA(1, 1, 0, 3, 1)

/*  llv0    -       0x4A4A6012  v0 * light matrix */
#define gte_nllv0() gte_nMVMVA(1, 1, 0, 3, 0)

#define gte_llv0() gte_MVMVA(1, 1, 0, 3, 0)

/*  llv1    -       0x4A4AE012  v1 * light matrix */
#define gte_nllv1() gte_nMVMVA(1, 1, 1, 3, 0)

#define gte_llv1() gte_MVMVA(1, 1, 1, 3, 0)

/*  llv2    -       0x4A4B6012  v2 * light matrix */
#define gte_nllv2() gte_nMVMVA(1, 1, 2, 3, 0)

#define gte_llv2() gte_MVMVA(1, 1, 2, 3, 0)

/*  llvir   -       0x4A4BE012  ir * light matrix */
#define gte_nllvir() gte_nMVMVA(1, 1, 3, 3, 0)

#define gte_llvir() gte_MVMVA(1, 1, 3, 3, 0)

/*  llv0tr  -       0x4A4A0012  v0 * light matrix + tr vector */
#define gte_nllv0tr() gte_nMVMVA(1, 1, 0, 0, 0)

#define gte_llv0tr() gte_MVMVA(1, 1, 0, 0, 0)

/*  llv1tr  -       0x4A4A8012  v1 * light matrix + tr vector */
#define gte_nllv1tr() gte_nMVMVA(1, 1, 1, 0, 0)

#define gte_llv1tr() gte_MVMVA(1, 1, 1, 0, 0)

/*  llv2tr  -       0x4A4B0012  v2 * light matrix + tr vector */
#define gte_nllv2tr() gte_nMVMVA(1, 1, 2, 0, 0)

#define gte_llv2tr() gte_MVMVA(1, 1, 2, 0, 0)

/*  llirtr  -       0x4A4B8012  ir * light matrix + tr vector */
#define gte_nllirtr() gte_nMVMVA(1, 1, 3, 0, 0)

#define gte_llirtr() gte_MVMVA(1, 1, 3, 0, 0)

/*  llv0bk  -       0x4A4A2012  v0 * light matrix + bk vector */
#define gte_nllv0bk() gte_nMVMVA(1, 1, 0, 1, 0)

#define gte_llv0bk() gte_MVMVA(1, 1, 0, 1, 0)

/*  llv1bk  -       0x4A4AA012  v1 * light matrix + bk vector */
#define gte_nllv1bk() gte_nMVMVA(1, 1, 1, 1, 0)

#define gte_llv1bk() gte_MVMVA(1, 1, 1, 1, 0)

/*  llv2bk  -       0x4A4B2012  v2 * light matrix + bk vector */
#define gte_nllv2bk() gte_nMVMVA(1, 1, 2, 1, 0)

#define gte_llv2bk() gte_MVMVA(1, 1, 2, 1, 0)

/*  llirbk  -       0x4A4BA012  ir * light matrix + bk vector */
#define gte_nllirbk() gte_nMVMVA(1, 1, 3, 1, 0)

#define gte_llirbk() gte_MVMVA(1, 1, 3, 1, 0)

/*  lc      -       0x4A4DA412  v0 * color matrix, Lower limit clamped to 0 */
#define gte_nlc() gte_nMVMVA(1, 2, 3, 1, 1)

#define gte_lc() gte_MVMVA(1, 2, 3, 1, 1)

/*  lcv0    -       0x4A4C6012  v0 * color matrix */
#define gte_nlcv0() gte_nMVMVA(1, 2, 0, 3, 0)

#define gte_lcv0() gte_MVMVA(1, 2, 0, 3, 0)

/*  lcv1    -       0x4A4CE012  v1 * color matrix */
#define gte_nlcv1() gte_nMVMVA(1, 2, 1, 3, 0)

#define gte_lcv1() gte_MVMVA(1, 2, 1, 3, 0)

/*  lcv2    -       0x4A4D6012  v2 * color matrix */
#define gte_nlcv2() gte_nMVMVA(1, 2, 2, 3, 0)

#define gte_lcv2() gte_MVMVA(1, 2, 2, 3, 0)

/*  lcvir   -       0x4A4DE012  ir * color matrix */
#define gte_nlcvir() gte_nMVMVA(1, 2, 3, 3, 0)

#define gte_lcvir() gte_MVMVA(1, 2, 3, 3, 0)

/*  lcv0tr  -       0x4A4C0012  v0 * color matrix + tr vector */
#define gte_nlcv0tr() gte_nMVMVA(1, 2, 0, 0, 0)

#define gte_lcv0tr() gte_MVMVA(1, 2, 0, 0, 0)

/*  lcv1tr  -       0x4A4C8012  v1 * color matrix + tr vector */
#define gte_nlcv1tr() gte_nMVMVA(1, 2, 1, 0, 0)

#define gte_lcv1tr() gte_MVMVA(1, 2, 1, 0, 0)

/*  lcv2tr  -       0x4A4D0012  v2 * color matrix + tr vector */
#define gte_nlcv2tr() gte_nMVMVA(1, 2, 2, 0, 0)

#define gte_lcv2tr() gte_MVMVA(1, 2, 2, 0, 0)

/*  lcirtr  -       0x4A4D8012  ir * color matrix + tr vector */
#define gte_nlcirtr() gte_nMVMVA(1, 2, 3, 0, 0)

#define gte_lcirtr() gte_MVMVA(1, 2, 3, 0, 0)

/*  lev0bk  -       0x4A4C2012  v0 * color matrix + bk vector */
#define gte_nlev0bk() gte_nMVMVA(1, 2, 0, 1, 0)

#define gte_lev0bk() gte_MVMVA(1, 2, 0, 1, 0)

/*  lev1bk  -       0x4A4CA012  v1 * color matrix + bk vector */
#define gte_nlev1bk() gte_nMVMVA(1, 2, 1, 1, 0)

#define gte_lev1bk() gte_MVMVA(1, 2, 1, 1, 0)

/*  lev2bk  -       0x4A4D2012  v2 * color matrix + bk vector */
#define gte_nlev2bk() gte_nMVMVA(1, 2, 2, 1, 0)

#define gte_lev2bk() gte_MVMVA(1, 2, 2, 1, 0)

/*  leirbk  -       0x4A4DA012  ir * color matrix + bk vector */
#define gte_nleirbk() gte_nMVMVA(1, 2, 3, 1, 0)

#define gte_leirbk() gte_MVMVA(1, 2, 3, 1, 0)

/*  sqr12   -       0x4AA80428  square of ir    1,19,12 */
// #define gte_nsqr12() gte_nSQR(1)
// 
// #define gte_sqr12() gte_SQR(1)

/*  sqr0    -       0x4AA80428  square of ir    1,31, 0 */
// #define gte_nsqr0() gte_nSQR(1)
// 
// #define gte_sqr0() gte_SQR(1)

/*  op12    -       0x4B78000C  outer product   1,19,12 */
#define gte_nop12() gte_nOP(1)

#define gte_op12() gte_OP(1)

/*  op0     -       0x4B70000C  outer product   1,31, 0 */
#define gte_nop0() gte_nOP(0)

#define gte_op0() gte_OP(0)

/*  gpf12   -       0x4B98003D  general purpose interpolation   1,19,12 */
#define gte_ngpf12() gte_nGPF(1)

#define gte_gpf12() gte_GPF(1)

/*  gpf0    -       0x4B90003D  general purpose interpolation   1,31, 0 */
#define gte_ngpf0() gte_nGPF(0)

#define gte_gpf0() gte_GPF(0)

/*  gpl12   -       0x4BA8003E  general purpose interpolation   1,19,12 */
#define gte_ngpl12() gte_nGPL(1)

#define gte_gpl12() gte_GPL(1)

/*  gpl0    -       0x4BA0003E  general purpose interpolation   1,31, 0 */
#define gte_ngpl0() gte_nGPL(0)

#define gte_gpl0() gte_GPL(0)


/**
 * Memory and register management
 */

#define gte_ldv0(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $0,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $1,4($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldv1(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $2,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $3,4($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldv2(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $4,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $5,4($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldv3(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $0,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $1,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $2,($13)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $3,4($13)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $4,($14)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $5,4($14)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldv3c(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $0,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $1,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $2,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $3,12($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $4,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $5,20($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldv3c_vertc(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $0,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $1,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $2,12($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $3,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $4,24($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $5,28($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldv01(r1,r2) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $0,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $1,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $2,($13)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $3,4($13)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldv01c(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $0,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $1,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $2,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $3,12($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldrgb(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $6,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldrgb3(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $20,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $21,($13)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $22,($14)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $6,($14)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldrgb3c(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $20,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $21,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $22,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $6,8($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldlv0(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lhu   $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lhu   $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $14,$14,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("or    $13,$13,$14": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$0": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $1,8($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldlvl(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $9,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $10,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $11,8($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldsv(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lhu   $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lhu   $14,2($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lhu   $15,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $14,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $15,$11": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldbv(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lbu   $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lbu   $14,1($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $14,$10": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldcv(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lbu   $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lbu   $14,1($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lbu   $15,2($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $14,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $15,$11": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldclmv(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lhu   $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lhu   $14,6($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lhu   $15,12($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $14,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $15,$11": : :"$12","$13","$14","$15","memory"); \
}
#define gte_lddp(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $12,$8": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldsxy0(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $12,$12": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldsxy1(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $12,$13": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldsxy2(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $12,$14": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldsxy3(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $12,$12": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $14,$14": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$13": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldsxy3c(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $12,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $13,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $14,8($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldsz3(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $12,$17": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$18": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $14,$19": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldsz4(r1,r2,r3,r4) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $15,%0": :"r"(r4):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $12,$16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$17": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $14,$18": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $15,$19": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldopv1(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$0": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $15,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$2": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $15,$4": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldopv2(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $11,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $9,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $10,4($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldlzc(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $12,$30": : :"$12","$13","$14","$15","memory"); \
}
#define gte_SetRGBcd(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lwc2  $6,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldbkdir(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $12,$13": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$14": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$15": : :"$12","$13","$14","$15","memory"); \
}
#define gte_SetBackColor(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $12,$12,4": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $13,$13,4": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $14,$14,4": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $12,$13": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$14": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$15": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldfcdir(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $12,$21": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$22": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$23": : :"$12","$13","$14","$15","memory"); \
}
#define gte_SetFarColor(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $12,$12,4": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $13,$13,4": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $14,$14,4": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $12,$21": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$22": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$23": : :"$12","$13","$14","$15","memory"); \
}
#define gte_SetGeomOffset(r1,r2) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $12,$12,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $13,$13,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $12,$24": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$25": : :"$12","$13","$14","$15","memory"); \
}
#define gte_SetGeomScreen(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $12,$26": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldsvrtrow0(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$0": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$1": : :"$12","$13","$14","$15","memory"); \
}
#define gte_SetRotMatrix(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$0": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$1": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $14,12($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $15,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$2": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$3": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $15,$4": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldsvllrow0(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$8": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$9": : :"$12","$13","$14","$15","memory"); \
}
#define gte_SetLightMatrix(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$8": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $14,12($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $15,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$11": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $15,$12": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldsvlcrow0(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$17": : :"$12","$13","$14","$15","memory"); \
}
#define gte_SetColorMatrix(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$17": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $14,12($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $15,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$18": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$19": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $15,$20": : :"$12","$13","$14","$15","memory"); \
}
#define gte_SetTransMatrix(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,20($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $14,24($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$5": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $15,28($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$6": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $15,$7": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldtr(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $12,$5": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$6": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$7": : :"$12","$13","$14","$15","memory"); \
}
#define gte_SetTransVector(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $15,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$5": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$6": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $15,$7": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ld_intpol_uv0(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lbu   $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lbu   $14,1($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$21": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$22": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ld_intpol_uv1(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lbu   $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lbu   $14,1($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $14,$10": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ld_intpol_bv0(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lbu   $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lbu   $14,1($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$21": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$22": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ld_intpol_bv1(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lbu   $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lbu   $14,1($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$10": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ld_intpol_sv0(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lh    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lh    $14,2($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lh    $15,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$21": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$22": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $15,$23": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ld_intpol_sv1(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lh    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lh    $14,2($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lh    $15,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $14,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $15,$11": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ldfc(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $15,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$21": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$22": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $15,$23": : :"$12","$13","$14","$15","memory"); \
}





#define gte_stsxy(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $14,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy3(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $12,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $13,($13)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $14,($14)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy3c(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $12,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $13,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $14,8($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy2(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $14,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy1(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $13,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy0(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $12,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy01(r1,r2) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $12,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $13,($13)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy01c(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $12,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $13,4($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy3_f3(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $12,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $13,12($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $14,16($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy3_g3(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $12,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $13,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $14,24($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy3_ft3(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $12,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $13,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $14,24($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy3_gt3(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $12,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $13,20($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $14,32($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy3_f4(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $12,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $13,12($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $14,16($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy3_g4(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $12,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $13,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $14,24($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy3_ft4(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $12,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $13,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $14,24($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsxy3_gt4(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $12,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $13,20($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $14,32($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stdp(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $8,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stflg(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$31": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("nop   ": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stflg_4(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$31": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("addi  $14,$0,4": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $14,$14,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("and   $13,$13,$14": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsz(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $19,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsz3(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $17,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $18,($13)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $19,($14)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsz4(r1,r2,r3,r4) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $15,%0": :"r"(r4):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $16,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $17,($13)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $18,($14)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $19,($15)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsz3c(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $17,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $18,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $19,8($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsz4c(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $16,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $17,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $18,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $19,12($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stszotz(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $13,$19": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("nop   ": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $13,$13,2": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stotz(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $7,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stopz(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $24,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stlvl(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $9,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $10,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $11,8($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stlvnl(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $25,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $26,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $27,8($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stlvnl0(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $25,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stlvnl1(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $26,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stlvnl2(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $27,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stsv(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $13,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $14,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $15,$11": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sh    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sh    $14,2($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sh    $15,4($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stclmv(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $13,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $14,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $15,$11": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sh    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sh    $14,6($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sh    $15,12($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stbv(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $13,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $14,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sb    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sb    $14,1($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stcv(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $13,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $14,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $15,$11": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sb    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sb    $14,1($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sb    $15,2($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_strgb(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $22,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_strgb3(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $20,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $21,($13)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $22,($14)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_strgb3_g3(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $20,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $21,12($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $22,20($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_strgb3_gt3(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $20,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $21,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $22,28($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_strgb3_g4(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $20,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $21,12($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $22,20($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_strgb3_gt4(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $20,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $21,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $22,28($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ReadGeomOffset(r1,r2) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $14,$24": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $15,$25": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $14,$14,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $15,$15,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $14,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $15,($13)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ReadGeomScreen(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$26": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("nop   ": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ReadRotMatrix(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$0": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $14,$1": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$2": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $14,$3": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $15,$4": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $14,12($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $15,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$5": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $14,$6": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $15,$7": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,20($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $14,24($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $15,28($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_sttr(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$5": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $14,$6": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $15,$7": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $15,8($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ReadLightMatrix(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$8": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $14,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $14,$11": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $15,$12": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $14,12($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $15,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$13": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $14,$14": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $15,$15": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,20($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $14,24($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $15,28($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_ReadColorMatrix(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $14,$17": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$18": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $14,$19": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $15,$20": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,8($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $14,12($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $15,16($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$21": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $14,$22": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $15,$23": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,20($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $14,24($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $15,28($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stlzc(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("swc2  $31,($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_stfc(r1) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$21": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $14,$22": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $15,$23": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $13,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $14,4($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $15,8($12)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_mvlvtr() { \
    __asm__ volatile ("mfc2  $12,$25": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $13,$26": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $14,$27": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $12,$5": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $13,$6": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$7": : :"$12","$13","$14","$15","memory"); \
}
#define gte_nop() { \
    __asm__ volatile ("nop   ": : :"$12","$13","$14","$15","memory"); \
}
#define gte_subdvl(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $12,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,($13)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $12,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $12,$12,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $13,$13,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("subu  $15,$12,$13": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $12,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $13,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $15,4($14)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("subu  $12,$12,$13": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $12,($14)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_subdvd(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $12,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,($13)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $12,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $12,$12,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $13,$13,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("subu  $15,$12,$13": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $12,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $13,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sh    $15,2($14)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("subu  $12,$12,$13": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sh    $12,($14)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_adddvl(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $12,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,($13)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $12,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $12,$12,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $13,$13,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("addu  $15,$12,$13": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $12,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $13,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $15,4($14)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("addu  $12,$12,$13": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sw    $12,($14)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_adddvd(r1,r2,r3) { \
    __asm__ volatile ("move  $12,%0": :"r"(r1):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $13,%0": :"r"(r2):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("move  $14,%0": :"r"(r3):"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $12,($12)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("lw    $13,($13)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $12,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mtc2  $13,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $12,$12,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $13,$13,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("addu  $15,$12,$13": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $12,$9": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("mfc2  $13,$10": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sh    $15,2($14)": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("addu  $12,$12,$13": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sh    $12,($14)": : :"$12","$13","$14","$15","memory"); \
}
#define gte_FlipRotMatrixX() { \
    __asm__ volatile ("cfc2  $12,$0": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("cfc2  $13,$1": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $14,$12,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $14,$14,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("subu  $14,$0,$14": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $15,$12,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("subu  $15,$0,$15": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $15,$15,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $14,$14,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("srl   $14,$14,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("or    $14,$14,$15": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$0": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $14,$13,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $14,$14,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("subu  $14,$0,$14": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sra   $15,$13,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $15,$15,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("sll   $14,$14,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("srl   $14,$14,16": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("or    $14,$14,$15": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $14,$1": : :"$12","$13","$14","$15","memory"); \
}
#define gte_FlipTRX() { \
    __asm__ volatile ("cfc2  $12,$5": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("nop   ": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("subu  $12,$0,$12": : :"$12","$13","$14","$15","memory"); \
    __asm__ volatile ("ctc2  $12,$5": : :"$12","$13","$14","$15","memory"); \
}
