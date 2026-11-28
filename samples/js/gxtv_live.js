/**
 * 广西台：清单可直拉，TS 分片 PES 中段 AES-128-ECB + 分块轮转。
 * 对齐 plugin-lua entry.lua gxtv_live / gx_decrypt。
 * 分片必须 ms.getbin（ArrayBuffer）；ms.get 当 UTF-8 会毁掉 0x47 同步字节。
 *
 * 频道：asset:///js/gxtv_live.js?id=gxws
 */
var GXTV_IDS = {
  gxws: "1",
  gxzy: "2",
  gxds: "3",
  gxys: "4",
  gxxw: "5",
  gxgj: "6",
  gxls: "7",
  gxyd: "8",
  cetv1: "9",
  cetv2: "10",
  cetv4: "11"
};

var GXTV_CH = [
  null,
  ["0c4ef3a44b934cacb8b47121dfada66c/d7e04258157b480dae53883cc6f8123b", "aa390855e94889d26ccf2c5a0c342e73", 18],
  ["de0f97348eb84f62aa6b7d8cf0430770/dd505d87880c478f901f38560ca4d4e6", "59ccd582591a61e35a2434df51d8e697", 26],
  ["b8f4e500a4024fd2bf189b46f490359f/b04d249044fb4d0887b88aa9c2cc8f6c", "3d4a0a74ee9af217ff63c7bf7bfa4f91", 17],
  ["a84182dabc5147afbd3d90ddbb5a9404/d097f6c24c53463e897de496b32c7d2b", "328d87240d5c25c1f412a08be81a1649", 4],
  ["a48635e37ac84afa82c0d0edc4bfabf9/dbc9a18971294257bad7c75b7f3f0c20", "c21df4dd9cbd339b20b6e435a62f10e3", 4],
  ["0234c48e0bc24fe1b41b9999a253e581/1075ee38e04f490690f6a36a16e09c79", "4297eb2b6d538f7bee595e70b35289fb", 24],
  ["2cb851292fd14014a6558343872899e6/0820054f3fcc4ee5b4d17198bd7eddd6", "1ec32c4e7960167d4d9679d2ef5f7265", 16],
  ["b6cea70bfad24970aaa2256a3c340ad4/0a79a8e5f94641e583d1872ef7bed2bf", "5c1d834a84f3ff24720622105b5cddfe", 9],
  ["ddb2ee1aa1134ac591230352a121aa22/bc359bd2e13b4cb9a3096effa77d1bc0", "b324a5b0682ea911d1ccc18ebc1c0cba", 11],
  ["3f29b81206fe4d229e1522d59aae8e75/15a4a13dbf624ab9ac7cca5df100e985", "7a63f3adc0ebbf30eab427a9846cf8be", 7],
  ["63f3fd7d8cf44a3e9719eec310c86fa5/b96f8fd6d5424ad4862d054172f616e4", "86556718d2add26ff9136a1af241b3db", 11]
];

function main(item) {
  var id = ms.getQuery(item.url, "id") || item.id || "";
  if (GXTV_IDS[id]) id = GXTV_IDS[id];
  var c = GXTV_CH[parseInt(id, 10) || 0];
  if (!c) throw new Error("gxtv bad id=" + id);
  return {
    url: "https://hlscdn.liangtv.cn/live/" + c[0] + "-1.m3u8",
    referer: "https://www.gxtv.cn/",
    userAgent: "Mozilla/5.0",
    stream: true,
    streamArgs: { keyHex: c[1], blocks: c[2], referer: "https://www.gxtv.cn/" }
  };
}

function asU8(x) {
  if (!x) return new Uint8Array(0);
  if (x instanceof Uint8Array) return x;
  if (x instanceof ArrayBuffer) return new Uint8Array(x);
  if (typeof x === "string") {
    var a = new Uint8Array(x.length),
      i;
    for (i = 0; i < x.length; i++) a[i] = x.charCodeAt(i) & 0xff;
    return a;
  }
  return new Uint8Array(0);
}

function catU8(parts) {
  var n = 0,
    i,
    o = 0,
    out;
  for (i = 0; i < parts.length; i++) n += parts[i].length;
  out = new Uint8Array(n);
  for (i = 0; i < parts.length; i++) {
    out.set(parts[i], o);
    o += parts[i].length;
  }
  return out;
}

function gx_pes(l, key, blocks) {
  var t = (l.length / blocks) | 0;
  var h = (t / 2) | 0;
  var r = ((l.length - h - 1) / 16) | 0;
  var d;
  if (r > 0) {
    d = asU8(
      ms.cipher("aes-128-ecb", "dec", l.subarray(h, h + 16 * r), key, "", {
        padding: false
      })
    );
    if (!d.length) return null;
    l = catU8([l.subarray(0, h), d, l.subarray(h + 16 * r)]);
  }
  if (t === 0) return l;
  return catU8([
    l.subarray(0, t),
    l.subarray((blocks - 1) * t, blocks * t),
    l.subarray(t, (blocks - 1) * t),
    l.subarray(blocks * t)
  ]);
}

function gx_decrypt(s, key, blocks) {
  s = asU8(s);
  key = asU8(key);
  var n = s.length - (s.length % 188);
  function payload(p) {
    var afc = (s[p + 3] & 48) >> 4;
    if (afc < 2) return p + 4;
    var h = p + 5 + (s[p + 4] & 0xff);
    return h < p + 188 ? h : -1;
  }
  var pmt = -1;
  var want = null;
  var p, h, pid, e, ty;
  for (p = 0; p < n; p += 188) {
    if (s[p] !== 0x47) return null;
    pid = ((s[p + 1] & 31) << 8) | (s[p + 2] & 0xff);
    h = payload(p);
    if (h < 0) continue;
    if ((s[p + 1] & 64) === 0) continue;
    if (pid !== 0 && pid !== pmt) continue;
    h = h + (s[h] & 0xff) + 1;
    if (pid === 0) {
      pmt = ((s[h + 10] & 31) << 8) | (s[h + 11] & 0xff);
    } else {
      want = {};
      e = h + 3 + (((s[h + 1] & 15) << 8) | (s[h + 2] & 0xff)) - 4;
      h = h + 12 + (((s[h + 10] & 15) << 8) | (s[h + 11] & 0xff));
      while (h < e) {
        ty = s[h] & 0xff;
        if (ty === 15 || ty === 21 || ty === 27) {
          want[((s[h + 1] & 31) << 8) | (s[h + 2] & 0xff)] = true;
        }
        h = h + 5 + (((s[h + 3] & 15) << 8) | (s[h + 4] & 0xff));
      }
      break;
    }
  }
  if (!want) return s;

  var rep = {};
  var cur = {};
  function flush(arr) {
    if (!arr || !arr.length) return;
    var parts = [],
      i,
      st,
      len,
      skip,
      pkt,
      pos,
      f,
      u,
      plen,
      raw,
      l;
    for (i = 0; i < arr.length; i += 2) {
      parts.push(s.subarray(arr[i], arr[i] + arr[i + 1]));
    }
    raw = catU8(parts);
    f = raw[7] || 0;
    u = (raw[8] || 0) + 9;
    plen = ((raw[4] || 0) << 8) | (raw[5] || 0);
    if (
      raw.length < 9 ||
      raw[0] !== 0 ||
      raw[1] !== 0 ||
      raw[2] !== 1 ||
      (plen !== 0 && plen > raw.length - 6) ||
      (f & 1) === 0 ||
      raw.length <= u
    ) {
      return;
    }
    l = gx_pes(raw.subarray(u), key, blocks);
    if (!l) return;
    pos = 0;
    for (i = 0; i < arr.length; i += 2) {
      st = arr[i];
      len = arr[i + 1];
      skip = Math.max(0, Math.min(len, u - pos));
      if (skip < len) {
        pkt = st - (st % 188);
        rep[pkt] = catU8([
          s.subarray(pkt, st + skip),
          l.subarray(pos + skip - u, pos + len - u)
        ]);
      }
      pos += len;
    }
  }

  for (p = 0; p < n; p += 188) {
    pid = ((s[p + 1] & 31) << 8) | (s[p + 2] & 0xff);
    h = payload(p);
    if (h < 0 || !want[pid]) continue;
    if ((s[p + 1] & 64) !== 0) {
      flush(cur[pid]);
      cur[pid] = [];
    }
    if (cur[pid]) {
      cur[pid].push(h, p + 188 - h);
    }
  }
  for (pid in cur) {
    if (cur.hasOwnProperty(pid)) flush(cur[pid]);
  }
  var has = false;
  for (p in rep) {
    if (rep.hasOwnProperty(p)) {
      has = true;
      break;
    }
  }
  if (!has) return s;
  var outParts = [];
  for (p = 0; p < n; p += 188) {
    outParts.push(rep[p] !== undefined ? rep[p] : s.subarray(p, p + 188));
  }
  if (n < s.length) outParts.push(s.subarray(n));
  return catU8(outParts);
}

function stream_call(url, args) {
  var u = String(url || "");
  if (/\.m3u8(\?|$)/i.test(u)) {
    return null;
  }
  var hdr = { "User-Agent": "Mozilla/5.0" };
  if (args && args.referer) hdr.Referer = args.referer;
  var raw = ms.getbin(u, hdr);
  if (!raw || (raw.byteLength !== undefined && raw.byteLength === 0)) {
    throw new Error("gxtv empty seg");
  }
  var key = ms.hex_decode((args && args.keyHex) || "");
  var blocks = (args && args.blocks) | 0;
  if (!key || key.byteLength === 0 || blocks <= 0) return raw;
  var plain = gx_decrypt(raw, key, blocks);
  return plain || raw;
}
