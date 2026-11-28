/**
 * 网页抠流：从 page= 或 item.url 拉 HTML，取首个 m3u8/flv。
 * 频道例：http://cdn/web_page.js?page=https://example.com/live
 * 由 entry.lua web_page_stream 翻译，只用 ms.*。
 */
function main(item) {
  var page = item.pageUrl || ms.getQuery(item.url, "page") || "";
  if (!page) throw new Error("missing page");
  var html = ms.get(page, {
    "User-Agent": "Mozilla/5.0",
    "Referer": page
  });
  if (!html) throw new Error("empty body");
  html = String(html).replace(/\\\//g, "/").replace(/&amp;/g, "&");
  var re = /https?:\/\/[^\s"'<>\\()]+/gi;
  var m;
  while ((m = re.exec(html))) {
    var u = m[0];
    var p = u.toLowerCase().split("?")[0];
    if (p.indexOf(".m3u8") >= 0 || p.indexOf(".flv") >= 0) {
      return { url: u, referer: page, userAgent: "Mozilla/5.0" };
    }
  }
  throw new Error("m3u8/flv not found");
}
