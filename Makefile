CFLAGS += -Wall -Wextra -std=c11
LDLIBS += -lmpsse

all: oz890

oz890: oz890.o

.PHONY:	clean

clean:
	rm -rf *.o
	rm -rf oz890

