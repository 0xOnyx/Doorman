CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 \
          -I../../components/tc_bus/include -I../../components/netlog/include -I../../main

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

TESTS := test_ota_auth$(EXE) test_logbuf$(EXE) test_tc_proto$(EXE) test_tc_slicer$(EXE) test_tc_tx_policy$(EXE)

all: $(TESTS)

test_ota_auth$(EXE): test_ota_auth.c ../../main/ota_auth.c ../../main/ota_auth.h t.h
	$(CC) $(CFLAGS) -o $@ test_ota_auth.c ../../main/ota_auth.c

test_logbuf$(EXE): test_logbuf.c ../../components/netlog/logbuf.c ../../components/netlog/include/logbuf.h t.h
	$(CC) $(CFLAGS) -o $@ test_logbuf.c ../../components/netlog/logbuf.c

test_tc_proto$(EXE): test_tc_proto.c ../../components/tc_bus/tc_proto.c ../../components/tc_bus/include/tc_proto.h t.h
	$(CC) $(CFLAGS) -o $@ test_tc_proto.c ../../components/tc_bus/tc_proto.c

test_tc_slicer$(EXE): test_tc_slicer.c ../../components/tc_bus/tc_slicer.c ../../components/tc_bus/tc_proto.c ../../components/tc_bus/include/tc_slicer.h ../../components/tc_bus/include/tc_proto.h t.h
	$(CC) $(CFLAGS) -o $@ test_tc_slicer.c ../../components/tc_bus/tc_slicer.c ../../components/tc_bus/tc_proto.c

test_tc_tx_policy$(EXE): test_tc_tx_policy.c ../../components/tc_bus/tc_tx_policy.c ../../components/tc_bus/tc_proto.c ../../components/tc_bus/include/tc_tx_policy.h ../../components/tc_bus/include/tc_proto.h t.h
	$(CC) $(CFLAGS) -o $@ test_tc_tx_policy.c ../../components/tc_bus/tc_tx_policy.c ../../components/tc_bus/tc_proto.c

test: $(TESTS)
	./test_ota_auth$(EXE)
	./test_logbuf$(EXE)
	./test_tc_proto$(EXE)
	./test_tc_slicer$(EXE)
	./test_tc_tx_policy$(EXE)

clean:
	rm -f $(TESTS)

.PHONY: all test clean
