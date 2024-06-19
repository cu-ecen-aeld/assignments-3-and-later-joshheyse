SRC := aesdsocket.c
TARGET ?= aesdsocket
CC ?= $(CROSS_COMPILE)gcc
LDFLAGS ?= -lpthread -lrt
CFLAGS ?= -Wall -Werror -g

OBJS := $(SRC:.c=.o)

$(info CROSS_COMPILE is $(CROSS_COMPILE))
$(info CC is $(CC))

all: $(TARGET)

$(TARGET) : $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) $(OBJS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f *.o $(TARGET) *.elf *.map
