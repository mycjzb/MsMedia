/**
 * 凤凰秀 / 凤凰：auth-url → data.live_url（FLV）。
 * 对齐 entry.lua fhx / ifeng + apply_play_host_headers（.fengshows. → VLC/3.0.20）。
 * 频道：asset:///js/fhx_live.js?id=fhzw
 */
var FHX_IDS = {
  fhzw: "f7f48462-9b13-485b-8101-7b54716411ec",
  fhhk: "15e02d92-1698-416c-af2f-3e9a872b4d78",
  fhzx: "7c96b084-60e1-40a9-89c5-682b994fb680"
};

/* 取址：json_url 用网页 UA；起播：host 含 .fengshows. 固定 VLC/3.0.20 */
var WEB_UA =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
var PLAY_UA = "VLC/3.0.20";
var SITE = "https://m.fengshows.com";

function main(item) {
  var id = ms.getQuery(item.url, "id") || item.id || "";
  if (FHX_IDS[id]) id = FHX_IDS[id];
  if (!id) throw new Error("fhx need id");
  var api =
    SITE +
    "/api/v3/hub/live/auth-url?stream_type=flv&live_id=" +
    ms.urlencode(id) +
    "&live_qa=hd";
  var body = ms.get(api, {
    Referer: SITE,
    "User-Agent": WEB_UA
  });
  if (!body) throw new Error("fhx empty");
  body = String(body).replace(/\\\//g, "/");
  var m =
    body.match(/"live_url"\s*:\s*"(https?:\/\/[^"]+)"/) ||
    body.match(/"(?:url|playUrl)"\s*:\s*"(https?:\/\/[^"]+)"/);
  if (!m) throw new Error("fhx miss id=" + id);
  /* 只回起播 UA，与 entry.lua apply_play_host_headers 一致（不强制 Referer） */
  return {
    url: m[1],
    userAgent: PLAY_UA
  };
}
