;
; GTE commands without nops
;

RTPS		macro
		dw	$0000007f
		endm

RTPT		macro
		dw	$000000bf
		endm

DCPL		macro
		dw	$00000dff
		endm

DPCS		macro
		dw	$00000e3f
		endm

DPCT		macro
		dw	$00000e7f
		endm

INTPL		macro
		dw	$00000ebf
		endm

NCS		macro
		dw	$00000f7f
		endm

NCT		macro
		dw	$00000fbf
		endm

NCDS		macro
		dw	$00000fff
		endm

NCDT		macro
		dw	$0000103f
		endm

NCCS		macro
		dw	$0000107f
		endm

NCCT		macro
		dw	$000010bf
		endm

CDP		macro
		dw	$000010ff
		endm

CC		macro
		dw	$0000113f
		endm

NCLIP		macro
		dw	$0000117f
		endm

AVSZ3		macro
		dw	$000011bf
		endm

AVSZ4		macro
		dw	$000011ff
		endm

MVMVA		macro	sf,mx,v,cv,lm
		dw	$000013bf|sf<<25|mx<<23|v<<21|cv<<19|lm<<18
		endm

SQR		macro	sf
		dw	$000013ff|sf<<25
		endm

OP		macro	sf
		dw	$0000143f|sf<<25
		endm

GPF		macro	sf
		dw	$0000147f|sf<<25
		endm

GPL		macro	sf
		dw	$000014bf|sf<<25
		endm
