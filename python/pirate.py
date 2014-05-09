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
 

import sys
import struct
from perf_pb_pb2 import *

def _read_magic(f, magic):
    """Read and compare the magic value in a data file.

    Arguments:
      f - Input file.
      magic - Magic value"

    Returns:
      True on success, False otherwise.
    """
    f_magic = f.read(len(magic))
    return f_magic == magic

def _read_entry(f, type):
    """Read a protobuf entry from a pirate log file.

    The pirate stores protobuf messages with the length prepended to
    the message as an uin32_t. This function first reads the uint32_t
    to determine the length of the entry and then the message
    itself. The message is then parsed using the appropriate protobuf
    class.

    Arguments:
       f - Input file.
       type - Protobuf entry type.

    Returns:
       Message object on success, None on orderly EOF.

    Exceptions:
       RuntimeError on EOF in the middle of a message.
    """

    # Read the entry length (uint32_t)
    packet_head = f.read(4)
    if len(packet_head) == 0:
        return None
    elif len(packet_head) != 4:
        raise RuntimeError('Unexpected EOF while reading packet length')

    (length, ) = struct.unpack("@I", packet_head)

    pb_message = type()
    packet = f.read(length)
    if len(packet) == 0:
        return None
    elif len(packet) != length:
        raise RuntimeError('Unexpected EOF while reading packet')

    pb_message.ParseFromString(packet)

    return pb_message

def read_header(fin):
    """Read the header structure from a pirate log file.

    Arguments:
       fin - Input file.

    Returns:
       PerfHeader object.

    Exceptions:
       RuntimeError on EOF.
    """
    if not _read_magic(fin, "PIRATEv1"):
        raise RuntimeError("Invalid magic in file header")

    header = _read_entry(fin, PerfHeader)
    if header is None:
        raise RuntimeError("Failed to read header")

    return header

def stream_dumps(fin):
    """Stream dumps from a pirate log file.

    This function must be called after read_header(), which sets the
    file pointer to the first dump in the file.

    Arguments:
       fin - Input file.

    Exceptions:
       RuntimeError on EOF in the middle of a message.
    """
    while True:
        dump = _read_entry(fin, PerfCtrDump)
        if dump is None:
            break
        yield dump
