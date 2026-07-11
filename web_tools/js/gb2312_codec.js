// Catalog title codec for MiaoPaper web uploader.
// Firmware stores raw title bytes + enc flag (0=ASCII, 1=GB2312).
// Device renders GB2312 bytes directly — upload MUST be real GB2312, not UTF-8.

function catalogTitleEncoding(title, enc) {
  enc = enc | 0;
  if (enc === 0 && /[^\x00-\x7F]/.test(title || '')) return 1;
  return enc;
}

function catalogBytesTrimZero(tb) {
  var n = tb.length;
  while (n > 0 && !tb[n - 1]) n--;
  return tb.subarray(0, n);
}

function catalogTitleFromNotify(tb, enc) {
  var bytes = catalogBytesTrimZero(tb);
  if (!bytes.length) return '';
  enc = enc | 0;
  if (enc === 1) {
    try { return new TextDecoder('gbk').decode(bytes); } catch (e) {}
  }
  try { return new TextDecoder('utf-8').decode(bytes); } catch (e) {}
  var s = '', i;
  for (i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return s;
}

function catalogTitleToBytesAscii(title, maxLen) {
  var out = [], i, c;
  for (i = 0; i < title.length && out.length < maxLen; i++) {
    c = title.charCodeAt(i);
    if (c < 128) out.push(c);
  }
  return new Uint8Array(out);
}

function catalogAppendGb2312(out, gb, maxLen) {
  if (out.length >= maxLen) return false;
  if (gb <= 0xFF) {
    out.push(gb);
    return true;
  }
  if (out.length + 2 > maxLen) return false;
  out.push((gb >> 8) & 0xFF, gb & 0xFF);
  return true;
}

function catalogTitleToBytesGbk(title, maxLen) {
  if (typeof gb2312EncodeChar !== 'function') return null;
  var out = [], i, cp, gb;
  title = title || '';
  maxLen = maxLen || 19;
  for (i = 0; i < title.length; ) {
    cp = title.codePointAt(i);
    if (cp < 0x80) {
      if (out.length >= maxLen) break;
      out.push(cp);
    } else {
      gb = gb2312EncodeChar(cp);
      if (gb === null) break;
      if (!catalogAppendGb2312(out, gb, maxLen)) break;
    }
    i += cp > 0xFFFF ? 2 : 1;
  }
  return new Uint8Array(out);
}

function catalogTitleToBytes(title, enc, maxLen) {
  enc = enc | 0;
  title = title || '';
  maxLen = maxLen || 19;
  if (enc === 0) return catalogTitleToBytesAscii(title, maxLen);
  var gbk = catalogTitleToBytesGbk(title, maxLen);
  if (gbk) return gbk;
  return catalogTitleToBytesAscii(title, maxLen);
}

function catalogTitleToBytesAsync(title, enc, maxLen) {
  return Promise.resolve(catalogTitleToBytes(title, enc, maxLen));
}
