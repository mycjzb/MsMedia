/**
 * 芒果 TV 直播：channel_id → getLivePlayUrlMPP。
 * 频道：asset:///js/mgtv_live.js?id=hndsj
 */
var MGTV_IDS = { hndsj: "484" };

function main(item) {
  var id = ms.getQuery(item.url, "id") || item.id || "";
  if (MGTV_IDS[id]) id = MGTV_IDS[id];
  if (!id) throw new Error("mgtv need id");
  var apis = [
    "http://pwlp.bz.mgtv.com/v1/epg/turnplay/getLivePlayUrlMPP?version=PCweb_1.0&platform=4&buss_id=2000001&channel_id=" +
      id,
    "https://mpplive.api.mgtv.com/v1/epg/turnplay/getLivePlayUrlMPP?version=PCweb_1.0&platform=4&buss_id=2000001&channel_id=" +
      id +
      "&definition=std&_support=10000000"
  ];
  var i, t, body, m, u;
  for (i = 0; i < apis.length; i++) {
    for (t = 0; t < 2; t++) {
      body = ms.get(apis[i], { Referer: "https://www.mgtv.com/live/" });
      if (!body) continue;
      body = String(body).replace(/\\\//g, "/");
      m =
        body.match(/"stream_url"\s*:\s*"(https?:\/\/[^"]+)"/) ||
        body.match(/"url"\s*:\s*"(https?:\/\/[^"]+)"/);
      u = m && m[1] ? m[1] : "";
      if (u.indexOf("http") === 0) {
        return { url: u, referer: "https://www.mgtv.com/live/", userAgent: "Mozilla/5.0" };
      }
    }
  }
  throw new Error("mgtv miss id=" + id);
}
