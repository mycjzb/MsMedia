/**
 * 河南台（pubmod.hntv.tv / hndt）getAuth → video_streams。
 * 注意：域名 hntv 实为河南，不是湖南。对齐 entry.lua hntv_live。
 * 频道：asset:///js/hntv_live.js?id=hnws
 */
var HNTV_IDS = {
  hnws: "145", /* 河南卫视 */
  hnds: "141", /* 都市频道 */
  hnms: "146", /* 民生 */
  hnfz: "147", /* 法治 */
  hndsj: "148", /* 电视剧 */
  hnxw: "149", /* 新闻 */
  htgw: "150", /* 欢腾购物 */
  hngg: "151", /* 公共 */
  hnxc: "152", /* 乡村 */
  hnly: "154" /* 梨园 */
};

function main(item) {
  var id = ms.getQuery(item.url, "id") || item.id || "";
  if (HNTV_IDS[id]) id = HNTV_IDS[id];
  if (!/^\d+$/.test(id)) throw new Error("hntv bad id");
  var t = String(Math.floor(ms.now_ms() / 1000));
  var body = ms.get(
    "https://pubmod.hntv.tv/program/getAuth/channel/channelIds/1/" + id,
    {
      timestamp: t,
      sign: ms.sha256("6ca114a836ac7d73" + t),
      Referer: "https://static.hntv.tv/",
      "User-Agent": "Mozilla/5.0"
    }
  );
  if (!body) throw new Error("hntv empty");
  body = String(body).replace(/\\\//g, "/");
  /* 只取本频道 video_streams[0]，勿扫整包未鉴权 streams */
  var vs = (body.match(/"video_streams"\s*:\s*(\[[^\]]*\])/) || [])[1] || "";
  var m =
    vs.match(/"(https?:\/\/[^"]+)"/) ||
    body.match(/"video_streams"\s*:\s*\[[^\]]*?"(https?:\/\/[^"]+)"/);
  var u = m && m[1] ? m[1] : "";
  if (!u) throw new Error("hntv miss id=" + id);
  if (u.indexOf("http:") === 0) u = "https:" + u.substring(5);
  return {
    url: u,
    referer: "https://www.hntv.tv/",
    userAgent: "Mozilla/5.0"
  };
}
