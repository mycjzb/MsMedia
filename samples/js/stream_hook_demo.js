/**
 * stream_call 演示：main 回清单（B）并声明 stream；
 * 内核拉每个分片 URL 时调 stream_call(url, args) 取字节。
 *
 * 频道：…/stream_hook_demo.js?page=https://cdn/index.m3u8
 * 生产（广西等）：在 stream_call 里 ms.get + cipher 解密后 return 字节。
 */
function main(item) {
  var page = item.pageUrl || ms.getQuery(item.url, "page") || "";
  if (!page) throw new Error("need page=");
  var body = ms.get(page, { "User-Agent": "Mozilla/5.0", Referer: page });
  if (!body || String(body).indexOf("#EXTM3U") < 0) throw new Error("not m3u8");
  var origin = (page.match(/^(https?:\/\/[^/?#]+)/) || [])[1] || "";
  var base = (page.match(/^([^?]*)\//) || [])[0] || (origin + "/");
  var lines = String(body).replace(/\r/g, "").split("\n");
  var out = [], i, l;
  for (i = 0; i < lines.length; i++) {
    l = lines[i];
    if (!l || l.charAt(0) === "#") {
      out.push(l);
      continue;
    }
    if (l.indexOf("://") >= 0) out.push(l);
    else if (l.charAt(0) === "/") out.push(origin + l);
    else out.push(base + l);
  }
  return {
    m3u8: out.join("\n"),
    url: page,
    referer: origin + "/",
    stream: true,
    streamArgs: { site: "demo", referer: origin + "/" }
  };
}

/** 内核固定调用：原始分片 URL + main 带回的 args → 返回字节 */
function stream_call(url, args) {
  var hdr = { "User-Agent": "Mozilla/5.0" };
  if (args && args.referer) hdr.Referer = args.referer;
  var raw = ms.get(url, hdr);
  if (!raw) throw new Error("stream_call empty " + url);
  /* 演示透传；广西等在此 cipher("aes-128-ecb","dec",…) 后再 return */
  return raw;
}
