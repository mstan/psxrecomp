.set noreorder

.section .text

.global test
trunc.w.s $f0, $f12
jr $31
nop

.global test2
cvt.w.s $f0, $f12
jr $31
nop


