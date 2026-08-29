# -*- coding: utf-8 -*-
b = open(r'C:\project\Miaopaper\Firmware\MiaoPaper.bin', 'rb').read()
print('bin size:', len(b))
print('last 16 bytes:', b[-16:].hex())

exp = (sum(b) + (0x20000 - len(b)) * 0xff) & 0xffff
print('expected exp =', exp, 'device crc = 5779, diff =', exp - 5779)

# Hypothesis: the last K bytes were NOT written (device has 0xFF there)
for K in range(1, 60):
    dev = (sum(b[:-K]) + K * 0xff + (0x20000 - len(b)) * 0xff) & 0xffff
    if dev == 5779:
        print('MATCH tail-missing K=%d, those bytes sum=%d' % (K, sum(b[-K:])))
        break
else:
    print('tail-missing no match')

# Hypothesis: the last K bytes became 0x00 (erased differently)
for K in range(1, 60):
    dev = (exp - sum(b[-K:])) & 0xffff
    if dev == 5779:
        print('MATCH tail-zero K=%d, sum of last %d = %d' % (K, K, sum(b[-K:])))
        break
else:
    print('tail-zero no match')

# Hypothesis: a single byte at some position differs by 171
# (device has byte X where file has X+171 or X-171)
diff = (exp - 5779) & 0xffff
print('single-byte diff needed (mod 65536):', diff)
