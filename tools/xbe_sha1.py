"""the sha1 an xbe is known by: its certificate's allowed media (+0x9c) and region (+0xa0) set back to the us
release's, as dump tools patch them for region-free, any-media play (the code and data aren't touched, and
game_files.cpp hashes the same way)"""
import hashlib, struct, sys

data = bytearray(open(sys.argv[1], 'rb').read())
base, cert = struct.unpack_from('<I', data, 0x104)[0], struct.unpack_from('<I', data, 0x118)[0]
off = cert - base
if data[:4] == b'XBEH' and 0 <= off and off + 0xa4 <= len(data):
    struct.pack_into('<II', data, off + 0x9c, 0x202, 0x1)
print(hashlib.sha1(data).hexdigest())
