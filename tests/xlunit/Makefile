# per-instruction tests of the translator:  make -C tests/xlunit   (needs m68k-linux-gnu-gcc/as and qemu-m68k)
OUT=../../build/xlt
CC=m68k-linux-gnu-gcc
AS=m68k-linux-gnu-as
all: $(OUT)/xl_unit
	qemu-m68k -cpu m68020 $(OUT)/xl_unit $(OUT) $(TRIALS)
$(OUT)/code.s $(OUT)/tab.s $(OUT)/prg.bin $(OUT)/tests.txt: gen.py ../../translator/x65.py ../../translator/opinfo.py
	python3 gen.py $(OUT)
$(OUT)/xl_unit: $(OUT)/code.s $(OUT)/tab.s xl_unit.s xl_unit.c ../../runtime/cpu6502.c
	$(AS) -m68020 --register-prefix-optional $(OUT)/code.s -o $(OUT)/code.o
	$(AS) -m68020 --register-prefix-optional $(OUT)/tab.s -o $(OUT)/tab.o
	$(AS) -m68020 --register-prefix-optional xl_unit.s -o $(OUT)/xl_unit_s.o
	$(CC) -static -m68020 -O1 -o $@ xl_unit.c ../../runtime/cpu6502.c $(OUT)/code.o $(OUT)/tab.o $(OUT)/xl_unit_s.o
