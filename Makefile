CFLAGS += -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE
LDLIBS += -lmpsse

all: oz890

oz890: oz890.o

.PHONY:	clean

clean:
	rm -rf *.o
	rm -rf oz890

