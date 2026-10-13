CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 \
          -I../../components/tc_bus/include -I../../components/netlog/include -I../../main

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

TESTS := test_ota_auth$(EXE) test_logbuf$(EXE) test_tc_proto$(EXE)

all: $(TESTS)

test_ota_auth$(EXE): test_ota_auth.c ../../main/ota_auth.c t.h
	$(CC) $(CFLAGS) -o $@ test_ota_auth.c ../../main/ota_auth.c

test_logbuf$(EXE): test_logbuf.c ../../components/netlog/logbuf.c t.h
	$(CC) $(CFLAGS) -o $@ test_logbuf.c ../../components/netlog/logbuf.c

test_tc_proto$(EXE): test_tc_proto.c ../../components/tc_bus/tc_proto.c t.h
	$(CC) $(CFLAGS) -o $@ test_tc_proto.c ../../components/tc_bus/tc_proto.c

test: $(TESTS)
	./test_ota_auth$(EXE)
	./test_logbuf$(EXE)
	./test_tc_proto$(EXE)

clean:
	rm -f $(TESTS)

.PHONY: all test clean
