.PHONY: clean run

all: help

help: 
	@echo make [ clean macpaste run ]

clean:
	-rm -f macpaste

macpaste: macpaste.c
	gcc -O2 -Wall -Wextra -framework ApplicationServices -o macpaste macpaste.c

run:
	./macpaste
