#!/usr/bin/env python

#  Copyright (C) 2013, Andreas Sandberg
#  All rights reserved.
# 
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
# 
#      * Redistributions of source code must retain the above copyright
#        notice, this list of conditions and the following disclaimer.
#      * Redistributions in binary form must reproduce the above
#        copyright notice, this list of conditions and the following
#        disclaimer in the documentation and/or other materials provided
#        with the distribution.
# 
# 
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
#  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
#  COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
#  INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
#  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
#  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
#  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
#  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
#  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
#  OF THE POSSIBILITY OF SUCH DAMAGE.

import argparse
import sys

import pirate

class CtrSample(object):
    def __init__(self, pb_sample):
        self.counters = pb_sample.ctr

    def add(self, dump):
        assert len(dump.counters) == len(self.counters)
        self.counters = [ a + b for a, b in zip(self.counters, dump.counters) ]

class Dump(object):
    def __init__(self, pb_dump):
        self.size = pb_dump.t_sample.size
        self.target = CtrSample(pb_dump.t_sample)
        self.pirates = [ CtrSample(p) for p in pb_dump.p_sample ]

    def add(self, dump):
        assert self.size == dump.size

        self.target.add(dump.target)
        for p_self, p_dump in zip(self.pirates, dump.pirates):
            p_self.add(p_dump)

    def print_csv(self, ofs=" "):
        fields = [ "%i" % self.size ]
        fields += [ "%li" % c for c in self.target.counters ]
        for p in self.pirates:
            fields += [ "%li" % c for c in p.counters ]

        print ofs.join(fields)
        

def print_header(header, comment="#", cur_field=1):
    fmt_entries = {
        "target_cpu" : header.t_setup.cpu,
        "target_sample_period" : header.t_setup.sample_period,
        "target_command" : header.t_setup.command,
        "cache_ways" : header.p_setup.ways,
        "cache_size" : header.p_setup.cache_size,
        "way_size" : header.p_setup.way_size,
        "stride" : header.p_setup.stride,
        "pirate_cpus" : ",".join([ str(c) for c in header.p_setup.cpu ]),
        "reference_size" : header.reference.size,
        "reference" : " ".join(["%li" % r for r in header.reference.ctr ]),
    }

    csv_head = [
        "%i: Target cache size" % cur_field,
        "",
        "Target:",
        "\tCommand: %(target_command)s",
        "\tCPU: %(target_cpu)i",
        "\tSample period: %(target_sample_period)i",
        "\tCounters:",
    ]

    cur_field += 1

    csv_head += [ "\t\t %i: %s" % (i + cur_field, ctr.name)
                  for (i, ctr) in enumerate(header.t_setup.ctr) ]
    cur_field += len(header.t_setup.ctr)

    csv_head += [
        "Pirate:",
        "\tWays: %(cache_ways)i",
        "\tCache size: %(cache_size)i",
        "\tWay size: %(way_size)i",
        "\tStride: %(stride)i",
        "\tCPU: %(pirate_cpus)s",
        "\tCounters:",
    ]
        
    csv_head += [ "\t\t %i: %s" % (i + cur_field, ctr.name)
                  for (i, ctr) in enumerate(header.p_setup.ctr) ]
    cur_field += len(header.p_setup.ctr)

    csv_head += [
        "\tReference size:\t%(reference_size)i",
        "\tReference:\t%(reference)s",
    ]

    for l in csv_head:
        print comment + " " + l % fmt_entries

def main():
    parser = argparse.ArgumentParser(
        description='Dump the contents of a pirate data file')
    parser.add_argument('log', metavar='LOG', type=argparse.FileType('r'),
                        help="Pirate log to analyze")

    parser.add_argument('--fs', metavar='FS', type=str, default=" ",
                        help="Output field separator")

    parser.add_argument('--no-header', action="store_true", default=False,
                        help="Don't include CSV header with field descriptions")

    parser.add_argument('--no-aggregate', action="store_true", default=False,
                        help="Don't sum counters")

    args = parser.parse_args()

    try:
        header = pirate.read_header(args.log)
        if not args.no_header:
            print_header(header)

        d_agg = {}
        for _d in pirate.stream_dumps(args.log):
            d = Dump(_d)
            if args.no_aggregate:
                d.print_csv(ofs=args.fs)
            elif d.size in d_agg:
                d_agg[d.size].add(d)
            else:
                d_agg[d.size] = d

        if not args.no_aggregate:
            sizes = d_agg.items()
            sizes.sort(key=lambda (size, dump): size)
            for size, dump in sizes:
                dump.print_csv(ofs=args.fs)
    except RuntimeError, e:
        print >> sys.stderr, "Failed to read pirate log: %s" % e
        sys.exit(2)

if __name__ == "__main__":
    main()
