# -*- coding: utf-8 -*-
b = open(r'C:\project\Miaopaper\Firmware\MiaoPaper.bin', 'rb').read()
N = len(b)
exp = (sum(b) + (0x20000 - N) * 0xff) & 0xffff
TARGET = 5779

# single byte: find positions where changing b[P] to some value gives TARGET
# diff contribution = (new - old)
need = (TARGET - exp) & 0xffff  # how much the sum must change mod 65536
print('sum change needed (mod 65536):', need)
# a single byte change of delta: delta in [-255, 255]; delta mod 65536 == need
deltas = []
for delta in range(-255, 256):
    if (delta & 0xffff) == need:
        deltas.append(delta)
print('possible single-byte deltas:', deltas)
for delta in deltas:
    # b[P] must be in [max(0,-delta), min(255,255-delta)]
    lo = max(0, -delta)
    hi = min(255, 255 - delta)
    found = False
    for P in range(N):
        if lo <= b[P] <= hi:
            newv = b[P] + delta
            print('single byte fix: pos %d (0x%X) file=0x%02X -> device=0x%02X' % (P, P, b[P], newv))
            found = True
            break
    if not found:
        print('delta %d: no position fits' % delta)

# two-byte: any pair summing to need (within -510..510)
print('--- two-byte pairs ---')
count = 0
for P1 in range(N):
    for P2 in range(P1+1, N):
        # delta1+delta2 ≡ need (mod 65536); allow small range
        for d1 in range(-10, 11):
            d2 = need - d1
            if d2 < -10 or d2 > 10:
                continue
            if 0 <= b[P1]+d1 <= 255 and 0 <= b[P2]+d2 <= 255:
                print('two-byte: pos %d 0x%02X->0x%02X, pos %d 0x%02X->0x%02X' % (P1, b[P1], b[P1]+d1, P2, b[P2], b[P2]+d2))
                count += 1
                break
        if count >= 5:
            break
    if count >= 5:
        break
if count == 0:
    print('no small two-byte pair found')
