writer: writer.c
	${CROSS_COMPILE}gcc -o writer writer.c -Wall -Werror

all: writer

.PHONY: clean
clean:
	rm -f writer
	rm -f *.o
