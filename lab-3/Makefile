MPICC ?= mpicc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra

# Apple clang has no built-in OpenMP: use Homebrew's libomp on macOS.
# Linux/CAAS (gcc) supports -fopenmp directly.
ifeq ($(shell uname -s),Darwin)
LIBOMP ?= $(shell brew --prefix libomp 2>/dev/null)
OMPFLAGS = -Xpreprocessor -fopenmp -I$(LIBOMP)/include
OMPLIBS = -L$(LIBOMP)/lib -lomp
else
OMPFLAGS = -fopenmp
OMPLIBS =
endif

.PHONY: all clean
all: lab3_wsn

lab3_wsn: lab3_wsn.c
	$(MPICC) $(CFLAGS) $(OMPFLAGS) $< -o $@ $(OMPLIBS)

clean:
	rm -f lab3_wsn *.log
