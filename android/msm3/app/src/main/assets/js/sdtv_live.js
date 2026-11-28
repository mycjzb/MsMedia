/**
 * 山东台 sdtv（齐鲁/闪电新闻 API）。
 * 对齐 entry.lua sdtv_live。
 * 频道：asset:///js/sdtv_live.js?id=sdws
 */
var SDTV_IDS = {
  sdws: "1",
  sdws4k: "1k",
  sdxw: "3",
  sdql: "5",
  sdty: "7",
  sdsh: "9",
  sdzy: "11",
  sdwl: "13",
  sdnk: "15",
  sdse: "17"
};

var SDTV_KEY_HEX =
  "6262393735383763666138356563653535343961336432353766373931396633";

function main(item) {
  var id = ms.getQuery(item.url, "id") || item.id || "";
  if (SDTV_IDS[id]) id = SDTV_IDS[id];
  var m = String(id).match(/^(\d+)(k?)$/);
  if (!m) throw new Error("sdtv bad id");
  var cid = m[1];
  var k4 = m[2] === "k";
  var t = String(Math.floor(ms.now_ms() / 1000));
  var path =
    "https://sdxw.iqilu.com/v1/app/play/tv" +
    (cid.length > 2 ? "/region" : "") +
    "/live?e=1";
  var resp = ms.get(path, {
    version: "10.1.1",
    orgid: "21",
    platform: "android",
    imei: "7f918d21082ed7eb",
    timestamp: t,
    noncestr: "huangye",
    sign: ms.md5("huangye" + t + "211f68ea4aeb687a6561707b6e3523c84e"),
    "User-Agent": "chuangqi.o.21.com.iqilu.ksd/10.1.1"
  });
  if (!resp) throw new Error("sdtv empty");
  var key = ms.hex_decode(SDTV_KEY_HEX);
  var iv = "0000000000000000";
  var plainAb = ms.aes_cbc_decrypt(ms.b64_decode(resp) || new ArrayBuffer(0), key, iv);
  var plain = "",
    i,
    pu = plainAb instanceof ArrayBuffer ? new Uint8Array(plainAb) : null;
  if (pu) for (i = 0; i < pu.length; i++) plain += String.fromCharCode(pu[i]);
  plain = plain.replace(/\\\//g, "/");
  var field = k4 ? "s4KStream" : "stream";
  var re = new RegExp(
    '"id"\\s*:\\s*"?' +
      cid +
      '"?[\\s\\S]{0,1200}?"' +
      field +
      '"\\s*:\\s*"(https?:\\/\\/[^"]+)"'
  );
  var hit = plain.match(re);
  if (!hit) {
    re = new RegExp(
      '"' +
        field +
        '"\\s*:\\s*"(https?:\\/\\/[^"]+)"[\\s\\S]{0,400}?"id"\\s*:\\s*"?' +
        cid +
        '"?'
    );
    hit = plain.match(re);
  }
  if (!hit) throw new Error("sdtv miss id=" + id);
  return {
    url: hit[1],
    referer: "https://www.iqilu.com/",
    userAgent: "Mozilla/5.0"
  };
}
