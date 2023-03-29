CFLAGS += -ffunction-sections -fdata-sections -fPIC -std=gnu99
LDFLAGS += -Wl,--gc-sections
all : main.o Makefile
	$(CC) -o helloylt main.o $(LDFLAGS)
main.o : main.c
	$(CC) -c main.c $(CFLAGS)
clean :
	rm -f main.o
clean-all :
	rm -f helloylt *.o
romfs:
	$(ROMFSINST) helloylt /bin/helloylt
