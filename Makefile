RPM_OPT_FLAGS ?= -O2 -g -Wall
all: lz4reader
lz4reader: main.o liblz4reader.a
	$(CC) $(RPM_OPT_FLAGS) -o $@ $^ -llz4
liblz4reader.a: lz4reader.o
	$(AR) r $@ $^
main.o: main.c lz4reader.h
	$(CC) $(RPM_OPT_FLAGS) -c $<
lz4reader.o: lz4reader.c lz4reader.h
	$(CC) $(RPM_OPT_FLAGS) -c $<
clean:
	rm -f lz4reader.o liblz4reader.a main.o lz4reader
