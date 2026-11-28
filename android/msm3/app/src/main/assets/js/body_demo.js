/**
 * B 形态演示：JS 自己拉 m3u8 正文，return { m3u8 } 交给内核内存起播。
 * 频道：…/body_demo.js?page=https://cdn.example/live/index.m3u8
 * 生产脚本可在拉完后改写相对 TS 为绝对地址再 return。
 */
function main(item) {
  var page = item.pageUrl || ms.getQuery(item.url, "page") || "";
  if (!page) throw new Error("need page=");
  var body = ms.get(page, { "User-Agent": "Mozilla/5.0", Referer: page });
  if (!body || String(body).indexOf("#EXTM3U") < 0) throw new Error("not m3u8");
  var origin = (page.match(/^(https?:\/\/[^/?#]+)/) || [])[1] || "";
  var base = (page.match(/^([^?]*)\//) || [])[0] || (origin + "/");
  var q = (page.match(/\?.+$/) || [])[0] || "";
  var text = String(body).replace(/\r/g, "");
  var lines = text.split("\n");
  var out = [], i, l;
  for (i = 0; i < lines.length; i++) {
    l = lines[i];
    if (!l || l.charAt(0) === "#") {
      out.push(l);
      continue;
    }
    if (l.indexOf("://") >= 0) out.push(l);
    else if (l.indexOf("//") === 0) out.push((page.indexOf("https") === 0 ? "https:" : "http:") + l);
    else if (l.charAt(0) === "/") out.push(origin + l + (l.indexOf("?") >= 0 ? "" : q));
    else out.push(base + l + (l.indexOf("?") >= 0 ? "" : q));
  }
  return {
    m3u8: out.join("\n"),
    url: page,
    referer: origin + "/",
    userAgent: "Mozilla/5.0"
  };
}
