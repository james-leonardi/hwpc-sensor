CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -ldw -lelf

dw-pid.exe: dw-pid.c
	$(CC) $(CFLAGS) -o dw-pid.exe dw-pid.c $(LDFLAGS)

dw.exe: dw.c
	$(CC) $(CFLAGS) -o dw.exe dw.c $(LDFLAGS)

clean:
	rm -f dw.exe

.PHONY: clean
