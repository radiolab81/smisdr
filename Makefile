CFLAGS += -I. -I/usr/src/linux-headers-6.12.47+rpt-common-rpi/include/  -pipe -Wall -Wextra -O2 -g
LDLIBS += -lcrypto

RM := rm -f

smi_util: smi_util.o

.PHONY: clean
clean:
	$(RM) smi_util
	$(RM) smi_util.o
