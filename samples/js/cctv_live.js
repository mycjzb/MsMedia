/**
 * 央视新闻直播：emas HMAC-SHA256 + AES-CBC 解 authUrl。
 * 需要 ms.hmac_sha256 / aes_cbc_decrypt / md5 / b64_decode。
 * 频道例：http://宿主/cctv_live.js?id=直播间号
 */
function main(item) {
  var id = ms.getQuery(item.url, "id") || item.id || "";
  if (!/^\d+$/.test(id)) throw new Error("cctv bad id");
  var t = String(Math.floor(ms.now_ms() / 1000));
  var q = "articleId=" + id + "&scene_type=6";
  var sign = ms.hmac_sha256(
    "emasgatewayh5",
    "&&&20000009&" + ms.md5(q) + "&" + t + "&emas.feed.article.live.detail&1.0.0&&&&&"
  );
  var body = ms.get(
    "https://emas-api.cctvnews.cctv.com/h5/emas.feed.article.live.detail/1.0.0?" + q,
    {
      cookieuid: ms.md5(String(ms.now_ms()) + Math.random()),
      "from-client": "h5",
      Referer: "https://m-live.cctvnews.cctv.com/live/landscape.html?liveRoomNumber=" + id,
      "x-emas-gw-appkey": "20000009",
      "x-emas-gw-pv": "6.1",
      "x-emas-gw-sign": sign,
      "x-emas-gw-t": t,
      "x-req-ts": t + "000"
    }
  );
  if (!body) throw new Error("cctv empty");
  var respB64 = (String(body).match(/"response"\s*:\s*"([^"]+)"/) || [])[1] || "";
  function abStr(x) {
    if (!x) return "";
    if (typeof x === "string") return x;
    var u = x instanceof ArrayBuffer ? new Uint8Array(x) : new Uint8Array(x.buffer || x);
    var s = "",
      i;
    for (i = 0; i < u.length; i++) s += String.fromCharCode(u[i]);
    return s;
  }
  var data = abStr(ms.b64_decode(respB64));
  var dk = (data.match(/"dk"\s*:\s*"([^"]+)"/) || [])[1] || "";
  var enc = (data.match(/"authUrl"\s*:\s*"([^"]+)"/) || [])[1] || "";
  if (dk.length < 8 || !enc) throw new Error("cctv emas miss");
  var plain = abStr(
    ms.aes_cbc_decrypt(
      ms.b64_decode(enc),
      dk.substring(0, 8) + t.substring(t.length - 8),
      dk.substring(dk.length - 8) + t.substring(0, 8)
    )
  );
  var url = plain.replace(/\\\//g, "/");
  var m = url.match(/https?:\/\/[^\s"'<>]+/);
  if (!m) throw new Error("cctv no url");
  return {
    url: m[0],
    referer: "https://m-live.cctvnews.cctv.com/",
    userAgent: "Mozilla/5.0"
  };
}
