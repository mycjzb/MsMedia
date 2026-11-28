/**
 * YY 直播：官网 / wap / yyloop://房间号 → FLV/HLS。
 * 由 entry.lua yy_try_resolve / yy_sm / yy_wap_hls 翻译。
 */
function main(item) {
  var u = item.url || "";
  var cid = null, sid = null;
  var m = u.match(/^yyloop:\/\/(\d+)(?:\/(\d+))?/i);
  if (m) {
    cid = m[1];
    sid = m[2] || m[1];
  } else {
    m = u.match(/yy\.com\/(?:mobileweb\/)?(\d+)(?:\/(\d+))?/i);
    if (m) {
      cid = m[1];
      sid = m[2] || m[1];
    }
  }
  if (!cid) throw new Error("yy cid missing");

  function ok(x) {
    if (!x) return "";
    x = String(x);
    if (x.indexOf("http") !== 0) return "";
    if (x.indexOf("yy.com") < 0) return "";
    if (x.indexOf("google.com") >= 0) return "";
    return x;
  }

  function sm(fmt) {
    var ver = "5.14.13";
    var seq = String(ms.now_ms());
    var body =
      '{"head":{"seq":' + seq + ',"appidstr":"0","bidstr":"121","cidstr":"' + cid +
      '","sidstr":"' + sid + '","uid64":0,"client_type":108,"client_ver":"' + ver +
      '","stream_sys_ver":1,"app":"yylive_web","playersdk_ver":"' + ver +
      '","thundersdk_ver":"0","streamsdk_ver":"' + ver + '"},' +
      '"client_attribute":{"client":"web","model":"","cpu":"","graphics_card":"","os":"chrome",' +
      '"osversion":"106.0.0.0","vsdk_version":"","app_identify":"","app_version":"","business":"",' +
      '"width":"1536","height":"864","scale":"","client_type":8,"h265":0},' +
      '"avp_parameter":{"version":1,"client_type":8,"service_type":0,"imsi":0,"send_time":' +
      Math.floor(ms.time()) + ',"line_seq":-1,"gear":4,"ssl":1,"stream_format":' + fmt + "}}";
    var api =
      "https://stream-manager.yy.com/v3/channel/streams?uid=0&cid=" + cid +
      "&sid=" + sid + "&appid=0&sequence=" + seq + "&encode=json";
    var r = ms.request(api, "POST", {
      "Content-Type": "text/plain;charset=UTF-8",
      "Referer": "https://www.yy.com/"
    }, body);
    if (!r || !r.body) return "";
    var bodyS = String(r.body);
    if (bodyS.indexOf('"avp_info_res"') < 0) return "";
    var um = bodyS.match(/"cdn_info"\s*:\s*\{[^}]*"url"\s*:\s*"([^"]+)"/);
    return ok(um && um[1] ? um[1].replace(/\\\//g, "/") : "");
  }

  function wap() {
    var api =
      "https://interface.yy.com/hls/new/get/" + cid + "/" + sid +
      "/1200?source=wapyy&callback=";
    var html = ms.get(api, { "Referer": "https://wap.yy.com/mobileweb/" + cid });
    var hm = String(html || "").match(/"hls"\s*:\s*"([^"]+)"/);
    return ok(hm && hm[1] ? hm[1].replace(/\\\//g, "/") : "");
  }

  var play = sm(0) || sm(1) || wap();
  if (!play) throw new Error("yy resolve miss cid=" + cid);
  return { url: play, referer: "https://www.yy.com/", userAgent: "Mozilla/5.0" };
}
