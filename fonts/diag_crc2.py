# -*- coding: utf-8 -*-
b = open(r'C:\project\Miaopaper\Firmware\MiaoPaper.bin', 'rb').read()
N = len(b)
exp = (sum(b) + (0x20000 - N) * 0xff) & 0xffff
TARGET = 5779
print('N=%d expected=%d target=%d' % (N, exp, TARGET))

def crc_with_replaced(start, k, value):
    # replace b[start:start+k] with value bytes; keep everything else
    s = exp
    for i in range(start, min(start + k, N)):
        s = (s - b[i] + value) & 0xffff
    return s

# 1) a single dropped 0x03 chunk of size 19 (replaced by 0xFF)
for P in range(0, N - 19 + 1):
    if crc_with_replaced(P, 19, 0xff) == TARGET:
        print('DROPPED 19B chunk at pos %d (0x%X), file bytes: %s' % (P, P, b[P:P+19].hex()))
        break
else:
    print('no 19B-drop match')

# 2) a single dropped chunk of size 9
for P in range(0, N - 9 + 1):
    if crc_with_replaced(P, 9, 0xff) == TARGET:
        print('DROPPED 9B chunk at pos %d (0x%X), file bytes: %s' % (P, P, b[P:P+9].hex()))
        break
else:
    print('no 9B-drop match')

# 3) last-K bytes replaced by 0xFF, K up to 300
for K in range(1, 301):
    if crc_with_replaced(N - K, K, 0xff) == TARGET:
        print('TAIL missing K=%d, bytes: %s sum=%d' % (K, b[N-K:].hex(), sum(b[N-K:])))
        break
else:
    print('no tail match')

# 4) last-K bytes replaced by 0x00
for K in range(1, 301):
    if crc_with_replaced(N - K, K, 0x00) == TARGET:
        print('TAIL zero K=%d, bytes: %s sum=%d' % (K, b[N-K:].hex(), sum(b[N-K:])))
        break
else:
    print('no tail-zero match')

# 5) dump the very last 256 bytes to eyeball
print('last 256 bytes:', b[-256:].hex())
