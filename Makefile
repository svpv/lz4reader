RPM_OPT_FLAGS ?= -O2 -g -Wall
READA_DIR = ../reada
all: lz4reader
lz4reader: main.o lz4reader.o reada.o
	$(CC) $(RPM_OPT_FLAGS) -o $@ $^ -llz4
main.o: main.c lz4reader.h $(READA_DIR)/reada.h
	$(CC) $(RPM_OPT_FLAGS) -I$(READA_DIR) -c $<
lz4reader.o: lz4reader.c lz4reader.h $(READA_DIR)/reada.h
	$(CC) $(RPM_OPT_FLAGS) -I$(READA_DIR) -c $<
reada.o: $(READA_DIR)/reada.c $(READA_DIR)/reada.h
	$(CC) $(RPM_OPT_FLAGS) -I$(READA_DIR) -c $<
clean:
	rm -f lz4reader main.o lz4reader.o reada.o
check: lz4reader
	true        |lz4  >test.lz4
	echo -n foo |lz4 >>test.lz4
	true        |lz4 >>test.lz4
	echo -n bar |lz4 >>test.lz4
	echo    baz |lz4 >>test.lz4
	test `./lz4reader <test.lz4` = foobarbaz
	rm test.lz4
