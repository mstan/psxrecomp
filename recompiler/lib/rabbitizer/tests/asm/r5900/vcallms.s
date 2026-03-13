.set noreorder

.section .text

.global vcallms_test
vcallms_test:
.ent vcallms_test
    vcallms 0x0000
    vcallms 0x0800
    vcallms 0x1000
    vcallms 0x1100
    vcallms 0x1200
    vcallms 0x1300
    vcallms 0x1400
    vcallms 0xFFF8
    vcallms 0x10000
    vcallms 0x3F000
    vcallms 0x3FFF0
    jr $ra
    nop

.end vcallms_test
