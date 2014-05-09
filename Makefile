CC=gcc
LD=gcc
CXX=g++

LIBS=-lpthread -lm -lprotobuf

ifneq ($(wildcard /usr/include/perfmon/pfmlib_perf_event.h),)
LIBS+=-lpfm
else ifneq ($(wildcard /usr/local/include/perfmon/pfmlib_perf_event.h),)
LIBS+=-L/usr/local/lib -lpfm
else
$(error Can't find libpfm4)
endif


CFLAGS=-g -O2 -fno-strict-aliasing -Wall -std=gnu99
CXXFLAGS=-g -O2 -fno-strict-aliasing -Wall

all: perfpirate python

python/%_pb2.py: %.proto
	protoc --python_out=python/ $^

%.pb.cc %.pb.h: %.proto
	protoc --cpp_out=. $^


perf_data.o: perf_data.cc expect.h perf_common.h perfpirate.h perf_data.h perf_pb.pb.h
perf_pirate.o: perf_pirate.c expect.h perf_common.h perfpirate.h perf_data.h perf_pb.pb.h

perfpirate: perfpirate.o perf_common.o perf_data.o perf_pb.pb.o
	$(CXX) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

python: python/perf_pb_pb2.py

clean:
	$(RM) *.o *.pb.* perfpirate python/*_pb2.py python/*.pyc

.PHONY: all clean python
