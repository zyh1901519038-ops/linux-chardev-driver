CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra

all: mydev_test

mydev_test: mydev_test.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f mydev_test

.PHONY: all clean
