/**
 * 广电云 web.guangdianyun.tv/tv/?id=&uin= → getPlayAddress。
 * 由 entry.lua gdy_live 翻译。
 */
function main(item) {
  var page = item.pageUrl || ms.getQuery(item.url, "page") || item.url || "";
  var id = (page.match(/[?&]id=(\d+)/) || page.match(/\/tv\/(\d+)/) || [])[1];
  var uin = (page.match(/[?&]uin=(\d+)/) || [])[1];
  if (!id || !uin) throw new Error("gdy need id+uin");

  function uuid4() {
    return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, function (c) {
      var r = (Math.random() * 16) | 0;
      var v = c === "x" ? r : (r & 0x3) | 0x8;
      return v.toString(16);
    });
  }

  var api =
    "https://apiliveroom.dev.guangdianyun.tv/v1/tv/getPlayAddress?id=" + id +
    "&uin=" + uin + "&clientId=" + uuid4();
  var body = ms.get(api, { "Referer": "https://web.guangdianyun.tv/" });
  if (!body) throw new Error("gdy empty");
  var s = String(body).replace(/\\\//g, "/");
  var m =
    s.match(/"hlsUrl"\s*:\s*"(https?:\/\/[^"]+)"/) ||
    s.match(/"(?:url|playUrl|play_url)"\s*:\s*"(https?:\/\/[^"]+)"/);
  if (!m) throw new Error("gdy no url");
  return {
    url: m[1],
    referer: "https://web.guangdianyun.tv/",
    userAgent: "Mozilla/5.0"
  };
}
