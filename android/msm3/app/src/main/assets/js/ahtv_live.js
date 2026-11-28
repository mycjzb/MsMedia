/**
 * 安徽台：频道列表抠 m3u8。
 * 频道例：http://宿主/ahtv_live.js?id=频道数字 id
 */
function main(item) {
  var id = ms.getQuery(item.url, "id") || item.id || "";
  if (!id) throw new Error("ahtv need id");
  var body = ms.get(
    "https://mapi.ahtv.cn/api/open/ahtv/channel.php?appid=m2otdjzyuuu8bcccnq" +
      "&appkey=5eab6b4e1969a8f9aef459699f0d9000&is_audio=0&category_id=1",
    { Referer: "https://www.ahtv.cn/" }
  );
  if (!body) throw new Error("ahtv empty");
  body = String(body).replace(/\\\//g, "/");
  var mark = '"id":' + id + ",";
  var p = body.indexOf(mark);
  if (p < 0) throw new Error("ahtv id not found");
  var slice = body.substring(p, p + 800);
  var m = slice.match(/"m3u8"\s*:\s*"(https?:\/\/[^"]+)"/);
  if (!m) throw new Error("ahtv no m3u8");
  return { url: m[1], referer: "https://www.ahtv.cn/", userAgent: "Mozilla/5.0" };
}
