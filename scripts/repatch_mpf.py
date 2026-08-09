"""Recomputes the MPF sizes and offsets of a two-image file after its segments
have been edited. Offsets are measured from the first byte of the MP Endian
field, not from the start of the file."""
import sys, struct

data = bytearray(open(sys.argv[1], 'rb').read())
i = data.find(b'MPF\x00')
base = i + 4
count = struct.unpack('>I', data[base + 30:base + 34])[0]
entries = base + 4 + 8 + (2 + 3*12 + 4) - 4

# Image boundaries: the primary ends at the first EOI that a SOI follows.
imgs, j = [0], 0
while True:
    k = data.find(b'\xff\xd8', j + 2)
    if k < 0: break
    if bytes(data[k-2:k]) == b'\xff\xd9': imgs.append(k)
    j = k
imgs.append(len(data))

for n in range(count):
    start, end = imgs[n], imgs[n + 1]
    struct.pack_into('>I', data, entries + n*16 + 4, end - start)
    struct.pack_into('>I', data, entries + n*16 + 8, 0 if n == 0 else start - base)
open(sys.argv[2], 'wb').write(bytes(data))
print(f"repatched {count} MPF entries over {len(imgs)-1} images")
