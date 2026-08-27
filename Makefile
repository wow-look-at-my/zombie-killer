CC      ?= cc
GO      ?= go

all: gozer makezombies

gozer: main.go
	$(GO) build -o gozer main.go

makezombies: test/makezombies.c
	$(CC) -O2 -Wall -o makezombies test/makezombies.c

test: gozer makezombies
	./test/run-test.sh

clean:
	rm -f gozer makezombies

.PHONY: all test clean
