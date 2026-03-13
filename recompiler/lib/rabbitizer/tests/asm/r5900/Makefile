CROSS := mips-linux-gnu-
AS    := $(CROSS)as

%.o: %.s
	$(AS) -march=r5900 -mabi=eabi -o $@ $^
