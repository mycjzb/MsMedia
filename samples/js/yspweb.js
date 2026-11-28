/**
 * 央视频 Web 播放（对齐 plugin-lua ysp_web）。
 * 频道：…/yspweb.js?id=cnlid|livepid|defn
 * 返回 A：{ url, referer, userAgent }（按需带 ysp-tea=）。
 */
function main(item) {
  var UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/106.0.0.0 Safari/537.36";
  var ORIGIN = "https://www.yangshipin.cn";
  var PLAYER = "https://player-api.yangshipin.cn";
  var APPID = "519748109";
  var PLATFORM = "5910204";
  var AUTH_APPID = "ysp_pc";
  var AUTH_SALT = "n@7QKk%YeSjfw%22";
  var LIVE_SALT = "0f$IVHi9Qno?G";
  var AES_KEY = ms.hex_decode("48e5918a74ae21c972b90cce8af6c8be");
  var AES_IV = ms.hex_decode("9a7e7d23610266b1d9fbf98581384d92");
  var VAPPID = "59306155";
  var VSECRET = "b42702bf7309a179d102f3d51b1add2fda0bc7ada64cb801";
  var TICKET_KEY = ms.hex_decode(
    "6a70e9ac007ccc384c28a88dfd2211decb66494bca83bd6178f85732b2eacc9594e4e5ab720ec3a4" +
      "3e87b3c72b86192ff0dfea8fa8f47794d8792668438c"
  );
  var SDK_KEYS = [
    "adjust", "app_version", "appVer", "channel", "cKey", "cmd", "cnlid", "defn",
    "devid", "dtype", "encryptVer", "guid", "livepid", "otype", "platform",
    "sphttps", "stream"
  ];
  var HEX = "0123456789abcdef";

  /* hex_decode/aes/random → ArrayBuffer（勿当 UTF-8 字符串） */
  function u8(x) {
    if (!x) return new Uint8Array(0);
    if (x instanceof ArrayBuffer) return new Uint8Array(x);
    if (x instanceof Uint8Array) return x;
    if (typeof x.length === "number" && typeof x.byteLength === "number")
      return new Uint8Array(x.buffer, x.byteOffset, x.byteLength);
    if (typeof x === "string") {
      var a = new Uint8Array(x.length),
        i;
      for (i = 0; i < x.length; i++) a[i] = x.charCodeAt(i) & 0xff;
      return a;
    }
    return new Uint8Array(0);
  }
  function q(k) {
    return ms.getQuery(item.url, k) || "";
  }
  /* 短名 → cnlid|livepid|defn（对齐超级直播 ysp ids 常用台） */
  var IDS = {
    cctv1: "2024078201|600001859|fhd",
    cctv2: "2024075401|600001800|fhd",
    cctv3: "2024068501|600001801|fhd",
    cctv4: "2029797101|600001814|fhd",
    cctv5: "2024078401|600001818|fhd",
    cctv5p: "2024078001|600001817|fhd",
    cctv6: "2013693901|600108442|fhd",
    cctv13: "2029797201|600001811|fhd",
    hnws: "2024054803|600002475|fhd",
    dfws: "2024054503|600002483|fhd",
    zjws: "2024054703|600002520|fhd",
    bjws: "2024052703|600002309|fhd"
  };
  var mapped = q("id") || item.id || item.name || "";
  if (mapped && mapped.indexOf("|") < 0 && IDS[mapped]) {
    mapped = IDS[mapped];
  }
  var cnlid = q("cnlid"),
    livepid = q("livepid"),
    defn = q("defn") || "fhd";
  if (mapped.indexOf("|") >= 0) {
    var p = mapped.split("|");
    cnlid = cnlid || p[0] || "";
    livepid = livepid || p[1] || "";
    if (p[2]) defn = p[2];
  }
  if (!cnlid || !livepid) throw new Error("yspweb need cnlid|livepid|短名");

  function randStr(n) {
    var a = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    var o = "",
      i,
      r = u8(ms.random_bytes(n));
    for (i = 0; i < n; i++) o += a.charAt((r[i] || 0) % a.length);
    return o;
  }
  function javaHash(s) {
    var h = 0,
      i;
    for (i = 0; i < s.length; i++) h = (Math.imul(31, h) + s.charCodeAt(i)) | 0;
    return h;
  }
  function hexUpper(bin) {
    var h = "",
      i,
      v,
      b = u8(bin);
    for (i = 0; i < b.length; i++) {
      v = b[i] & 0xff;
      h += (v < 16 ? "0" : "") + v.toString(16).toUpperCase();
    }
    return h;
  }
  function cKey(ts, guid) {
    var raw =
      "|" +
      cnlid +
      "|" +
      ts +
      "|mg3c3b04ba|V1.0.0|" +
      guid +
      "|" +
      PLATFORM +
      "|https://www.yangshipin.c|mozilla/5.0 (windows nt ||Mozilla|Netscape|Win32|";
    var plain = "|" + String(javaHash(raw)) + raw;
    var enc = ms.aes_cbc_encrypt(plain, AES_KEY, AES_IV);
    if (!enc) throw new Error("cKey aes fail");
    return "--01" + hexUpper(enc);
  }
  function cookie(guid, requestId) {
    var b =
      "guid=" +
      guid +
      "; versionName=99.99.99; versionCode=999999; vplatform=109" +
      "; platformVersion=Chrome; deviceModel=152; newLogin=1; pc_version=1.1.16; ysp_uinfo_pc=";
    if (requestId) b += "; nseqId=1; nrequest-id=" + requestId;
    return b;
  }
  function apiHdr(guid, contentType, requestId, extra) {
    var h = {
      "User-Agent": UA,
      Accept: "application/json, text/plain, */*",
      Origin: ORIGIN,
      Referer: ORIGIN + "/",
      Cookie: cookie(guid, requestId),
      yspappid: APPID
    };
    var k;
    if (contentType) h["Content-Type"] = contentType;
    if (extra) for (k in extra) if (extra.hasOwnProperty(k)) h[k] = extra[k];
    return h;
  }
  function liveSign(fields) {
    var keys = [],
      k,
      i,
      parts = [];
    for (k in fields) if (fields.hasOwnProperty(k)) keys.push(k);
    keys.sort();
    for (i = 0; i < keys.length; i++) parts.push(keys[i] + "=" + fields[keys[i]]);
    return ms.md5(parts.join("&") + LIVE_SALT);
  }
  function jsonBody(fields, sign) {
    var keys = [],
      k,
      i,
      parts = [];
    for (k in fields) if (fields.hasOwnProperty(k)) keys.push(k);
    keys.sort();
    for (i = 0; i < keys.length; i++) {
      k = keys[i];
      if (k === "adjust") parts.push('"' + k + '":' + fields[k]);
      else
        parts.push(
          '"' + k + '":"' + String(fields[k]).replace(/\\/g, "\\\\").replace(/"/g, '\\"') + '"'
        );
    }
    parts.push('"signature":"' + sign + '"');
    return "{" + parts.join(",") + "}";
  }
  /** Lua：md5(key=value&…) */
  function sdkInputMd5(fields) {
    var i,
      parts = [];
    for (i = 0; i < SDK_KEYS.length; i++) parts.push(SDK_KEYS[i] + "=" + (fields[SDK_KEYS[i]] || ""));
    return ms.md5(parts.join("&"));
  }
  function sdkSign(guid, token, input) {
    return ms.md5(
      guid + ";www.yangshipin.cn;" + token + ";https:;" + APPID + ";" + input + ";"
    );
  }
  /** XOR + 小写 hex（非 AES） */
  function ticket(pid, ts, guid, rnd) {
    var p = pid + "&" + ts + "&" + guid + "&" + APPID + "&" + rnd;
    var out = "",
      i,
      b,
      tk = u8(TICKET_KEY);
    for (i = 0; i < p.length; i++) {
      b = (p.charCodeAt(i) ^ (tk[i] || 0)) & 0xff;
      out += HEX.charAt(b >> 4) + HEX.charAt(b & 0xf);
    }
    return out;
  }
  function patchTea(url, enc) {
    if (!url || url.indexOf("ysp-tea=") >= 0) return url;
    var l = url.toLowerCase();
    if (enc === 2) return url + (url.indexOf("?") >= 0 ? "&" : "?") + "ysp-tea=1";
    if (l.indexOf("cdrmld") >= 0 || l.indexOf("cctvwbcd") >= 0)
      return url + (url.indexOf("?") >= 0 ? "&" : "?") + "ysp-tea=2";
    return url;
  }

  var guid = ms.getCache("yspweb_guid");
  if (!guid || String(guid).length !== 20) {
    guid = randStr(8) + "_" + randStr(11);
    ms.setCache("yspweb_guid", guid, 86400000);
  }
  var ticketRandom = randStr(10);
  var liveRandom = randStr(10);
  var rand = randStr(10);

  var authBody =
    "pid=" +
    livepid +
    "&guid=" +
    guid +
    "&appid=" +
    AUTH_APPID +
    "&rand_str=" +
    rand +
    "&signature=" +
    ms.md5(
      "appid=" + AUTH_APPID + "&guid=" + guid + "&pid=" + livepid + "&rand_str=" + rand + AUTH_SALT
    );
  var authJson = ms.post(
    PLAYER + "/v1/player/auth",
    authBody,
    apiHdr(guid, "application/x-www-form-urlencoded;charset=UTF-8", null, null)
  );
  if (!authJson || String(authJson).indexOf('"code":0') < 0) throw new Error("ysp auth fail");
  var authToken = (String(authJson).match(/"token"\s*:\s*"([^"]+)"/) || [])[1] || "";
  var serverSec = parseInt((String(authJson).match(/"ts"\s*:\s*"?(\d+)/) || [])[1] || "0", 10);
  if (!authToken || !serverSec) throw new Error("ysp auth parse");

  var sdkToken = ms.getCache("yspweb_sdk_token");
  if (!sdkToken) {
    var tsMs = serverSec * 1000;
    var rnd = ms.md5("token_rnd;" + APPID + ";v1;" + guid + ";" + tsMs + ";");
    var openUrl =
      "https://h5access.yangshipin.cn/web/open/token?yspappid=" +
      APPID +
      "&guid=" +
      guid +
      "&vappid=" +
      VAPPID +
      "&vsecret=" +
      VSECRET +
      "&raw=1&version=v1&ts=" +
      tsMs +
      "&rnd=" +
      rnd;
    var openJson = ms.get(openUrl, apiHdr(guid, null, null, null));
    sdkToken = (String(openJson || "").match(/"token"\s*:\s*"([^"]+)"/) || [])[1] || "";
    if (!sdkToken) throw new Error("ysp token fail");
    ms.setCache("yspweb_sdk_token", sdkToken, 300000);
  }

  var requestId = "999999" + randStr(10) + String(ms.now_ms());
  var fields = {
    adjust: "1",
    appVer: "V1.0.0",
    app_version: "V1.0.0",
    cKey: cKey(serverSec, guid),
    channel: "ysp_tx",
    cmd: "2",
    cnlid: cnlid,
    defn: defn,
    devid: "devid",
    dtype: "1",
    encryptVer: "8.1",
    guid: guid,
    livepid: livepid,
    otype: "ojson",
    platform: PLATFORM,
    rand_str: liveRandom,
    sphttps: "1",
    stream: "2"
  };
  var body = jsonBody(fields, liveSign(fields));
  var sin = sdkInputMd5(fields);
  var input = sin + "-" + guid + "-1-" + requestId;
  var liveHdr = apiHdr(guid, "application/json;charset=UTF-8", requestId, {
    yspplayertoken: authToken,
    yspticket: ticket(livepid, serverSec, guid, ticketRandom),
    seqid: "1",
    "request-id": requestId,
    yspsdkinput: sin,
    yspsdksign: sdkSign(guid, sdkToken, input) + "-" + input
  });
  var liveJson = ms.post(PLAYER + "/v1/player/get_live_info", body, liveHdr);
  if (!liveJson || String(liveJson).indexOf("playurl") < 0) throw new Error("ysp live miss");
  var u = (String(liveJson).match(/"playurl"\s*:\s*"([^"]+)"/) || [])[1] || "";
  u = u.replace(/\\\//g, "/").replace(/\\u0026/g, "&");
  if (u.indexOf("http") !== 0) throw new Error("ysp playurl empty");
  var ext = (String(liveJson).match(/"extended_param"\s*:\s*"([^"]*)"/) || [])[1] || "";
  ext = ext.replace(/\\\//g, "/").replace(/\\u0026/g, "&");
  if (ext) {
    if (ext.charAt(0) === "&" || ext.charAt(0) === "?") u += ext;
    else u += "&" + ext;
  }
  var enc = parseInt((String(liveJson).match(/"encrypt"\s*:\s*(-?\d+)/) || [])[1] || "-1", 10);
  u = patchTea(u, enc);
  return { url: u, referer: ORIGIN + "/", userAgent: UA };
}
