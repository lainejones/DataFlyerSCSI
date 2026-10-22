# DataFlyerSCSI - a driver for the Expansion Systems DataFlyer 1200 SCSI+.
# Build under WSL with amiga-gcc:  PATH=/opt/amiga/bin:$PATH make
CC      = m68k-amigaos-gcc
CFLAGS  = -m68000 -Os -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer $(if $(DEBUG),-DDFDEBUG)
OUT     = out

all: $(OUT)/dataflyer.device $(OUT)/flyprobe $(OUT)/flytest $(OUT)/flystat $(OUT)/devlist $(OUT)/fcmp $(OUT)/flyread $(OUT)/rdbfix $(OUT)/RDBMount $(OUT)/tick $(OUT)/flychange $(OUT)/flyirq

# 68020: the DataFlyer 1200 only fits the A1200 (68EC020 and up) - hardware divide
$(OUT)/dataflyer.device: dev/start.S dev/dataflyer.c dev/ide.c dev/dataflyer.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -m68020 -ffreestanding -fno-builtin -nostartfiles -nostdlib -s -o $@ dev/start.S dev/dataflyer.c dev/ide.c -lgcc

$(OUT)/flyprobe: tools/flyprobe.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

$(OUT)/flytest: tools/flytest.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

clean:
	rm -rf $(OUT)

$(OUT)/flystat: tools/flystat.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

$(OUT)/devlist: tools/devlist.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

$(OUT)/fcmp: tools/fcmp.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

$(OUT)/flyread: tools/flyread.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

$(OUT)/rdbfix: tools/rdbfix.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

$(OUT)/RDBMount: tools/rdbmount.c tools/rdbmountcore.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ tools/rdbmount.c

$(OUT)/tick: tools/tick.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

$(OUT)/flychange: tools/flychange.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

$(OUT)/flyirq: tools/flyirq.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<
