# -*- coding: utf-8 -*-
"""
mp_scan.py — 扫描广播,实时显示 mp_sleeptest 诊断字节
用法:  python mp_scan.py          (Ctrl+C 退出)
依赖:  pip install bleak
广播厂商字段: CID=0x504D('M','P') + [cnt, el_hi, el_lo, mark, w44]
  cnt =启动计数(狗/软复位清零它)
  el  =上次入睡→本次开机真实断电秒数
  mark=5A → 真 0x80 掉电后唤醒; 00 → 中途被复位打断
  w44 bit3(0x08) → pad 按键唤醒
"""
import asyncio
from bleak import BleakScanner

MP_CID = 0x504D          # 'M','P' 小端
last_line = None

def cb(device, ad):
    global last_line
    md = ad.manufacturer_data or {}
    if MP_CID not in md:
        return
    b = bytes(md[MP_CID])
    if len(b) < 5:
        return
    cnt  = b[0]
    el   = (b[1] << 8) | b[2]
    mark = b[3]
    w44  = b[4]
    flags = []
    if w44 & 0x08: flags.append("PAD!")
    if w44 & 0x04: flags.append("core")
    if w44 & 0x02: flags.append("timer")
    out = "[%s] cnt=%-3d sleep=%-5ds mark=%-9s w44=%02X %s" % (
        device.address[-5:], cnt, el,
        "5A(真掉电)" if mark == 0x5A else "%02X" % mark, w44, " ".join(flags))
    if out != last_line:
        last_line = out
        print(out, flush=True)

async def main():
    print("扫描中(设备 8s 无连接会断电消失;按 F 唤醒重新出现)...Ctrl+C 退出", flush=True)
    async with BleakScanner(detection_callback=cb) as sc:
        await asyncio.Event().wait()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n结束")
