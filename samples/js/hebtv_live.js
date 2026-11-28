/**
 * 河北 TV 拉流：path + md5 鉴权。
 * 频道例：http://宿主/hebtv_live.js?id=频道路径名（如 hbws）
 */
function main(item) {
  var id = ms.getQuery(item.url, "id") || item.id || "";
  if (!id) throw new Error("hebtv need id");
  var path = "/" + id + ".m3u8";
  var t = String(ms.time() + 3600);
  var k = ms.md5(path + "k5m9p2x8r4b3" + t);
  return {
    url: "https://tv.pull.hebtv.com" + path + "?t=" + t + "&k=" + k,
    referer: "https://www.hebtv.com/",
    userAgent: "Mozilla/5.0"
  };
}
