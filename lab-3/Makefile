MPICC ?= mpicc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -fopenmp

.PHONY: all clean
all: lab3_wsn

lab3_wsn: lab3_wsn.c
	$(MPICC) $(CFLAGS) $< -o $@

clean:
	rm -f lab3_wsn *.log
