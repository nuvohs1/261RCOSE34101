CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -g
TARGET  = cpu_scheduler
SRCS    = cpu_scheduler.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
