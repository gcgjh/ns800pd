/**
 * @file    web_page.h
 * @brief   管理页的 HTML+CSS+JS 源码（编译进固件的静态常量）
 *
 * 单独拆一个文件，让 web_server.c 专注于 HTTP 逻辑，不被几百行 CSS 淹没。
 * 本头文件只含一个字符串常量，无逻辑，因此重复包含无副作用。
 *
 * 修改本文件须注意 C 字符串转义：所有内容都是 C 字符串字面量，
 * 因此反斜杠要写成 \\，双引号要写成 \"，JS 中的单引号不受影响
 * （外层为双引号）。页面本身不含 printf 占位符，动态内容全部由
 * JS 拉取 /api/status 后填充。
 *
 * 视觉为 Material Design 浅色主题，纯 CSS 实现、零额外素材：
 * 背景 #F6F7F9 / 卡片 #FFFFFF / 主色蓝 #1A73E8 / 强调绿 #16A34A /
 * 次要文字 #5F6368 / 分隔线 #E3E6EA。安卓观感来自蓝色 AppBar、
 * 纯白圆角卡片加柔和投影浮起、输入框聚焦变主色边、Snackbar 式 Toast。
 */
#ifndef __WEB_PAGE_H__
#define __WEB_PAGE_H__

static const char WEB_PAGE_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>NSSinePad C3 控制台</title>\n"
"<style>\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{font-family:Roboto,'Helvetica Neue',Arial,'PingFang SC','Microsoft YaHei',sans-serif;"
"background:#F6F7F9;color:#202124;-webkit-font-smoothing:antialiased;padding-bottom:40px}\n"

/* ── AppBar ── */
".appbar{position:sticky;top:0;z-index:10;height:56px;background:#1A73E8;display:flex;"
"align-items:center;padding:0 16px;color:#fff;box-shadow:0 2px 4px rgba(60,64,67,.3);font-size:20px;font-weight:500}\n"
".appbar .sub{margin-left:12px;font-size:13px;font-weight:400;opacity:.9}\n"

/* ── 卡片（MD3 elevation-1：两层投影叠出浮起感）── */
".wrap{max-width:720px;margin:0 auto;padding:16px}\n"
".card{background:#fff;border-radius:14px;margin-bottom:16px;overflow:hidden;"
"box-shadow:0 1px 2px rgba(60,64,67,.16),0 1px 3px rgba(60,64,67,.10)}\n"
".card h2{font-size:13px;font-weight:500;color:#5F6368;padding:16px 16px 0;letter-spacing:.4px}\n"
".rows{padding:6px 0}\n"
".row{display:flex;align-items:center;min-height:48px;padding:0 16px;border-bottom:1px solid #E3E6EA}\n"
".row:last-child{border-bottom:none}\n"
".row .k{color:#5F6368;font-size:14px;flex:0 0 132px}\n"
".row .v{flex:1;font-size:14px;word-break:break-all;color:#202124}\n"

/* ── 徽章（浅底深字）── */
".badge{display:inline-block;padding:3px 10px;border-radius:12px;font-size:12px;font-weight:500}\n"
".b-on{background:#E6F4EA;color:#137333}\n"
".b-link{background:#FEF7E0;color:#B06000}\n"
".b-off{background:#FCE8E6;color:#C5221F}\n"

/* ── 表单 ── */
".form{padding:16px}\n"
"label{display:block;font-size:12px;color:#5F6368;margin:14px 0 6px}\n"
"label:first-child{margin-top:0}\n"
"input,select{width:100%;background:#F1F3F4;border:1px solid transparent;color:#202124;"
"border-radius:8px;padding:12px 14px;font-size:15px;font-family:inherit;outline:none;"
"transition:border-color .15s,background .15s}\n"
"input:focus{border-color:#1A73E8;background:#fff}\n"

/* ── 分段按钮 ── */
".seg{display:flex;border:1px solid #DADCE0;border-radius:8px;overflow:hidden;background:#fff}\n"
".seg button{flex:1;background:transparent;border:none;color:#5F6368;padding:11px;"
"font-size:14px;font-family:inherit;cursor:pointer;transition:background .15s,color .15s}\n"
".seg button.on{background:#1A73E8;color:#fff}\n"

/* ── 按钮 ── */
".btn{display:block;width:100%;border:none;border-radius:8px;padding:12px;"
"font-size:15px;font-family:inherit;font-weight:500;cursor:pointer;margin-top:16px;"
"transition:box-shadow .15s,opacity .15s}\n"
".btn:active{box-shadow:0 1px 3px rgba(60,64,67,.3)}\n"
".btn:disabled{opacity:.45;cursor:not-allowed}\n"
".b-primary{background:#1A73E8;color:#fff}\n"
".b-warn{background:#D93025;color:#fff}\n"
".b-line{background:#fff;border:1px solid #DADCE0;color:#1A73E8}\n"
".b-up{background:#16A34A;color:#fff}\n"
".hint{font-size:12px;color:#80868B;margin-top:8px;line-height:1.6}\n"

/* ── 文件选择（自绘成按钮，原生 input 藏起来）── */
".filebtn{display:block;width:100%;background:#F1F3F4;border:1px dashed #B9BDC3;"
"border-radius:8px;padding:18px;text-align:center;cursor:pointer;color:#5F6368;font-size:14px;"
"transition:background .15s,border-color .15s}\n"
".filebtn:hover{background:#E8F0FE;border-color:#1A73E8}\n"
".filebtn b{color:#1A73E8;font-weight:500}\n"
"#f-bin{display:none}\n"
".picked{margin-top:10px;font-size:13px;color:#202124;word-break:break-all}\n"

/* ── 进度条 ── */
".pbar{height:4px;background:#E8EAED;border-radius:2px;overflow:hidden;margin-top:14px;display:none}\n"
".pbar>i{display:block;height:100%;width:0;background:#1A73E8;transition:width .3s}\n"

/* ── Toast（MD3 snackbar：浅色界面里用深灰底保证可读）── */
"#toast{position:fixed;left:50%;bottom:24px;transform:translateX(-50%) translateY(140px);"
"background:#303134;color:#F1F3F4;padding:14px 22px;border-radius:8px;font-size:14px;"
"box-shadow:0 3px 8px rgba(60,64,67,.35);transition:transform .25s;z-index:99;max-width:88vw;text-align:center}\n"
"#toast.show{transform:translateX(-50%) translateY(0)}\n"
"#toast.err{background:#B3261E;color:#fff}\n"
"</style>\n"
"</head>\n"
"<body>\n"

"<div class=\"appbar\">NSSinePad C3<span class=\"sub\">ESP32-C3 联网桥控制台</span></div>\n"

"<div class=\"wrap\">\n"

/* 连接状态卡片 */
"<div class=\"card\">\n<h2>连接状态</h2>\n<div class=\"rows\">\n"
"<div class=\"row\"><span class=\"k\">WiFi</span><span class=\"v\" id=\"s-wifi\">—</span></div>\n"
"<div class=\"row\"><span class=\"k\">IP 地址</span><span class=\"v\" id=\"s-ip\">—</span></div>\n"
"<div class=\"row\"><span class=\"k\">信号 RSSI</span><span class=\"v\" id=\"s-rssi\">—</span></div>\n"
"<div class=\"row\"><span class=\"k\">板间链路</span><span class=\"v\" id=\"s-link\">—</span></div>\n"
"<div class=\"row\"><span class=\"k\">网络时间</span><span class=\"v\" id=\"s-time\">—</span></div>\n"
"</div></div>\n"

/* 运行信息卡片 */
"<div class=\"card\">\n<h2>运行信息</h2>\n<div class=\"rows\">\n"
"<div class=\"row\"><span class=\"k\">固件版本</span><span class=\"v\" id=\"s-ver\">—</span></div>\n"
"<div class=\"row\"><span class=\"k\">运行槽</span><span class=\"v\" id=\"s-slot\">—</span></div>\n"
"<div class=\"row\"><span class=\"k\">运行时间</span><span class=\"v\" id=\"s-up\">—</span></div>\n"
"<div class=\"row\"><span class=\"k\">空闲堆</span><span class=\"v\" id=\"s-heap\">—</span></div>\n"
"<div class=\"row\"><span class=\"k\">上报成功/失败</span><span class=\"v\" id=\"s-rep\">—</span></div>\n"
"<div class=\"row\"><span class=\"k\">链路帧/CRC错</span><span class=\"v\" id=\"s-fr\">—</span></div>\n"
"</div></div>\n"

/* WiFi 配置卡片 */
"<div class=\"card\">\n<h2>WiFi 配置</h2>\n<div class=\"form\">\n"
"<label>工作模式</label>\n"
"<div class=\"seg\">\n"
"<button id=\"m-sta\" class=\"on\" onclick=\"setMode(0)\">连接路由器 (STA)</button>\n"
"<button id=\"m-ap\" onclick=\"setMode(1)\">自建热点 (AP)</button>\n"
"</div>\n"
"<label>路由器 SSID（STA 模式）</label>\n"
"<input id=\"f-ssid\" placeholder=\"例如 gpm\" autocomplete=\"off\">\n"
"<label>路由器密码（STA 模式）</label>\n"
"<input id=\"f-pass\" type=\"password\" placeholder=\"留空表示不修改\" autocomplete=\"off\">\n"
"<label>热点名称（AP 模式）</label>\n"
"<input id=\"f-apssid\" placeholder=\"NSSinePad-AP\" autocomplete=\"off\">\n"
"<label>热点密码（AP 模式）</label>\n"
"<input id=\"f-appass\" type=\"password\" placeholder=\"至少 8 位\" autocomplete=\"off\">\n"
"<button class=\"btn b-primary\" id=\"b-save\" onclick=\"saveWifi()\">保存并应用</button>\n"
"<div class=\"hint\">保存后立即重启 WiFi。若 STA 密码填错导致连不上，"
"网页会跟着一起失联 —— 这时在 NS800 的 msh 里打 <code>c3wifirst</code> 可恢复出厂配置。</div>\n"
"</div></div>\n"

/* 固件升级卡片（本地上传） */
"<div class=\"card\">\n<h2>固件升级</h2>\n<div class=\"form\">\n"
"<label>选择固件文件（.bin）</label>\n"
"<div class=\"filebtn\" onclick=\"document.getElementById('f-bin').click()\">\n"
"点击选择固件 &nbsp;<b>c3_bridge.bin</b>\n"
"</div>\n"
"<input type=\"file\" id=\"f-bin\" accept=\".bin,application/octet-stream\" onchange=\"onPick(this)\">\n"
"<div class=\"picked\" id=\"picked\"></div>\n"
"<button class=\"btn b-up\" id=\"b-up\" onclick=\"doUpload()\" disabled>上传并升级</button>\n"
"<div class=\"pbar\" id=\"up-bar\"><i id=\"up-fill\"></i></div>\n"
"<div class=\"hint\">直接选文件上传，<b>不需要先传到任何服务器、也不需要填地址</b>。"
"上传期间千万别断电。完成后设备自动重启并切到新固件。<br>"
"固件用本地 <code>idf.py build</code> 产出的 <code>build/c3_bridge.bin</code>，上限 1.5MB。</div>\n"
"</div></div>\n"

/* 设备卡片 */
"<div class=\"card\">\n<h2>设备</h2>\n<div class=\"form\">\n"
"<button class=\"btn b-warn\" onclick=\"reboot()\">重启设备</button>\n"
"<div class=\"hint\">页面每秒自动刷新一次状态。</div>\n"
"</div></div>\n"

"</div>\n"
"<div id=\"toast\"></div>\n"

/* 脚本 */
"<script>\n"
"function $(i){return document.getElementById(i)}\n"
"function toast(m,err){var t=$('toast');t.textContent=m;t.className=(err?'err ':'')+'show';"
"clearTimeout(t._h);t._h=setTimeout(function(){t.className=''},3000)}\n"
"function esc(s){return String(s==null?'':s).replace(/[&<>\"']/g,function(c){"
"return{'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]})}\n"

/* ── 状态轮询（每 1 秒）── */
"var g_polling=false;\n"
"function poll(){\n"
"  if(g_polling)return;        /* 上一拍还没回来就跳过，别把请求堆成一串 */\n"
"  g_polling=true;\n"
"  fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json()})"
".then(function(d){\n"
"    g_polling=false;\n"
"    var b;\n"
"    if(d.online){b='<span class=\"badge b-on\">'+esc(d.ssid)+'</span>';}\n"
"    else if(d.wifi_state===1){b='<span class=\"badge b-link\">连接中</span>';}\n"
"    else{b='<span class=\"badge b-off\">未联网</span>';}\n"
"    $('s-wifi').innerHTML=b;\n"
"    $('s-ip').textContent=d.ip;\n"
"    $('s-rssi').textContent=(d.rssi<0?d.rssi+' dBm':'—');\n"
"    $('s-link').innerHTML=d.link_alive?'<span class=\"badge b-on\">NS800 在线</span>':"
"'<span class=\"badge b-off\">无应答</span>';\n"
"    $('s-time').textContent=d.time_valid?d.time_str:'未对时';\n"
"    $('s-ver').textContent=d.version;\n"
"    $('s-slot').textContent=d.slot;\n"
"    $('s-up').textContent=d.uptime;\n"
"    $('s-heap').textContent=d.heap;\n"
"    $('s-rep').textContent=d.rep_ok+' / '+d.rep_fail;\n"
"    $('s-fr').textContent=d.frames+' / '+d.badcrc;\n"
/* 写入 flash 的进度以设备 OTA 状态为准 —— 浏览器的上传进度只覆盖
 *   "网络传输"那一段，之后还有几百毫秒的擦写，状态以设备为准更准。 */
"    if(d.ota_state===1){$('up-bar').style.display='block';"
"$('up-fill').style.width=d.ota_pct+'%';$('b-up').disabled=true;"
"$('b-up').textContent='写入 flash '+d.ota_pct+'%';}\n"
"    else if(d.ota_state===2){$('up-bar').style.display='block';"
"$('up-fill').style.width='100%';$('b-up').disabled=false;$('b-up').textContent='上传并升级';"
"toast('固件已写入，重启中');}\n"
"  }).catch(function(){g_polling=false;});\n"
"}\n"

/* ── 模式切换（只切 UI 高亮，不发请求）── */
"var g_mode=0;\n"
"function setMode(m){g_mode=m;$('m-sta').className=m===0?'on':'';$('m-ap').className=m===1?'on':'';}\n"

/* ── 保存 WiFi ── */
"function saveWifi(){\n"
"  var b=$('b-save');b.disabled=true;b.textContent='保存中…';\n"
"  fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({mode:g_mode,ssid:$('f-ssid').value,pass:$('f-pass').value,"
"apssid:$('f-apssid').value,appass:$('f-appass').value})})\n"
"  .then(function(r){return r.json()})\n"
"  .then(function(d){b.disabled=false;b.textContent='保存并应用';"
"if(d.ok){toast('已保存，WiFi 正在重连');$('f-pass').value='';$('f-appass').value='';}"
"else{toast(d.msg||'失败',1);}})\n"
"  .catch(function(){b.disabled=false;b.textContent='保存并应用';toast('请求失败',1)});\n"
"}\n"

/* ── 选文件 ── */
"var g_file=null;\n"
"function onPick(el){g_file=el.files[0]||null;\n"
"  if(!g_file){$('picked').textContent='';$('b-up').disabled=true;return;}\n"
"  $('picked').innerHTML='已选择 <b>'+esc(g_file.name)+'</b> · '"
"+(g_file.size/1048576).toFixed(2)+' MB';\n"
"  $('b-up').disabled=false;\n"
"}\n"

/* ── 上传并升级 ──
 * 必须用 XMLHttpRequest 而非 fetch：只有 XHR 的 upload.onprogress 能给出
 * 真实上传百分比，1MB 固件上传时用户需要看到进度条。 */
"function doUpload(){\n"
"  if(!g_file){toast('请先选择固件文件',1);return}\n"
"  if(g_file.size>1572864){toast('文件超过 1.5MB，装不进 OTA 槽',1);return}\n"
"  if(!confirm('确认上传 '+g_file.name+' 并重启升级？\\n上传期间请勿断电。'))return;\n"
"  var xhr=new XMLHttpRequest();\n"
"  xhr.open('POST','/api/ota/upload',true);\n"
"  xhr.setRequestHeader('Content-Type','application/octet-stream');\n"
"  $('up-bar').style.display='block';$('b-up').disabled=true;\n"
"  xhr.upload.onprogress=function(e){\n"
"    if(e.lengthComputable)$('up-fill').style.width=(e.loaded*100/e.total)+'%';\n"
"  };\n"
"  xhr.onload=function(){\n"
"    var d={};try{d=JSON.parse(xhr.responseText)}catch(err){}\n"
"    if(xhr.status===200&&d.ok){toast(d.msg||'上传完成');}\n"
"    else{$('b-up').disabled=false;toast(d.msg||'上传失败',1);}\n"
"  };\n"
"  xhr.onerror=function(){$('b-up').disabled=false;toast('上传中断',1)};\n"
"  $('b-up').textContent='上传中…';\n"
"  xhr.send(g_file);\n"
"}\n"

/* ── 重启 ── */
"function reboot(){\n"
"  if(!confirm('确定要重启设备吗？'))return;\n"
"  toast('重启中…');\n"
"  fetch('/api/reboot',{method:'POST'}).catch(function(){});\n"
"}\n"

/* ── 启动：拉一次配置填表，然后每秒轮询 ── */
"fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json()}).then(function(d){\n"
"  setMode(d.mode);$('f-ssid').value=d.sta_ssid||'';$('f-apssid').value=d.ap_ssid||'';\n"
"}).catch(function(){});\n"
"poll();setInterval(poll,1000);\n"
"</script>\n"
"</body>\n"
"</html>\n";

#endif /* __WEB_PAGE_H__ */