CROSS_COMPILE ?=
# Keep a CC given on the command line or in the environment (buildroot),
# otherwise use the (cross) compiler named by CROSS_COMPILE
ifeq ($(origin CC),default)
CC := $(CROSS_COMPILE)gcc
endif
CFLAGS ?= -g -O0
# override: keep the warning flags when CFLAGS is given on the command line (buildroot)
override CFLAGS += -Wall -Werror

TARGET := aesdsocket
OBJS := aesdsocket.o

all: $(TARGET)

default: all

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) *.o

.PHONY: all default clean
