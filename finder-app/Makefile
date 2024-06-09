writer: writer.c
	echo ${CROSS_COMPILE}
	${CROSS_COMPILE}gcc -o writer writer.c -Wall -Werror

all: writer

.PHONY: clean
clean:
	rm -f writer
	rm -f *.o
