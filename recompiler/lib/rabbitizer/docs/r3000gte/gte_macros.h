/**
 * GTE macros
 * This file is meant to be used as an alternative to DMPSX
 */


/*  RTPS    15      0x4A180001  Perspective transform */
#define gte_nRTPS() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4A180001" \
    )

#define gte_RTPS() __asm__ ( \
        ".word 0x4A180001" \
    )

/*  RTPT    23      0x4A280030  Perspective transform on 3 points */
#define gte_nRTPT() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4A280030" \
    )

#define gte_RTPT() __asm__ ( \
        ".word 0x4A280030" \
    )

/*  DPCL    8       0x4A680029  Depth Cue Color light */
#define gte_nDPCL() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4A680029" \
    )

#define gte_DPCL() __asm__ ( \
        ".word 0x4A680029" \
    )

/*  DPCS    8       0x4A780010  Depth Cueing */
#define gte_nDPCS() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4A780010" \
    )

#define gte_DPCS() __asm__ ( \
        ".word 0x4A780010" \
    )

/*  DPCT    17      0x4AF8002A  Depth cue color RGB0,RGB1,RGB2 */
#define gte_nDPCT() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4AF8002A" \
    )

#define gte_DPCT() __asm__ ( \
        ".word 0x4AF8002A" \
    )

/*  INTPL   8       0x4A980011  Interpolation of vector and far color */
#define gte_nINTPL() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4A980011" \
    )

#define gte_INTPL() __asm__ ( \
        ".word 0x4A980011" \
    )

/*  NCS     14      0x4AC8041E  Normal color v0 */
#define gte_nNCS() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4AC8041E" \
    )

#define gte_NCS() __asm__ ( \
        ".word 0x4AC8041E" \
    )

/*  NCT     30      0x4AD80420  Normal color v0, v1, v2 */
#define gte_nNCT() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4AD80420" \
    )

#define gte_NCT() __asm__ ( \
        ".word 0x4AD80420" \
    )

/*  NCDS    19      0x4AE80413  Normal color depth cuev0 */
#define gte_nNCDS() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4AE80413" \
    )

#define gte_NCDS() __asm__ ( \
        ".word 0x4AE80413" \
    )

/*  NCDT    44      0x4AF80416  Normal color depth cue v0, v1, v2 */
#define gte_nNCDT() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4AF80416" \
    )

#define gte_NCDT() __asm__ ( \
        ".word 0x4AF80416" \
    )

/*  NCCS    17      0x4B08041B  Normal color col. v0 */
#define gte_nNCCS() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4B08041B" \
    )

#define gte_NCCS() __asm__ ( \
        ".word 0x4B08041B" \
    )

/*  NCCT    39      0x4B18043F  Normal color col.v0, v1, v2 */
#define gte_nNCCT() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4B18043F" \
    )

#define gte_NCCT() __asm__ ( \
        ".word 0x4B18043F" \
    )

/*  CDP     13      0x4B280414  Color Depth Queue */
#define gte_nCDP() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4B280414" \
    )

#define gte_CDP() __asm__ ( \
        ".word 0x4B280414" \
    )

/*  CC      11      0x4B38041C  Color Col. */
#define gte_CC() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4B38041C" \
    )

#define gte_CC() __asm__ ( \
        ".word 0x4B38041C" \
    )

/*  NCLIP   8       0x4B400006  Normal clipping */
#define gte_nNCLIP() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4B400006" \
    )

#define gte_NCLIP() __asm__ ( \
        ".word 0x4B400006" \
    )

/*  AVSZ3   5       0x4B58002D  Average of three Z values */
#define gte_nAVSZ3() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4B58002D" \
    )

#define gte_AVSZ3() __asm__ ( \
        ".word 0x4B58002D" \
    )

/*  AVSZ4   6       0x4B68002E  Average of four Z values */
#define gte_nAVSZ4() __asm__ ( \
        "nop;" \
        "nop;" \
        ".word 0x4B68002E" \
    )

#define gte_AVSZ4() __asm__ ( \
        ".word 0x4B68002E" \
    )

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
#define gte_nMVMVA(sf, mx, v, cv, lm) __asm__ ( \
        "nop;" \
        "nop;" \
        ".word %0" \
        : : "g"(0x4A400012 | ((sf) & 0x1) << 19 | ((mx) & 0x3) << 17 | ((v) & 0x3) << 15 | ((cv) & 0x3) << 13 | ((lm) & 0x1) << 10) \
    )

#define gte_MVMVA(sf, mx, v, cv, lm) __asm__ ( \
        ".word %0" \
        : : "g"(0x4A400012 | ((sf) & 0x1) << 19 | ((mx) & 0x3) << 17 | ((v) & 0x3) << 15 | ((cv) & 0x3) << 13 | ((lm) & 0x1) << 10) \
    )

/*  SQR     5       0x4AA00428  Square of vector */
#define gte_nSQR(sf) __asm__ ( \
        "nop;" \
        "nop;" \
        ".word %0" \
        : : "g"(0x4AA00428 | ((sf) & 0x1) << 19) \
    )

#define gte_SQR(sf) __asm__ ( \
        ".word %0" \
        : : "g"(0x4AA00428 | ((sf) & 0x1) << 19) \
    )

/*  OP      6       0x4B70000C  Outer Product */
#define gte_nOP(sf) __asm__ ( \
        "nop;" \
        "nop;" \
        ".word %0" \
        : : "g"(0x4B70000C | ((sf) & 0x1) << 19) \
    )

#define gte_OP(sf) __asm__ ( \
        ".word %0" \
        : : "g"(0x4B70000C | ((sf) & 0x1) << 19) \
    )

/*  GPF     6       0x4B90003D  General purpose interpolation */
#define gte_nGPF(sf) __asm__ ( \
        "nop;" \
        "nop;" \
        ".word %0" \
        : : "g"(0x4B90003D | ((sf) & 0x1) << 19) \
    )

#define gte_GPF(sf) __asm__ ( \
        ".word %0" \
        : : "g"(0x4B90003D | ((sf) & 0x1) << 19) \
    )

/*  GPL     5       0x4BA0003E  general purpose interpolation */
#define gte_nGPL(sf) __asm__ ( \
        "nop;" \
        "nop;" \
        ".word %0" \
        : : "g"(0x4BA0003E | ((sf) & 0x1) << 19) \
    )

#define gte_GPL(sf) __asm__ ( \
        ".word %0" \
        : : "g"(0x4BA0003E | ((sf) & 0x1) << 19) \
    )


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

#define gte_ldv0(r0) __asm__ ( \
        "lwc2    $0, 0(%0);" \
        "lwc2    $1, 4(%0)" \
        : : "r"(r0) \
    )

#define gte_ldv1(r0) __asm__ ( \
        "lwc2    $2, 0(%0);" \
        "lwc2    $3, 4(%0)" \
        : : "r"(r0) \
    )

#define gte_ldv2(r0) __asm__ ( \
        "lwc2    $4, 0(%0);" \
        "lwc2    $5, 4(%0)" \
        : : "r"(r0) \
    )

#define gte_ldv3(r0, r1, r2) __asm__ ( \
        "lwc2    $0, 0(%0);" \
        "lwc2    $1, 4(%0);" \
        "lwc2    $2, 0(%1);" \
        "lwc2    $3, 4(%1);" \
        "lwc2    $4, 0(%2);" \
        "lwc2    $5, 4(%2)" \
        : : "r"(r0), "r"(r1), "r"(r2) \
    )

#define gte_ldv3c(r0) __asm__ ( \
        "lwc2    $0, 0(%0);" \
        "lwc2    $1, 4(%0);" \
        "lwc2    $2, 8(%0);" \
        "lwc2    $3, 12(%0);" \
        "lwc2    $4, 16(%0);" \
        "lwc2    $5, 20(%0)" \
        : : "r"(r0) \
    )

#define gte_ldv3c_vertc(r0) __asm__ ( \
        "lwc2    $0, 0(%0);" \
        "lwc2    $1, 4(%0);" \
        "lwc2    $2, 12(%0);" \
        "lwc2    $3, 16(%0);" \
        "lwc2    $4, 24(%0);" \
        "lwc2    $5, 28(%0)" \
        : : "r"(r0) \
    )

#define gte_ldv01( r0, r1 ) __asm__ ( \
        "lwc2    $0, 0(%0);" \
        "lwc2    $1, 4(%0);" \
        "lwc2    $2, 0(%1);" \
        "lwc2    $3, 4(%1)" \
        : : "r"(r0), "r"(r1) \
    )

#define gte_ldv01c(r0) __asm__ ( \
        "lwc2    $0, 0(%0);" \
        "lwc2    $1, 4(%0);" \
        "lwc2    $2, 8(%0);" \
        "lwc2    $3, 12(%0)" \
        : : "r"(r0) \
    )

#define gte_ldrgb(r0) __asm__ ( \
        "lwc2    $6, 0(%0)" \
        : : "r"(r0) \
    )

#define gte_ldrgb3( r0, r1, r2 ) __asm__ ( \
        "lwc2    $20, 0(%0);" \
        "lwc2    $21, 0(%1);" \
        "lwc2    $22, 0(%2);" \
        "lwc2    $6, 0(%2)" \
        : : "r"(r0), "r"(r1), "r"(r2) \
    )

#define gte_ldrgb3c(r0) __asm__ ( \
        "lwc2    $20, 0(%0);" \
        "lwc2    $21, 4(%0);" \
        "lwc2    $22, 8(%0);" \
        "lwc2    $6, 8(%0)" \
        : : "r"(r0) \
    )

#define gte_ldlv0(r0) __asm__ ( \
        "lhu    $13, 4(%0);" \
        "lhu    $12, 0(%0);" \
        "sll    $13, $13, 16;" \
        "or    $12, $12, $13;" \
        "mtc2    $12, $0;" \
        "lwc2    $1, 8(%0)" \
        : : "r"(r0) \
        : "$12", "$13" \
    )

#define gte_ldlvl(r0) __asm__ ( \
        "lwc2    $9, 0(%0);" \
        "lwc2    $10, 4(%0);" \
        "lwc2    $11, 8(%0)" \
        : : "r"(r0) \
    )

#define gte_ldsv(r0) __asm__ ( \
        "lhu    $12, 0(%0);" \
        "lhu    $13, 2(%0);" \
        "lhu    $14, 4(%0);" \
        "mtc2    $12, $9;" \
        "mtc2    $13, $10;" \
        "mtc2    $14, $11" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )

#define gte_ldbv(r0) __asm__ ( \
        "lbu    $12, 0(%0);" \
        "lbu    $13, 1(%0);" \
        "mtc2    $12, $9;" \
        "mtc2    $13, $10" \
        : : "r"(r0) \
        : "$12", "$13" \
    )

#define gte_ldcv(r0) __asm__ ( \
        "lbu    $12, 0(%0);" \
        "lbu    $13, 1(%0);" \
        "lbu    $14, 2(%0);" \
        "mtc2    $12, $9;" \
        "mtc2    $13, $10;" \
        "mtc2    $14, $11" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )

#define gte_ldclmv(r0) __asm__ ( \
        "lhu    $12, 0(%0);" \
        "lhu    $13, 6(%0);" \
        "lhu    $14, 12(%0);" \
        "mtc2    $12, $9;" \
        "mtc2    $13, $10;" \
        "mtc2    $14, $11" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )

#define gte_lddp(r0) __asm__ ( \
        "mtc2    %0, $8" \
        : : "r"(r0) \
    )

#define gte_ldsxy0(r0) __asm__ ( \
        "mtc2    %0, $12" \
        : : "r"(r0) \
    )

#define gte_ldsxy1(r0) __asm__ ( \
        "mtc2    %0, $13" \
        : : "r"(r0) \
    )

#define gte_ldsxy2(r0) __asm__ ( \
        "mtc2    %0, $14" \
        : : "r"(r0) \
    )

#define gte_ldsxy3( r0, r1, r2 ) __asm__ ( \
        "mtc2    %0, $12;" \
        "mtc2    %2, $14;" \
        "mtc2    %1, $13" \
        : : "r"(r0), "r"(r1), "r"(r2) \
    )

#define gte_ldsxy3c(r0) __asm__ ( \
        "lwc2    $12, 0(%0);" \
        "lwc2    $13, 4(%0);" \
        "lwc2    $14, 8(%0)" \
        : : "r"(r0) \
    )

#define gte_ldsz3( r0, r1, r2 ) __asm__ ( \
        "mtc2    %0, $17;" \
        "mtc2    %1, $18;" \
        "mtc2    %2, $19" \
        : : "r"(r0), "r"(r1), "r"(r2) \
    )

#define gte_ldsz4( r0, r1, r2, r3 ) __asm__ (    \
        "mtc2    %0, $16;" \
        "mtc2    %1, $17;" \
        "mtc2    %2, $18;" \
        "mtc2    %3, $19" \
        : : "r"(r0), "r"(r1), "r"(r2), "r"(r3) \
    )

#define gte_ldopv1(r0) __asm__ ( \
        "lw    $12, 0(%0);" \
        "lw    $13, 4(%0);" \
        "ctc2    $12, $0;" \
        "lw    $14, 8(%0);" \
        "ctc2    $13, $2;" \
        "ctc2    $14, $4" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )

#define gte_ldopv2(r0) __asm__ ( \
        "lwc2    $11, 8(%0);" \
        "lwc2    $9, 0(%0);" \
        "lwc2    $10, 4(%0)" \
        : : "r"(r0) \
    )

#define gte_ldlzc(r0) __asm__ ( \
        "mtc2    %0, $30" \
        : : "r"(r0) \
    )

#define gte_SetRGBcd(r0) __asm__ ( \
        "lwc2    $6, 0(%0)" \
        : : "r"(r0) \
    )

#define gte_ldbkdir( r0, r1, r2 ) __asm__ ( \
        "ctc2    %0, $13;" \
        "ctc2    %1, $14;" \
        "ctc2    %2, $15" \
        : : "r"(r0), "r"(r1), "r"(r2) \
    )

#define gte_SetBackColor( r0, r1, r2 ) __asm__ (    \
        "sll    $12, %0, 4;" \
        "sll    $13, %1, 4;" \
        "sll    $14, %2, 4;" \
        "ctc2    $12, $13;" \
        "ctc2    $13, $14;" \
        "ctc2    $14, $15" \
        : : "r"(r0), "r"(r1), "r"(r2) \
        : "$12", "$13", "$14" \
    )

#define gte_ldfcdir( r0, r1, r2 ) __asm__ ( \
        "ctc2    %0, $21;" \
        "ctc2    %1, $22;" \
        "ctc2    %2, $23" \
        : : "r"(r0), "r"(r1), "r"(r2) \
    )

#define gte_SetFarColor( r0, r1, r2 ) __asm__ (    \
        "sll    $12, %0, 4;" \
        "sll    $13, %1, 4;" \
        "sll    $14, %2, 4;" \
        "ctc2    $12, $21;" \
        "ctc2    $13, $22;" \
        "ctc2    $14, $23" \
        : : "r"(r0), "r"(r1), "r"(r2) \
        : "$12", "$13", "$14" \
    )

#define gte_SetGeomOffset( r0, r1 ) __asm__ (    \
        "sll    $12, %0, 16;" \
        "sll    $13, %1, 16;" \
        "ctc2    $12, $24;" \
        "ctc2    $13, $25" \
        : : "r"(r0), "r"(r1) \
        : "$12", "$13" \
    )

#define gte_SetGeomScreen(r0) __asm__ ( \
        "ctc2    %0, $26" \
        : : "r"(r0) \
    )

#define gte_ldsvrtrow0(r0) __asm__ ( \
        "lw    $12, 0(%0);" \
        "lw    $13, 4(%0);" \
        "ctc2    $12, $0;" \
        "ctc2    $13, $1" \
        : : "r"(r0) \
        : "$12", "$13" \
    )

#define gte_SetRotMatrix(r0) __asm__ ( \
        "lw    $12, 0(%0);" \
        "lw    $13, 4(%0);" \
        "ctc2    $12, $0;" \
        "ctc2    $13, $1;" \
        "lw    $12, 8(%0);" \
        "lw    $13, 12(%0);" \
        "lw    $14, 16(%0);" \
        "ctc2    $12, $2;" \
        "ctc2    $13, $3;" \
        "ctc2    $14, $4" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )

#define gte_ldsvllrow0(r0) __asm__ ( \
        "lw    $12, 0(%0);" \
        "lw    $13, 4(%0);" \
        "ctc2    $12, $8;" \
        "ctc2    $13, $9" \
        : : "r"(r0) \
        : "$12", "$13" \
    )

#define gte_SetLightMatrix(r0) __asm__ ( \
        "lw    $12, 0(%0);" \
        "lw    $13, 4(%0);" \
        "ctc2    $12, $8;" \
        "ctc2    $13, $9;" \
        "lw    $12, 8(%0);" \
        "lw    $13, 12(%0);" \
        "lw    $14, 16(%0);" \
        "ctc2    $12, $10;" \
        "ctc2    $13, $11;" \
        "ctc2    $14, $12" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )

#define gte_ldsvlcrow0(r0) __asm__ ( \
        "lw    $12, 0(%0);" \
        "lw    $13, 4(%0);" \
        "ctc2    $12, $16;" \
        "ctc2    $13, $17" \
        : : "r"(r0) \
        : "$12", "$13" \
    )

#define gte_SetColorMatrix(r0) __asm__ ( \
        "lw    $12, 0(%0);" \
        "lw    $13, 4(%0);" \
        "ctc2    $12, $16;" \
        "ctc2    $13, $17;" \
        "lw    $12, 8(%0);" \
        "lw    $13, 12(%0);" \
        "lw    $14, 16(%0);" \
        "ctc2    $12, $18;" \
        "ctc2    $13, $19;" \
        "ctc2    $14, $20" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )

#define gte_SetTransMatrix(r0) __asm__ ( \
        "lw    $12, 20(%0);" \
        "lw    $13, 24(%0);" \
        "ctc2    $12, $5;" \
        "lw    $14, 28(%0);" \
        "ctc2    $13, $6;" \
        "ctc2    $14, $7" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )

#define gte_ldtr( r0, r1, r2 ) __asm__ ( \
        "ctc2    %0, $5;" \
        "ctc2    %1, $6;" \
        "ctc2    %2, $7" \
        : : "r"(r0), "r"(r1), "r"(r2) \
    )

#define gte_SetTransVector(r0) __asm__ ( \
        "lw    $12, 0(%0);" \
        "lw    $13, 4(%0);" \
        "lw    $14, 8(%0);" \
        "ctc2    $12, $5;" \
        "ctc2    $13, $6;" \
        "ctc2    $14, $7" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )

#define gte_ld_intpol_uv0(r0) __asm__ ( \
        "lbu    $12, 0(%0);" \
        "lbu    $13, 1(%0);" \
        "ctc2    $12, $21;" \
        "ctc2    $13, $22" \
        : : "r"(r0) \
        : "$12", "$13" \
    )

#define gte_ld_intpol_uv1(r0) __asm__ ( \
        "lbu    $12, 0(%0);" \
        "lbu    $13, 1(%0);" \
        "mtc2    $12, $9;" \
        "mtc2    $13, $10" \
        : : "r"(r0) \
        : "$12", "$13" \
    )

#define gte_ld_intpol_bv0(r0) __asm__ ( \
        "lbu    $12, 0(%0);" \
        "lbu    $13, 1(%0);" \
        "ctc2    $12, $21;" \
        "ctc2    $13, $22" \
        : : "r"(r0) \
        : "$12", "$13" \
    )

#define gte_ld_intpol_bv1(r0) __asm__ ( \
        "lbu    $12, 0(%0);" \
        "lbu    $13, 1(%0);" \
        "mtc2    $12, $9;" \
        "mtc2    $13, $10" \
        : : "r"(r0) \
        : "$12", "$13" \
    )

#define gte_ld_intpol_sv0(r0) __asm__ ( \
        "lh    $12, 0(%0);" \
        "lh    $13, 2(%0);" \
        "lh    $14, 4(%0);" \
        "ctc2    $12, $21;" \
        "ctc2    $13, $22;" \
        "ctc2    $14, $23" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )

#define gte_ld_intpol_sv1(r0) __asm__ ( \
        "lh    $12, 0(%0);" \
        "lh    $13, 2(%0);" \
        "lh    $14, 4(%0);" \
        "mtc2    $12, $9;" \
        "mtc2    $13, $10;" \
        "mtc2    $14, $11" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )

#define gte_ldfc(r0) __asm__ ( \
        "lw    $12, 0(%0);" \
        "lw    $13, 4(%0);" \
        "lw    $14, 8(%0);" \
        "ctc2    $12, $21;" \
        "ctc2    $13, $22;" \
        "ctc2    $14, $23" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )

#define gte_ldopv2SV(r0) __asm__ ( \
        "lh    $12, 0(%0);" \
        "lh    $13, 2(%0);" \
        "lh    $14, 4(%0);" \
        "mtc2    $12, $9;" \
        "mtc2    $13, $10;" \
        "mtc2    $14, $11" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )

#define gte_ldopv1SV(r0) __asm__ ( \
        "lh    $12, 0(%0);" \
        "lh    $13, 2(%0);" \
        "ctc2    $12, $0;" \
        "lh    $14, 4(%0);" \
        "ctc2    $13, $2;" \
        "ctc2    $14, $4" \
        : : "r"(r0) \
        : "$12", "$13", "$14" \
    )





#define gte_stsxy(r0) __asm__ ( \
        "swc2    $14, 0(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsxy3( r0, r1, r2 ) __asm__ ( \
        "swc2    $12, 0(%0);" \
        "swc2    $13, 0(%1);" \
        "swc2    $14, 0(%2)" \
        : : "r"(r0), "r"(r1), "r"(r2) \
        : "memory" \
    )

#define gte_stsxy3c(r0) __asm__ ( \
        "swc2    $12, 0(%0);" \
        "swc2    $13, 4(%0);" \
        "swc2    $14, 8(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsxy2(r0) __asm__ ( \
        "swc2    $14, 0(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsxy1(r0) __asm__ ( \
        "swc2    $13, 0(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsxy0(r0) __asm__ ( \
        "swc2    $12, 0(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsxy01( r0, r1 ) __asm__ ( \
        "swc2    $12, 0(%0);" \
        "swc2    $13, 0(%1)" \
        : : "r"(r0), "r"(r1) \
        : "memory" \
    )

#define gte_stsxy01c(r0) __asm__ ( \
        "swc2    $12, 0(%0);" \
        "swc2    $13, 4(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsxy3_f3(r0) __asm__ ( \
        "swc2    $12, 8(%0);" \
        "swc2    $13, 12(%0);" \
        "swc2    $14, 16(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsxy3_g3(r0) __asm__ ( \
        "swc2    $12, 8(%0);" \
        "swc2    $13, 16(%0);" \
        "swc2    $14, 24(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsxy3_ft3(r0) __asm__ ( \
        "swc2    $12, 8(%0);" \
        "swc2    $13, 16(%0);" \
        "swc2    $14, 24(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsxy3_gt3(r0) __asm__ ( \
        "swc2    $12, 8(%0);" \
        "swc2    $13, 20(%0);" \
        "swc2    $14, 32(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsxy3_f4(r0) __asm__ ( \
        "swc2    $12, 8(%0);" \
        "swc2    $13, 12(%0);" \
        "swc2    $14, 16(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsxy3_g4(r0) __asm__ ( \
        "swc2    $12, 8(%0);" \
        "swc2    $13, 16(%0);" \
        "swc2    $14, 24(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsxy3_ft4(r0) __asm__ ( \
        "swc2    $12, 8(%0);" \
        "swc2    $13, 16(%0);" \
        "swc2    $14, 24(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsxy3_gt4(r0) __asm__ ( \
        "swc2    $12, 8(%0);" \
        "swc2    $13, 20(%0);" \
        "swc2    $14, 32(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stdp(r0) __asm__ ( \
        "swc2    $8, 0(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stflg(r0) __asm__ ( \
        "cfc2    $12, $31;" \
        "nop;" \
        "sw    $12, 0(%0)" \
        : : "r"(r0) \
        : "$12", "memory" \
    )

#define gte_stflg_4(r0) __asm__ ( \
        "cfc2    $12, $31;" \
        "addi    $13, $0, 4;" \
        "sll    $13, $13, 16;" \
        "and    $12, $12, $13;" \
        "sw    $12, 0(%0)" \
        : : "r"(r0) \
        : "$12", "$13", "memory" \
    )

#define gte_stsz(r0) __asm__ ( \
        "swc2    $19, 0(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsz3( r0, r1, r2 ) __asm__ ( \
        "swc2    $17, 0(%0);" \
        "swc2    $18, 0(%1);" \
        "swc2    $19, 0(%2)" \
        : : "r"(r0), "r"(r1), "r"(r2) \
        : "memory" \
    )

#define gte_stsz4( r0, r1, r2, r3 ) __asm__ ( \
        "swc2    $16, 0(%0);" \
        "swc2    $17, 0(%1);" \
        "swc2    $18, 0(%2);" \
        "swc2    $19, 0(%3)" \
        : : "r"(r0), "r"(r1), "r"(r2), "r"(r3) \
        : "memory" \
    )

#define gte_stsz3c(r0) __asm__ ( \
        "swc2    $17, 0(%0);" \
        "swc2    $18, 4(%0);" \
        "swc2    $19, 8(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsz4c(r0) __asm__ ( \
        "swc2    $16, 0(%0);" \
        "swc2    $17, 4(%0);" \
        "swc2    $18, 8(%0);" \
        "swc2    $19, 12(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stszotz(r0) __asm__ ( \
        "mfc2    $12, $19;" \
        "nop;" \
        "sra    $12, $12, 2;" \
        "sw    $12, 0(%0)" \
        : : "r"(r0) \
        : "$12", "memory" \
    )

#define gte_stotz(r0) __asm__ ( \
        "swc2    $7, 0(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stopz(r0) __asm__ ( \
        "swc2    $24, 0(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stlvl(r0) __asm__ ( \
        "swc2    $9, 0(%0);" \
        "swc2    $10, 4(%0);" \
        "swc2    $11, 8(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stlvnl(r0) __asm__ ( \
        "swc2    $25, 0(%0);" \
        "swc2    $26, 4(%0);" \
        "swc2    $27, 8(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stlvnl0(r0) __asm__ ( \
        "swc2    $25, 0(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stlvnl1(r0) __asm__ ( \
        "swc2    $26, 0(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stlvnl2(r0) __asm__ ( \
        "swc2    $27, 0(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stsv(r0) __asm__ ( \
        "mfc2    $12, $9;" \
        "mfc2    $13, $10;" \
        "mfc2    $14, $11;" \
        "sh    $12, 0(%0);" \
        "sh    $13, 2(%0);" \
        "sh    $14, 4(%0)" \
        : : "r"(r0) \
        : "$12", "$13", "$14", "memory" \
    )

#define gte_stclmv(r0) __asm__ ( \
        "mfc2    $12, $9;" \
        "mfc2    $13, $10;" \
        "mfc2    $14, $11;" \
        "sh    $12, 0(%0);" \
        "sh    $13, 6(%0);" \
        "sh    $14, 12(%0)" \
        : : "r"(r0) \
        : "$12", "$13", "$14", "memory" \
    )

#define gte_stbv(r0) __asm__ ( \
        "mfc2    $12, $9;" \
        "mfc2    $13, $10;" \
        "sb    $12, 0(%0);" \
        "sb    $13, 1(%0)" \
        : : "r"(r0) \
        : "$12", "$13", "memory" \
    )

#define gte_stcv(r0) __asm__ ( \
        "mfc2    $12, $9;" \
        "mfc2    $13, $10;" \
        "mfc2    $14, $11;" \
        "sb    $12, 0(%0);" \
        "sb    $13, 1(%0);" \
        "sb    $14, 2(%0)" \
        : : "r"(r0) \
        : "$12", "$13", "$14", "memory" \
    )

#define gte_strgb(r0) __asm__ ( \
        "swc2    $22, 0(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_strgb3( r0, r1, r2 ) __asm__ ( \
        "swc2    $20, 0(%0);" \
        "swc2    $21, 0(%1);" \
        "swc2    $22, 0(%2)" \
        : : "r"(r0), "r"(r1), "r"(r2) \
        : "memory" \
    )

#define gte_strgb3_g3(r0) __asm__ ( \
        "swc2    $20, 4(%0);" \
        "swc2    $21, 12(%0);" \
        "swc2    $22, 20(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_strgb3_gt3(r0) __asm__ ( \
        "swc2    $20, 4(%0);" \
        "swc2    $21, 16(%0);" \
        "swc2    $22, 28(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_strgb3_g4(r0) __asm__ ( \
        "swc2    $20, 4(%0);" \
        "swc2    $21, 12(%0);" \
        "swc2    $22, 20(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_strgb3_gt4(r0) __asm__ ( \
        "swc2    $20, 4(%0);" \
        "swc2    $21, 16(%0);" \
        "swc2    $22, 28(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_ReadGeomOffset( r0, r1 ) __asm__ ( \
        "cfc2    $12, $24;" \
        "cfc2    $13, $25;" \
        "sra    $12, $12, 16;" \
        "sra    $13, $13, 16;" \
        "sw    $12, 0(%0);" \
        "sw    $13, 0(%1)" \
        : : "r"(r0), "r"(r1) \
        : "$12", "$13", "memory" \
    )

#define gte_ReadGeomScreen(r0) __asm__ ( \
        "cfc2    $12, $26;" \
        "nop;" \
        "sw    $12, 0(%0)" \
        : : "r"(r0) \
        : "$12", "memory" \
    )

#define gte_ReadRotMatrix(r0) __asm__ ( \
        "cfc2    $12, $0;" \
        "cfc2    $13, $1;" \
        "sw    $12, 0(%0);" \
        "sw    $13, 4(%0);" \
        "cfc2    $12, $2;" \
        "cfc2    $13, $3;" \
        "cfc2    $14, $4;" \
        "sw    $12, 8(%0);" \
        "sw    $13, 12(%0);" \
        "sw    $14, 16(%0);" \
        "cfc2    $12, $5;" \
        "cfc2    $13, $6;" \
        "cfc2    $14, $7;" \
        "sw    $12, 20(%0);" \
        "sw    $13, 24(%0);" \
        "sw    $14, 28(%0)" \
        : : "r"(r0) \
        : "$12", "$13", "$14", "memory" \
    )

#define gte_sttr(r0) __asm__ ( \
        "cfc2    $12, $5;" \
        "cfc2    $13, $6;" \
        "cfc2    $14, $7;" \
        "sw    $12, 0(%0);" \
        "sw    $13, 4(%0);" \
        "sw    $14, 8(%0)" \
        : : "r"(r0) \
        : "$12", "$13", "$14", "memory" \
    )

#define gte_ReadLightMatrix(r0) __asm__ ( \
        "cfc2    $12, $8;" \
        "cfc2    $13, $9;" \
        "sw    $12, 0(%0);" \
        "sw    $13, 4(%0);" \
        "cfc2    $12, $10;" \
        "cfc2    $13, $11;" \
        "cfc2    $14, $12;" \
        "sw    $12, 8(%0);" \
        "sw    $13, 12(%0);" \
        "sw    $14, 16(%0);" \
        "cfc2    $12, $13;" \
        "cfc2    $13, $14;" \
        "cfc2    $14, $15;" \
        "sw    $12, 20(%0);" \
        "sw    $13, 24(%0);" \
        "sw    $14, 28(%0)" \
        : : "r"(r0) \
        : "$12", "$13", "$14", "memory" \
    )

#define gte_ReadColorMatrix(r0) __asm__ ( \
        "cfc2    $12, $16;" \
        "cfc2    $13, $17;" \
        "sw    $12, 0(%0);" \
        "sw    $13, 4(%0);" \
        "cfc2    $12, $18;" \
        "cfc2    $13, $19;" \
        "cfc2    $14, $20;" \
        "sw    $12, 8(%0);" \
        "sw    $13, 12(%0);" \
        "sw    $14, 16(%0);" \
        "cfc2    $12, $21;" \
        "cfc2    $13, $22;" \
        "cfc2    $14, $23;" \
        "sw    $12, 20(%0);" \
        "sw    $13, 24(%0);" \
        "sw    $14, 28(%0)" \
        : : "r"(r0) \
        : "$12", "$13", "$14", "memory" \
    )

#define gte_stlzc(r0) __asm__ ( \
        "swc2    $31, 0(%0)" \
        : : "r"(r0) \
        : "memory" \
    )

#define gte_stfc(r0) __asm__ ( \
        "cfc2    $12, $21;" \
        "cfc2    $13, $22;" \
        "cfc2    $14, $23;" \
        "sw    $12, 0(%0);" \
        "sw    $13, 4(%0);" \
        "sw    $14, 8(%0)" \
        : : "r"(r0) \
        : "$12", "$13", "$14", "memory" \
    )

#define gte_mvlvtr() __asm__ ( \
        "mfc2    $12, $25;" \
        "mfc2    $13, $26;" \
        "mfc2    $14, $27;" \
        "ctc2    $12, $5;" \
        "ctc2    $13, $6;" \
        "ctc2    $14, $7" \
        : : \
        : "$12", "$13", "$14" \
    )

#define gte_nop() __asm__ ( \
        "nop" \
    )

#define gte_subdvl( r0, r1, r2 ) __asm__ ( \
        "lw    $12, 0(%0);" \
        "lw    $13, 0(%1);" \
        "mtc2    $12, $9;" \
        "mtc2    $13, $10;" \
        "sra    $12, $12, 16;" \
        "sra    $13, $13, 16;" \
        "subu    $15, $12, $13;" \
        "mfc2    $12, $9;" \
        "mfc2    $13, $10;" \
        "sw    $15, 4(%2);" \
        "subu    $12, $12, $13;" \
        "sw    $12, 0(%2)" \
        : : "r"(r0), "r"(r1), "r"(r2) \
        : "$12", "$13", "$14", "$15", "memory" \
    )

#define gte_subdvd( r0, r1, r2 ) __asm__ ( \
        "lw    $12, 0(%0);" \
        "lw    $13, 0(%1);" \
        "mtc2    $12, $9;" \
        "mtc2    $13, $10;" \
        "sra    $12, $12, 16;" \
        "sra    $13, $13, 16;" \
        "subu    $15, $12, $13;" \
        "mfc2    $12, $9;" \
        "mfc2    $13, $10;" \
        "sh    $15, 2(%2);" \
        "subu    $12, $12, $13;" \
        "sh    $12, 0(%2)" \
        : : "r"(r0), "r"(r1), "r"(r2) \
        : "$12", "$13", "$14", "$15", "memory" \
    )

#define gte_adddvl( r0, r1, r2 ) __asm__ ( \
        "lw    $12, 0(%0);" \
        "lw    $13, 0(%1);" \
        "mtc2    $12, $9;" \
        "mtc2    $13, $10;" \
        "sra    $12, $12, 16;" \
        "sra    $13, $13, 16;" \
        "addu    $15, $12, $13;" \
        "mfc2    $12, $9;" \
        "mfc2    $13, $10;" \
        "sw    $15, 4(%2);" \
        "addu    $12, $12, $13;" \
        "sw    $12, 0(%2)" \
        : : "r"(r0), "r"(r1), "r"(r2) \
        : "$12", "$13", "$14", "$15", "memory" \
    )

#define gte_adddvd( r0, r1, r2 ) __asm__ ( \
        "lw    $12, 0(%0);" \
        "lw    $13, 0(%1);" \
        "mtc2    $12, $9;" \
        "mtc2    $13, $10;" \
        "sra    $12, $12, 16;" \
        "sra    $13, $13, 16;" \
        "addu    $15, $12, $13;" \
        "mfc2    $12, $9;" \
        "mfc2    $13, $10;" \
        "sh    $15, 2(%2);" \
        "addu    $12, $12, $13;" \
        "sh    $12, 0(%2)" \
        : : "r"(r0), "r"(r1), "r"(r2) \
        : "$12", "$13", "$14", "$15", "memory" \
    )

#define gte_FlipRotMatrixX() __asm__ ( \
        "cfc2    $12, $0;" \
        "cfc2    $13, $1;" \
        "sll    $14, $12, 16;" \
        "sra    $14, $14, 16;" \
        "subu    $14, $0, $14;" \
        "sra    $15, $12, 16;" \
        "subu    $15, $0, $15;" \
        "sll    $15, $15, 16;" \
        "sll    $14, $14, 16;" \
        "srl    $14, $14, 16;" \
        "or    $14, $14, $15;" \
        "ctc2    $14, $0;" \
        "sll    $14, $13, 16;" \
        "sra    $14, $14, 16;" \
        "subu    $14, $0, $14;" \
        "sra    $15, $13, 16;" \
        "sll    $15, $15, 16;" \
        "sll    $14, $14, 16;" \
        "srl    $14, $14, 16;" \
        "or    $14, $14, $15;" \
        "ctc2    $14, $1" \
        : : \
        : "$12", "$13", "$14", "$15" \
    )

#define gte_FlipTRX() __asm__ ( \
        "cfc2    $12, $5;" \
        "nop;" \
        "subu    $12, $0, $12;" \
        "ctc2    $12, $5" \
        : : \
        : "$12" \
    )
