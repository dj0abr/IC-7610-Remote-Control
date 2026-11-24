CFLAGS ?= -O2 -Wall -Wextra -Werror

iq-test: iq-test.c ftdi/ftd3xx.h ftdi/Types.h ftdi/libftd3xx.so
	$(CC) $(CPPFLAGS) -isystem ftdi $(CFLAGS) -std=c11 -o $@ $< $(LDFLAGS) -Lftdi -Wl,-rpath,'$$ORIGIN/ftdi' -lftd3xx $(LDLIBS)
