/**
 * 虎牙直播：房间号 → FLV/HLS。
 * 频道例：http://宿主/huya_live.js?id=房间号
 */
function main(item) {
  var rid = ms.getQuery(item.url, "id") || item.id || "";
  if (!/^\d+$/.test(rid)) throw new Error("huya bad id");
  var UA = "Mozilla/5.0";
  var resp = ms.get(
    "https://mp.huya.com/cache.php?m=Live&do=profileRoom&roomid=" + rid,
    { "User-Agent": UA, Referer: "https://www.huya.com/" }
  );
  if (!resp || (String(resp).indexOf("multiLine") < 0 && String(resp).indexOf("baseSteamInfoList") < 0)) {
    throw new Error("huya offline id=" + rid);
  }
  resp = String(resp).replace(/\\\//g, "/");

  function fix(u) {
    if (!u) return "";
    u = u.replace(/&amp;/g, "&");
    if (u.indexOf("http") !== 0) return "";
    return u;
  }

  function pickMulti(kind, cdn) {
    var re = new RegExp('"' + kind + '"\\s*:\\s*\\{\\s*"multiLine"\\s*:\\s*(\\[[\\s\\S]*?\\])');
    var bm = resp.match(re);
    if (!bm) return "";
    var block = bm[1];
    var objs = block.match(/\{[^{}]*\}/g) || [];
    var i, o, cm, um;
    for (i = 0; i < objs.length; i++) {
      o = objs[i];
      if (o.indexOf('"cdnType":"' + cdn + '"') < 0 && o.indexOf('"cdnType": "' + cdn + '"') < 0) {
        continue;
      }
      um = o.match(/"url"\s*:\s*"([^"]+)"/);
      if (um) return fix(um[1]);
    }
    return "";
  }

  function pickBase(cdn, kind) {
    var re = new RegExp(
      '"sCdnType"\\s*:\\s*"' +
        cdn +
        '"[\\s\\S]*?"sStreamName"\\s*:\\s*"([^"]+)"[\\s\\S]*?"sFlvUrl"\\s*:\\s*"([^"]+)"[\\s\\S]*?"sFlvUrlSuffix"\\s*:\\s*"([^"]+)"[\\s\\S]*?"sFlvAntiCode"\\s*:\\s*"([^"]+)"[\\s\\S]*?"sHlsUrl"\\s*:\\s*"([^"]+)"[\\s\\S]*?"sHlsUrlSuffix"\\s*:\\s*"([^"]+)"[\\s\\S]*?"sHlsAntiCode"\\s*:\\s*"([^"]+)"'
    );
    var m = resp.match(re);
    if (!m) return "";
    var name = m[1],
      flv = m[2],
      fsuf = m[3],
      fanti = m[4],
      hls = m[5],
      hsuf = m[6],
      hanti = m[7];
    if (kind === "hls") {
      return fix(hls + "/" + name + "." + hsuf + "?" + hanti);
    }
    return fix(flv + "/" + name + "." + fsuf + "?" + fanti);
  }

  var kinds = ["flv", "hls"],
    cdns = ["AL", "TX", "HS"],
    ki,
    ci,
    u;
  for (ki = 0; ki < kinds.length; ki++) {
    for (ci = 0; ci < cdns.length; ci++) {
      u = pickMulti(kinds[ki], cdns[ci]) || pickBase(cdns[ci], kinds[ki]);
      if (u) {
        return { url: u, referer: "https://www.huya.com/", userAgent: UA };
      }
    }
  }
  throw new Error("huya stream empty id=" + rid);
}
