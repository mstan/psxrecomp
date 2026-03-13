.set noreorder

.section .text

.global gte_rtps_b
gte_rtps_b:
    RTPS
    jr $ra
    nop

.global gte_macros
gte_macros:
    rtv0
    rtv1
    rtv2
    rtir12
    rtir0
    rtv0tr
    rtv1tr
    rtv2tr
    rtirtr
    rtv0bk
    rtv1bk
    rtv2bk
    rtirbk
    ll
    llv0
    llv1
    llv2
    llvir
    llv0tr
    llv1tr
    llv2tr
    llirtr
    llv0bk
    llv1bk
    llv2bk
    llirbk
    lc
    lcv0
    lcv1
    lcv2
    lcvir
    lcv0tr
    lcv1tr
    lcv2tr
    lcirtr
    lev0bk
    lev1bk
    lev2bk
    leirbk
    # sqr12
    sqr0
    op12
    op0
    gpf12
    gpf0
    gpl12
    gpl0
    jr $ra
    nop
