CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -ldw -lelf

dw.exe: dw.c
	$(CC) $(CFLAGS) -o dw.exe dw.c $(LDFLAGS)

clean:
	rm -f dw.exe

.PHONY: clean
