#include "admin_server.h"

#include <cstring>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <nvs.h>

#include "quota_manager.h"
#include "todo_manager.h"
#include "weather_manager.h"
#include "calendar_manager.h"
#include "manager_safety.h"
#include "wifi_manager.h"
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "../custom_lcd_display.h"

namespace {
constexpr const char* TAG = "AdminServer";
constexpr const char* kPartition = "quota_nvs";
constexpr const char* kNamespace = "admin";
constexpr size_t kMaxRequest = 48 * 1024;
constexpr int64_t kSessionTimeoutUs = 30LL * 60 * 1000000;

void ScheduleTodoDisplayRefresh() {
    Application::GetInstance().Schedule([]() {
        auto* display = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay());
        if (display) display->RefreshMemoDisplay();
    });
}

// 后台远程切页：mode 与 MCP 工具 self.disp.switch 保持一致
// （toggle / overview|weather / calendar / forecast / quota）。
// 必须经 Application::Schedule 投递到主任务，HTTP 线程不能直接操作 LVGL。
void ScheduleDisplaySwitch(const std::string& mode) {
    Application::GetInstance().Schedule([mode]() {
        auto* display = static_cast<CustomLcdDisplay*>(Board::GetInstance().GetDisplay());
        if (!display) return;
        if (mode == "toggle") {
            display->CycleDisplayMode();
        } else if (mode == "overview" || mode == "weather") {
            display->SwitchToWeatherPage();
        } else if (mode == "calendar") {
            display->SwitchToCalendarPage();
        } else if (mode == "forecast") {
            display->SwitchToForecastPage();
        } else if (mode == "quota") {
            display->SwitchToQuotaPage();
        }
    });
}

const char kAdminHtml[] = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>RLCD CONTROL</title><style>
:root{--ink:#10110f;--paper:#f1efe6;--signal:#ddff35;--muted:#72756d;--line:#20221d;--bad:#b93127;--good:#4e792c}*{box-sizing:border-box}
body{margin:0;background:#10110f;color:var(--ink);font-family:ui-monospace,SFMono-Regular,Menlo,"PingFang SC",monospace;letter-spacing:.02em}
.shell{max-width:1120px;margin:auto;min-height:100vh;background:var(--paper);border-left:1px solid #45483f;border-right:1px solid #45483f}
header{padding:18px 22px;background:var(--ink);color:var(--paper);display:flex;align-items:center;justify-content:space-between;border-bottom:6px solid var(--signal)}
.brand{font-size:22px;font-weight:900;letter-spacing:.12em}.brand i{font-style:normal;color:var(--signal)}.meta{font-size:12px;color:#aeb2a6;text-align:right}
main{padding:22px}.auth{max-width:430px;margin:11vh auto;border:2px solid var(--line);box-shadow:8px 8px 0 var(--signal);padding:28px;background:#fff}
h1,h2,h3,p{margin-top:0}h2{font-size:15px;letter-spacing:.08em;text-transform:uppercase;border-bottom:2px solid;padding-bottom:8px}
.grid{display:grid;grid-template-columns:300px 1fr;gap:18px}.panel{border:2px solid var(--line);background:#fff;padding:16px}.toolbar{display:flex;gap:8px;flex-wrap:wrap;justify-content:space-between;align-items:center;margin-bottom:14px}
button{font:inherit;font-weight:800;border:2px solid var(--line);background:var(--paper);padding:9px 13px;cursor:pointer;box-shadow:3px 3px 0 var(--line)}button:hover{background:var(--signal)}button.primary{background:var(--signal)}button.danger{color:var(--bad)}button:active{transform:translate(2px,2px);box-shadow:1px 1px 0}
input,select{width:100%;font:inherit;border:1px solid var(--line);border-radius:0;background:#faf9f3;padding:8px;margin-top:5px}label{font-size:11px;font-weight:800;color:#484b44;text-transform:uppercase}.field{margin-bottom:10px}
.page-row{display:grid;grid-template-columns:24px 1fr auto;align-items:center;gap:8px;border-bottom:1px dashed #aaa;padding:10px 2px}.page-row input{margin:0;width:18px;height:18px}.arrows button{padding:3px 7px;box-shadow:none}
.quota{border:1px solid var(--line);margin-bottom:10px;transition:opacity .15s}.quota.disabled{opacity:.58}.quota-head{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:8px;padding:11px;background:#deddd5;align-items:center}.quota-body{padding:12px;border-top:1px solid;display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px 12px}.quota-body.hidden{display:none!important}
.identity{min-width:0;display:flex;align-items:center;gap:7px;flex-wrap:wrap}.identity b{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:230px}.account-logo{width:24px;height:24px;background:#fff;border:1px solid var(--line);display:inline-grid;place-items:center;flex:none}.account-logo img{width:100%;height:100%;display:block}.account-logo b{font-size:9px;max-width:none}.actions{display:flex;gap:5px;flex-wrap:wrap;justify-content:flex-end}.actions button{padding:5px 8px;box-shadow:none}.wide{grid-column:1/-1}
.badge{font-size:10px;background:var(--ink);color:var(--signal);padding:3px 6px}.proxy-box{grid-column:1/-1;border:1px dashed var(--line);padding:10px;background:#f1f0e9}.proxy-toggle{display:flex;align-items:center;gap:8px;text-transform:none}.proxy-toggle input{width:18px;height:18px;margin:0}.proxy-note{margin:7px 0 0}.state{font-size:10px;font-weight:900;padding:3px 6px;border:1px solid}.state.ok{background:var(--signal)}.state.error{color:var(--bad);border-color:var(--bad)}.state.stale{border-style:dashed}.secret-note{font-size:10px;color:var(--muted)}
.status{padding:8px 10px;background:#dad8ce;font-size:12px;border-left:5px solid var(--muted)}.status.ok{border-color:var(--good)}.status.bad{border-color:var(--bad)}.hidden{display:none!important}.hint{color:var(--muted);font-size:12px;line-height:1.6}.empty{padding:30px;text-align:center;border:1px dashed var(--muted)}
.toast{position:fixed;right:18px;bottom:18px;background:var(--ink);color:#fff;border-left:7px solid var(--signal);padding:12px 16px;max-width:380px;z-index:9}
.manage-section{margin-top:20px;border-top:3px solid var(--line);padding-top:16px}.manage-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}.todo-row{display:grid;grid-template-columns:24px 82px 1fr auto;gap:7px;align-items:center;padding:8px 0;border-bottom:1px dashed #999}.todo-row input{margin:0}.api-token{word-break:break-all;background:#111;color:var(--signal);padding:8px;font-size:11px}
.device-control{margin-bottom:20px;border:2px solid var(--line);background:#f1f0e9;padding:14px}.device-stats{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:1px;background:var(--line);border:1px solid var(--line);margin-bottom:14px}.device-stat{background:#fff;padding:9px}.device-stat small{display:block;color:var(--muted);font-size:9px}.device-stat b{font-size:13px}.volume-row{display:grid;grid-template-columns:1fr 54px auto auto;gap:8px;align-items:center}.volume-row input{margin:0}.volume-row button{padding:6px 9px;box-shadow:none}
@media(max-width:760px){.grid{grid-template-columns:1fr}.quota-head{grid-template-columns:1fr}.actions{justify-content:flex-start}.quota-body{grid-template-columns:1fr}.wide{grid-column:auto}.brand{font-size:17px}main{padding:12px}.device-stats{grid-template-columns:1fr 1fr}.volume-row{grid-template-columns:1fr 46px}.volume-row button{width:100%}}
</style></head><body><div class="shell"><header><div class="brand">RLCD <i>/</i> CONTROL</div><div class="meta"><span id="ip">LOCAL DEVICE</span><br><span id="clock">OFFLINE</span></div></header>
<main><section id="auth" class="auth"><h1 id="authTitle">管理员登录</h1><p class="hint" id="authHint">局域网设备管理。会话闲置 30 分钟后失效。</p><div class="field"><label>用户名<input id="user" value="admin" disabled></label></div><div class="field"><label>密码<input id="password" type="password" autocomplete="current-password"></label></div><button class="primary" id="authBtn">登录</button></section>
<section id="dashboard" class="hidden"><div class="toolbar"><div><b>设备页面编排</b><div class="hint">额度每屏固定 4 项，停留时每 10 秒翻页</div></div><div><button id="refreshBtn">立即刷新</button> <button id="logoutBtn">退出</button></div></div>
<div class="grid"><aside class="panel"><h2>PAGE ROUTING</h2><div id="pages"></div><button class="primary" id="savePages">保存页面顺序</button><hr><div id="runStatus" class="status">读取状态…</div><div class="field"><label>AI 自动刷新（分钟）<input id="quotaRefreshMinutes" type="number" min="1" max="60" step="1"></label></div><button onclick="saveQuotaRefreshInterval()">保存刷新间隔</button><p class="hint">默认 5 分钟，可设置 1–60 分钟<br>后台端口：8080<br>凭据：只写，不会回显</p></aside>
<article class="panel"><section class="device-control"><h2>设备控制</h2><div class="device-stats"><div class="device-stat"><small>运行时间</small><b id="deviceUptime">--</b></div><div class="device-stat"><small>可用内存</small><b id="deviceHeap">--</b></div><div class="device-stat"><small>最低内存</small><b id="deviceMinHeap">--</b></div><div class="device-stat"><small>网络地址</small><b id="deviceIp">--</b></div></div><label>扬声器音量</label><div class="volume-row"><input id="volumeSlider" type="range" min="0" max="100" step="1" oninput="showVolume(this.value)"><b id="volumeValue">--</b><button onclick="saveVolume()">应用</button><button id="muteBtn" onclick="toggleMute()">静音</button></div><hr><label>显示页面</label><div class="volume-row" style="grid-template-columns:none;gap:6px;flex-wrap:wrap;display:flex"><button onclick="switchPage('overview')">综合</button><button onclick="switchPage('calendar')">日历</button><button onclick="switchPage('forecast')">天气</button><button onclick="switchPage('quota')">AI</button><button onclick="switchPage('toggle')">下一页</button></div><p class="hint">立即切换设备屏幕到对应页面，与 USER 按钮单击等效</p></section>
<div class="toolbar"><h2 style="border:0;margin:0">ACCOUNT MANAGEMENT <span id="count" class="badge">0 / 32</span></h2><div><button id="reloadBtn">放弃修改</button> <button id="addBtn">＋ 添加账号</button></div></div><div id="quotas"></div><button class="primary" id="saveQuotas">保存全部更改</button>
<section class="manage-section"><h2>城市天气</h2><p class="hint">高德提供国内实时天气，Open-Meteo 按所选城市坐标提供七日预报。</p><div class="manage-grid"><div class="field"><label>省份<select id="weatherProvince" onchange="renderWeatherCities(this.value)"></select></label></div><div class="field"><label>城市<select id="weatherCity"></select></label></div></div><div class="field"><label>高德城市 adcode（必填）<input id="amap_adcode" autocomplete="off" placeholder="例如 320500"></label></div><div class="field"><label>高德 Web 服务 Key<input id="amapWebKey" type="password" autocomplete="new-password" placeholder="留空保留已保存 Key"></label></div><div class="field"><label>天气自动刷新（分钟）<input id="weatherRefreshMinutes" type="number" min="5" max="120" step="1"></label></div><label class="hint"><input id="weatherClearAmapKey" type="checkbox" style="width:auto;margin-right:8px">清除已保存高德 Key</label><p id="weatherKeyState" class="hint">高德 Key 不会回显</p><button onclick="saveWeather()" class="primary">保存并安排立即检测</button><button onclick="weatherDiagnostic()">查看天气诊断</button><pre id="weatherDiagnostic" class="status">尚未检查</pre></section>
<section class="manage-section"><h2>代理连通性诊断</h2><p class="hint">使用已保存的第一项启用代理，按真实额度查询路径检查 TCP、HTTP CONNECT 和 TLS；诊断不会回显代理凭据。</p><button onclick="proxyDiagnostic()">检查代理连通性</button><pre id="proxyDiagnostic" class="status">尚未检查</pre></section>
<section class="manage-section"><h2>日历与节假日</h2><div class="field"><label>年度 JSON 数据源<input id="holidaySource"></label></div><div id="holidayStatus" class="hint"></div><button onclick="saveCalendar()">保存数据源</button> <button onclick="syncCalendar()" class="primary">立即同步</button></section>
<section class="manage-section"><div class="toolbar"><h2 style="border:0;margin:0">待办事项</h2><button onclick="addTodo()">＋ 新增待办</button></div><div id="todos"></div></section>
<section class="manage-section"><h2>待办 API</h2><p class="hint">局域网客户端使用 Authorization: Bearer &lt;token&gt;，支持 GET/POST /api/todos 与 GET/PUT/DELETE /api/todos/{id}。</p><div id="apiToken" class="api-token">读取中…</div><button onclick="regenToken()">重新生成 Token</button></section>
</article></div></section></main></div><div id="toast" class="toast hidden"></div>
<script>
var csrf="",items=[],pages=[],todos=[],lastSuccess=0,setupMode=false,editingKey="",uiSeq=0,dirty=false,lastAudibleVolume=50;
var names={overview:"综合",calendar:"日历",forecast:"天气",quota:"AI"};
var providerNames={codex:"Codex",kimi:"Kimi","glm-cn":"GLM 国内","glm-global":"GLM 国际",deepseek:"DeepSeek","generic-json":"通用 JSON",manual:"手动额度"};
var stateNames={ok:"正常",error:"失败",stale:"旧数据",pending:"等待刷新",disabled:"已停用"};
var cityCatalog={
"北京市":[["北京市",39.9042,116.4074]],"天津市":[["天津市",39.0842,117.2009]],"上海市":[["上海市",31.2304,121.4737]],"重庆市":[["重庆市",29.5630,106.5516]],
"河北省":[["石家庄市",38.0428,114.5149],["唐山市",39.6305,118.1802],["秦皇岛市",39.9354,119.5996],["保定市",38.8739,115.4646],["张家口市",40.7675,114.8863]],
"山西省":[["太原市",37.8706,112.5489],["大同市",40.0768,113.3001],["运城市",35.0263,111.0075]],
"内蒙古自治区":[["呼和浩特市",40.8426,111.7492],["包头市",40.6574,109.8403],["鄂尔多斯市",39.6083,109.7813]],
"辽宁省":[["沈阳市",41.8057,123.4315],["大连市",38.9140,121.6147],["鞍山市",41.1086,122.9956]],
"吉林省":[["长春市",43.8171,125.3235],["吉林市",43.8379,126.5496],["延吉市",42.8913,129.5091]],
"黑龙江省":[["哈尔滨市",45.8038,126.5340],["齐齐哈尔市",47.3543,123.9182],["牡丹江市",44.5517,129.6332]],
"江苏省":[["南京市",32.0603,118.7969],["无锡市",31.4912,120.3119],["徐州市",34.2044,117.2858],["常州市",31.8107,119.9741],["苏州市",31.2989,120.5853],["南通市",31.9802,120.8943],["连云港市",34.5967,119.2216],["淮安市",33.6104,119.0153],["盐城市",33.3474,120.1636],["扬州市",32.3942,119.4129],["镇江市",32.1885,119.4250],["泰州市",32.4558,119.9231],["宿迁市",33.9630,118.2752]],
"浙江省":[["杭州市",30.2741,120.1551],["宁波市",29.8683,121.5440],["温州市",27.9949,120.6994],["嘉兴市",30.7461,120.7555],["绍兴市",30.0303,120.5802],["金华市",29.0791,119.6474]],
"安徽省":[["合肥市",31.8206,117.2272],["芜湖市",31.3525,118.4331],["黄山市",29.7147,118.3376]],
"福建省":[["福州市",26.0745,119.2965],["厦门市",24.4798,118.0894],["泉州市",24.8741,118.6757]],
"江西省":[["南昌市",28.6829,115.8582],["九江市",29.7051,116.0019],["赣州市",25.8311,114.9350]],
"山东省":[["济南市",36.6512,117.1201],["青岛市",36.0671,120.3826],["烟台市",37.4638,121.4479],["潍坊市",36.7069,119.1618],["威海市",37.5131,122.1204],["临沂市",35.1047,118.3564]],
"河南省":[["郑州市",34.7466,113.6254],["洛阳市",34.6197,112.4540],["开封市",34.7973,114.3073],["南阳市",32.9907,112.5283]],
"湖北省":[["武汉市",30.5928,114.3055],["宜昌市",30.6919,111.2865],["襄阳市",32.0089,112.1224]],
"湖南省":[["长沙市",28.2282,112.9388],["株洲市",27.8274,113.1340],["衡阳市",26.8932,112.5719],["张家界市",29.1171,110.4792]],
"广东省":[["广州市",23.1291,113.2644],["深圳市",22.5431,114.0579],["珠海市",22.2707,113.5767],["佛山市",23.0215,113.1214],["东莞市",23.0207,113.7518],["惠州市",23.1115,114.4168],["汕头市",23.3541,116.6819]],
"广西壮族自治区":[["南宁市",22.8170,108.3665],["桂林市",25.2736,110.2900],["柳州市",24.3264,109.4281],["北海市",21.4811,109.1202]],
"海南省":[["海口市",20.0440,110.1983],["三亚市",18.2528,109.5120]],
"四川省":[["成都市",30.5728,104.0668],["绵阳市",31.4675,104.6796],["乐山市",29.5521,103.7656],["宜宾市",28.7518,104.6432]],
"贵州省":[["贵阳市",26.6470,106.6302],["遵义市",27.7257,106.9272],["安顺市",26.2531,105.9476]],
"云南省":[["昆明市",25.0389,102.7183],["大理市",25.6065,100.2676],["丽江市",26.8721,100.2299],["西双版纳州",22.0075,100.7979]],
"西藏自治区":[["拉萨市",29.6520,91.1721],["日喀则市",29.2675,88.8811]],
"陕西省":[["西安市",34.3416,108.9398],["宝鸡市",34.3619,107.2373],["延安市",36.5853,109.4898]],
"甘肃省":[["兰州市",36.0611,103.8343],["天水市",34.5809,105.7249],["敦煌市",40.1421,94.6618]],
"青海省":[["西宁市",36.6171,101.7782],["格尔木市",36.4064,94.9285]],
"宁夏回族自治区":[["银川市",38.4872,106.2309],["中卫市",37.5003,105.1968]],
"新疆维吾尔自治区":[["乌鲁木齐市",43.8256,87.6168],["喀什市",39.4704,75.9898],["伊宁市",43.9095,81.2770],["克拉玛依市",45.5799,84.8892]],
"香港特别行政区":[["香港",22.3193,114.1694]],"澳门特别行政区":[["澳门",22.1987,113.5439]],"台湾省":[["台北市",25.0330,121.5654],["高雄市",22.6273,120.3014]]};
var providerMarks={codex:"C",kimi:"K","glm-cn":"G","glm-global":"G",deepseek:"D","generic-json":"JSON",manual:"M"};
var providerLogos={
codex:"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAIAAADYYG7QAAAA+UlEQVR42u2ZyQ7DMAhEQ9T//2V6sBShUuh4UeSh8SEHbzLPY4yJqOqxUzmPzcqzoF/l1TtARJLWeUVuR0hwmzybNjZn1suPk5BlEPVfxYn5lHnLIlXlpNs36slDKNIEQiXSHHIqGQiNnReEqO/v6e5NyFuD0MpVZVuR2fhve/wkNk6IzuoSyu81r0hKP3TTgkTkAqOqF1pbz6khu+u4B89nIIuHzg9rvu6urV9VojnZNJT73DFV5V6b01PjPrf3pcZPyFscWT/2AmaLGCMqUUyDvNR6IwVmPzT/fq2Y/ZjXBJI5qZUfWht918ox3pOXrUjo+bXw5wt6AwzBsWurQCz9AAAAAElFTkSuQmCC",
kimi:"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAIAAADYYG7QAAAAoUlEQVR42u2YwQ6AMAhDh/H/fxkPu5gsVZyigOXgYR7sXmiZE1VtkWppwYqCzmpFL0TE9cOod8MRklGpN5tjTrEJvckGcaLtKYiCOMtsaYES636qZSbkzaYKIT82FQl5nBTyE/L+j6s7OlT1EXhVXDZ6quPpz38nNdp9X99zmsunirMM8ZszXWZCV7uhyImRgiiIN2gfEOItrI0QXUZBxtoAjhZIYMFVQmUAAAAASUVORK5CYII=",
"glm-cn":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAIAAADYYG7QAAAAp0lEQVR42u2YMQ7AIAhFpen9r0yHLiZGCsUYhM/oUnx5CIWYuUWKqwULJPQV93hERNs+PxocjhD1Oe5kM+MEqTNUmccAv4VZCM06oJ/WyYSsc8E/WmcSktn0t5fd0nDCw1jrpR6deE88c/r5hDSVoqk7SF3XIU2Hql1lcofybwogdU6HZEvkWcc6WefqZT2tVX9n2echlD0SQkLyjhFbWB0hOISETPEAWaBLZlJf4mgAAAAASUVORK5CYII=",
"glm-global":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAIAAADYYG7QAAAAp0lEQVR42u2YMQ7AIAhFpen9r0yHLiZGCsUYhM/oUnx5CIWYuUWKqwULJPQV93hERNs+PxocjhD1Oe5kM+MEqTNUmccAv4VZCM06oJ/WyYSsc8E/WmcSktn0t5fd0nDCw1jrpR6deE88c/r5hDSVoqk7SF3XIU2Hql1lcofybwogdU6HZEvkWcc6WefqZT2tVX9n2echlD0SQkLyjhFbWB0hOISETPEAWaBLZlJf4mgAAAAASUVORK5CYII=",
deepseek:"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAIAAADYYG7QAAAAwklEQVR42u2YwQ6AMAhDqfH/fxkPXpYs20qm0RI4qiHdG1Ai3N3+FIf9LEpQCSpBAoIAAOCfixE6l6ff52dmIz/o3+oQirJhSEjWEPoz7ddNm5PJplND+2yYehqxUeuyW/sOs5ZTm2dOToFQe4JoD/Y8oowV3H7u0u6+bB/mmyxuH+3B+QRisGUhxHBKUkMLt2duPernGfeh0X7D7D3RgaRAKMqJ2Q5y1dAbO6TwpAZ/u8/Om4yE6v9QCSpBJagEfRIX3th1XkgYHkYAAAAASUVORK5CYII="};
function providerLogo(provider,klass){var src=providerLogos[provider];return '<span class="'+klass+'">'+(src?'<img alt="" src="'+src+'">':'<b>'+esc(providerMarks[provider]||"AI")+'</b>')+'</span>'}
function el(id){return document.getElementById(id)}
function esc(s){return String(s||"").replace(/[&<>\"]/g,function(c){return({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;"})[c]})}
function toast(s,bad){var t=el("toast");t.textContent=s;t.style.borderColor=bad?"var(--bad)":"var(--signal)";t.classList.remove("hidden");setTimeout(function(){t.classList.add("hidden")},2800)}
function markDirty(){dirty=true;el("saveQuotas").textContent="保存全部更改 *"}
function keyFor(x){if(!x._ui)x._ui="q"+(++uiSeq)+"-"+(x.id||0);return x._ui}
async function api(path,opt){opt=opt||{};opt.credentials="same-origin";opt.headers=opt.headers||{};if(opt.body)opt.headers["Content-Type"]="application/json";if(csrf&&opt.method&&opt.method!=="GET")opt.headers["X-CSRF-Token"]=csrf;var r=await fetch(path,opt),data={};try{data=await r.json()}catch(e){}if(r.status===401&&path!=="/api/login"){showAuth(false);throw Error("登录已失效")}if(!r.ok)throw Error(data.error||("HTTP "+r.status));return data}
function showAuth(setup){setupMode=setup;el("dashboard").classList.add("hidden");el("auth").classList.remove("hidden");el("authTitle").textContent=setup?"首次设置":"管理员登录";el("authHint").textContent=setup?"创建至少 8 位的管理员密码。设备不会保存明文。":"局域网设备管理。会话闲置 30 分钟后失效。";el("authBtn").textContent=setup?"创建并进入":"登录"}
function showDash(){el("auth").classList.add("hidden");el("dashboard").classList.remove("hidden")}
async function boot(){try{var s=await api("/api/status");if(s.setup_required){showAuth(true);return}if(!s.authenticated){showAuth(false);return}csrf=s.csrf||"";showDash();await loadAll()}catch(e){showAuth(false)}}
async function login(){var p=el("password").value;if(p.length<8){toast("密码至少 8 位",true);return}try{var d=await api(setupMode?"/api/setup":"/api/login",{method:"POST",body:JSON.stringify({password:p})});csrf=d.csrf||"";el("password").value="";showDash();await loadAll()}catch(e){toast(e.message,true)}}
async function loadAll(){var q=await api("/api/quotas"),p=await api("/api/pages");items=q.items||[];items.forEach(keyFor);pages=p.pages||[];editingKey="";dirty=false;el("saveQuotas").textContent="保存全部更改";renderPages();renderQuotas();await loadExtras();await status()}
async function loadExtras(){var t=await api("/api/todos"),w=await api("/api/weather"),c=await api("/api/calendar"),k=await api("/api/api-token"),r=await api("/api/refresh-interval");todos=t.items||[];renderTodos();renderWeatherProvinces(w.province||"江苏省",w.city||"苏州市");el("amap_adcode").value=w.amap_adcode||"";el("weatherRefreshMinutes").value=w.refresh_interval_minutes||15;el("weatherClearAmapKey").checked=false;el("weatherKeyState").textContent=w.has_amap_key?"高德 Web 服务 Key 已保存（不会回显）":"请配置高德 Web 服务 Key";el("holidaySource").value=c.source||"";el("holidayStatus").textContent=c.cached_year?("已缓存 "+c.cached_year+" 年数据") : "尚未同步";el("apiToken").textContent=k.token||"生成失败";el("quotaRefreshMinutes").value=r.minutes||5;await loadDevice()}
function heapText(bytes){return (Number(bytes||0)/1024).toFixed(1)+" KiB"}
function uptimeText(seconds){seconds=Number(seconds||0);var d=Math.floor(seconds/86400),h=Math.floor(seconds%86400/3600),m=Math.floor(seconds%3600/60);return (d?d+"天 ":"")+h+"小时 "+m+"分"}
function showVolume(value){value=Number(value);el("volumeValue").textContent=value+"%";el("muteBtn").textContent=value===0?"恢复":"静音"}
async function loadDevice(){var d=await api("/api/device");el("deviceUptime").textContent=uptimeText(d.uptime_seconds);el("deviceHeap").textContent=heapText(d.free_heap);el("deviceMinHeap").textContent=heapText(d.minimum_free_heap);el("deviceIp").textContent=d.ip||"未联网";var volume=Math.max(0,Math.min(100,Number(d.volume)||0));el("volumeSlider").value=volume;if(volume>0)lastAudibleVolume=volume;showVolume(volume)}
async function saveVolume(){var volume=Number(el("volumeSlider").value);await api("/api/device",{method:"PUT",body:JSON.stringify({volume:volume})});if(volume>0)lastAudibleVolume=volume;showVolume(volume);toast("音量已设置为 "+volume+"%")}
async function toggleMute(){var current=Number(el("volumeSlider").value);if(current>0)lastAudibleVolume=current;el("volumeSlider").value=current>0?0:Math.max(1,lastAudibleVolume);await saveVolume()}
async function switchPage(mode){try{await api("/api/display/switch",{method:"POST",body:JSON.stringify({mode:mode})});toast("已切换："+(names[mode]||(mode==="toggle"?"下一页":mode)))}catch(e){toast(e.message,true)}}
async function saveQuotaRefreshInterval(){var minutes=Number(el("quotaRefreshMinutes").value);if(!Number.isInteger(minutes)||minutes<1||minutes>60){toast("请输入 1–60 分钟",true);return}await api("/api/refresh-interval",{method:"PUT",body:JSON.stringify({minutes:minutes})});toast("AI 刷新间隔已设置为 "+minutes+" 分钟")}
function renderTodos(){if(!todos.length){el("todos").innerHTML='<div class="empty">暂无待办</div>';return}el("todos").innerHTML=todos.map(function(t){return '<div class="todo-row"><input type="checkbox" '+(t.completed?'checked':'')+' onchange="toggleTodo(\''+t.id+'\',this.checked)"><span>'+esc((t.due_date||'').slice(5)+(t.due_time?' '+t.due_time:''))+'</span><b>'+esc(t.content)+'</b><span><button onclick="editTodo(\''+t.id+'\')">编辑</button> <button class="danger" onclick="deleteTodo(\''+t.id+'\')">删除</button></span></div>'}).join('')}
async function addTodo(){var content=prompt("待办内容");if(!content)return;var due_date=prompt("日期 YYYY-MM-DD（可留空）","")||"",due_time=prompt("时间 HH:MM（可留空）","")||"";await api("/api/todos",{method:"POST",body:JSON.stringify({content:content,due_date:due_date,due_time:due_time})});toast("待办已添加");loadExtras()}
async function editTodo(id){var item=todos.find(function(t){return t.id===id});if(!item)return;var content=prompt("待办内容",item.content);if(content===null||!content)return;var due_date=prompt("日期 YYYY-MM-DD（可留空）",item.due_date||"");if(due_date===null)return;var due_time=prompt("时间 HH:MM（可留空）",item.due_time||"");if(due_time===null)return;await api("/api/todos/"+id,{method:"PUT",body:JSON.stringify({content:content,due_date:due_date,due_time:due_time})});toast("待办已更新");loadExtras()}
async function toggleTodo(id,done){await api("/api/todos/"+id,{method:"PUT",body:JSON.stringify({completed:done})});loadExtras()}
async function deleteTodo(id){if(!confirm("删除这条待办？"))return;await api("/api/todos/"+id,{method:"DELETE"});loadExtras()}
function renderWeatherProvinces(selectedProvince,selectedCity){var provinces=Object.keys(cityCatalog);el("weatherProvince").innerHTML=provinces.map(function(p){return '<option value="'+esc(p)+'" '+(p===selectedProvince?'selected':'')+'>'+esc(p)+'</option>'}).join("");if(!cityCatalog[selectedProvince])selectedProvince=provinces[0];el("weatherProvince").value=selectedProvince;renderWeatherCities(selectedProvince,selectedCity)}
function renderWeatherCities(province,selectedCity){var cities=cityCatalog[province]||[];el("weatherCity").innerHTML=cities.map(function(c,i){return '<option value="'+i+'" '+(c[0]===selectedCity?'selected':'')+'>'+esc(c[0])+'</option>'}).join("")}
async function saveWeather(){var province=el("weatherProvince").value,cities=cityCatalog[province]||[],city=cities[Number(el("weatherCity").value)],adcode=el("amap_adcode").value.trim(),minutes=Number(el("weatherRefreshMinutes").value);if(!city){toast("请选择城市",true);return}if(!/^\d{6}$/.test(adcode)){toast("请填写 6 位高德城市 adcode",true);return}if(!Number.isInteger(minutes)||minutes<5||minutes>120){toast("天气刷新间隔必须为 5–120 分钟",true);return}var key=el("amapWebKey").value;await api("/api/weather",{method:"PUT",body:JSON.stringify({province:province,city:city[0],latitude:city[1],longitude:city[2],amap_adcode:adcode,amap_key:key,clear_amap_key:el("weatherClearAmapKey").checked,refresh_interval_minutes:minutes})});el("amapWebKey").value="";el("weatherClearAmapKey").checked=false;el("weatherKeyState").textContent="高德配置已保存（Key 不会回显）";toast("配置已保存，设备空闲后立即检测")}
async function weatherDiagnostic(){var box=el("weatherDiagnostic");box.textContent="正在读取最近一次天气请求…";try{var d=await api("/api/weather-diagnostic");box.textContent="接口："+(d.endpoint||"未请求")+"\nHTTP："+(d.http_status||"未请求")+"\n结果："+(d.result||"无返回");box.className="status "+(d.ok?"ok":"bad")}catch(e){box.textContent="诊断失败："+e.message;box.className="status bad"}}
async function proxyDiagnostic(){var box=el("proxyDiagnostic");box.textContent="正在检查 TCP、CONNECT、TLS…";try{var d=await api("/api/proxy-diagnostic",{method:"POST"});box.textContent="端点："+(d.endpoint||"未配置")+"\n阶段："+(d.stage||"未知")+"\n结果："+(d.message||"无返回");box.className="status "+(d.tcp_connected?"ok":"bad")}catch(e){box.textContent="诊断失败："+e.message;box.className="status bad"}}
async function saveCalendar(){await api("/api/calendar",{method:"PUT",body:JSON.stringify({source:el("holidaySource").value})});toast("节假日数据源已保存")}
async function syncCalendar(){try{await api("/api/calendar/sync",{method:"POST"});toast("节假日同步完成");loadExtras()}catch(e){toast(e.message,true)}}
async function regenToken(){if(!confirm("旧 Token 将立即失效，继续？"))return;var k=await api("/api/api-token",{method:"POST"});el("apiToken").textContent=k.token;toast("Token 已重新生成")}
function renderPages(){var h="";pages.forEach(function(p,i){h+='<div class="page-row"><input type="checkbox" '+(p.enabled?"checked":"")+' onchange="pages['+i+'].enabled=this.checked"><b>'+(names[p.id]||p.id)+'</b><span class="arrows"><button onclick="movePage('+i+',-1)">↑</button> <button onclick="movePage('+i+',1)">↓</button></span></div>'});el("pages").innerHTML=h}
function movePage(i,d){var n=i+d;if(n<0||n>=pages.length)return;var x=pages[i];pages[i]=pages[n];pages[n]=x;renderPages()}
function field(i,key,label,type,wide){var v=items[i][key]===undefined?"":items[i][key];return '<div class="field '+(wide?"wide":"")+'"><label>'+label+'<input '+(type?'type="'+type+'" ':"")+'value="'+esc(v)+'" oninput="items['+i+'].'+key+'=this.value;markDirty()"></label></div>'}
function renderQuotas(){el("count").textContent=items.length+" / 32";if(!items.length){el("quotas").innerHTML='<div class="empty">尚未配置额度来源<br><span class="hint">点击“添加账号”创建第一项</span></div>';return}var h="";items.forEach(function(x,i){var key=keyFor(x),opened=editingKey===key,state=x.enabled===false?"disabled":(x.status||"pending"),options=["codex","kimi","glm-cn","glm-global","deepseek","generic-json","manual"].map(function(p){return '<option value="'+p+'" '+(x.provider===p?"selected":"")+'>'+providerNames[p]+"</option>"}).join("");h+='<div class="quota '+(x.enabled===false?"disabled":"")+'"><div class="quota-head"><div class="identity">'+providerLogo(x.provider,"account-logo")+'<b>'+esc(x.name||"未命名账号")+'</b><span class="badge">'+esc(providerNames[x.provider]||x.provider)+'</span><span class="state '+state+'">'+esc(stateNames[state]||state)+'</span>'+(x.error?'<span class="hint">'+esc(x.error)+'</span>':"")+'</div><div class="actions"><button onclick="toggleEdit('+i+')">'+(opened?"收起":"管理")+'</button><button onclick="toggleEnabled('+i+')">'+(x.enabled===false?"启用":"停用")+'</button><button onclick="moveItem('+i+',-1)">↑</button><button onclick="moveItem('+i+',1)">↓</button><button class="danger" onclick="delItem('+i+')">删除</button></div></div><div class="quota-body '+(opened?"":"hidden")+'">';h+=field(i,"name","屏幕名称")+'<div class="field"><label>供应商<select onchange="items['+i+'].provider=this.value;markDirty();renderQuotas()">'+options+'</select></label></div>';
if(x.provider!=="manual")h+=field(i,"secret",x.has_secret?"新凭据（留空保留现有）":"API KEY / ACCESS TOKEN","password",true)+'<div class="secret-note wide">'+(x.has_secret?'设备中已有凭据，此处不会回显。 <label><input style="width:auto;margin-left:10px" type="checkbox" onchange="items['+i+'].clear_secret=this.checked;markDirty()">清除凭据</label>':"尚未保存凭据。")+'</div>';
if(x.provider!=="manual"){h+='<div class="proxy-box"><label class="proxy-toggle"><input type="checkbox" '+(x.proxy_enabled?"checked":"")+' onchange="items['+i+'].proxy_enabled=this.checked;markDirty();renderQuotas()">查询余额使用代理</label>';if(x.proxy_enabled){var proxyHint=x.has_proxy_auth?"已保存认证代理，留空保留":(x.proxy_endpoint||"http://主机:端口");h+='<div class="field" style="margin-top:9px"><label>CONNECT 代理地址<input type="password" value="" placeholder="'+esc(proxyHint)+'" autocomplete="new-password" oninput="items['+i+'].proxy_url=this.value;markDirty()"></label></div><p class="hint proxy-note">'+(x.proxy_endpoint?"当前端点："+esc(x.proxy_endpoint)+"<br>":"")+'支持 http:// 或 https:// 用户名:密码@主机:端口；凭据不会回显，代理和目标 HTTPS 证书都会校验。</p>'+(x.proxy_endpoint?'<label><input style="width:auto;margin-right:10px" type="checkbox" onchange="items['+i+'].clear_proxy=this.checked;markDirty()">清除已保存代理</label><button type="button" onclick="testProxy()">测试代理</button>':"")}h+='</div>'}
if(x.provider==="codex")h+=field(i,"account_id","ChatGPT Account ID（可选）",null,true);
if(x.provider==="generic-json")h+=field(i,"base_url","JSON URL",null,true)+field(i,"total_field","总额字段")+field(i,"remaining_field","余额字段")+field(i,"label","层级名")+field(i,"unit","单位");
if(x.provider==="manual")h+=field(i,"manual_total","总额","number")+field(i,"manual_remaining","剩余","number")+field(i,"label","层级名")+field(i,"unit","单位");
h+='</div></div>'});el("quotas").innerHTML=h}
function toggleEdit(i){editingKey=editingKey===keyFor(items[i])?"":keyFor(items[i]);renderQuotas()}
function toggleEnabled(i){items[i].enabled=items[i].enabled===false;markDirty();renderQuotas()}
function add(){if(items.length>=32){toast("最多 32 项",true);return}var x={id:0,enabled:true,name:"新账号",provider:"codex",secret:"",proxy_enabled:false,proxy_url:"",status:"pending"};keyFor(x);items.push(x);editingKey=x._ui;markDirty();renderQuotas()}
function delItem(i){if(!confirm("删除账号“"+(items[i].name||"未命名")+"”？保存后生效。"))return;items.splice(i,1);editingKey="";markDirty();renderQuotas()}
function moveItem(i,d){var n=i+d;if(n<0||n>=items.length)return;var x=items[i];items[i]=items[n];items[n]=x;markDirty();renderQuotas()}
async function savePages(){try{await api("/api/pages",{method:"PUT",body:JSON.stringify({pages:pages})});toast("页面顺序已保存")}catch(e){toast(e.message,true)}}
async function saveQuotas(){try{await api("/api/quotas",{method:"PUT",body:JSON.stringify({items:items})});toast("账号更改已保存，准备刷新");await loadAll()}catch(e){toast(e.message,true)}}
async function testProxy(){if(dirty){toast("请先保存账号更改，再测试代理",true);return}try{await api("/api/refresh",{method:"POST"});toast("代理测试已开始，结果会显示在账号状态中");setTimeout(loadAll,3500)}catch(e){toast(e.message,true)}}
async function status(){try{var s=await api("/api/status");csrf=s.csrf||csrf;lastSuccess=Number(s.last_all_success_at)||0;el("ip").textContent=s.ip||"NO NETWORK";var d=lastSuccess?new Date(lastSuccess*1000):null;el("runStatus").className="status "+(s.refreshing?"":"ok");el("runStatus").textContent=s.refreshing?"正在串行刷新…":("上次全部成功："+(d?d.toLocaleString():"尚无"));el("clock").textContent=new Date().toLocaleTimeString();await loadDevice()}catch(e){}}
el("authBtn").onclick=login;el("password").onkeydown=function(e){if(e.key==="Enter")login()};el("addBtn").onclick=add;el("reloadBtn").onclick=function(){if(!dirty||confirm("放弃尚未保存的账号修改？"))loadAll()};el("savePages").onclick=savePages;el("saveQuotas").onclick=saveQuotas;
el("refreshBtn").onclick=async function(){try{await api("/api/refresh",{method:"POST"});toast("刷新请求已排队");status()}catch(e){toast(e.message,true)}};el("logoutBtn").onclick=async function(){try{await api("/api/logout",{method:"POST"})}catch(e){}csrf="";showAuth(false)};
boot();setInterval(status,5000);
</script></body></html>)HTML";

AdminServer* Self(httpd_req_t* req) {
    return static_cast<AdminServer*>(req->user_ctx);
}

esp_err_t Json(httpd_req_t* req, const std::string& body, int status = 200) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (status == 400) httpd_resp_set_status(req, "400 Bad Request");
    else if (status == 401) httpd_resp_set_status(req, "401 Unauthorized");
    else if (status == 403) httpd_resp_set_status(req, "403 Forbidden");
    else if (status == 404) httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t Error(httpd_req_t* req, int status, const std::string& message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message.c_str());
    char* raw = cJSON_PrintUnformatted(root);
    std::string out = raw ? raw : "{\"error\":\"error\"}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);
    return Json(req, out, status);
}

bool ReadBody(httpd_req_t* req, std::string& body) {
    if (req->content_len <= 0 || req->content_len > kMaxRequest) return false;
    std::vector<char> data(req->content_len + 1);
    size_t got = 0;
    while (got < static_cast<size_t>(req->content_len)) {
        int n = httpd_req_recv(req, data.data() + got, req->content_len - got);
        if (n <= 0) return false;
        got += n;
    }
    data[got] = '\0';
    body.assign(data.data(), got);
    return true;
}

std::string PasswordFromBody(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    std::string password;
    if (root) {
        cJSON* value = cJSON_GetObjectItem(root, "password");
        if (cJSON_IsString(value)) password = value->valuestring;
        cJSON_Delete(root);
    }
    return password;
}

std::string HexRandom(size_t bytes) {
    std::vector<uint8_t> raw(bytes);
    esp_fill_random(raw.data(), raw.size());
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (uint8_t value : raw) {
        out.push_back(hex[value >> 4]);
        out.push_back(hex[value & 15]);
    }
    return out;
}

std::string CookieValue(const char* cookie, const char* name) {
    const size_t name_len = strlen(name);
    const char* cursor = cookie;
    while (cursor && *cursor) {
        while (*cursor == ' ' || *cursor == ';') ++cursor;
        const char* end = strchr(cursor, ';');
        if (!end) end = cursor + strlen(cursor);
        const char* equal = static_cast<const char*>(memchr(cursor, '=', end - cursor));
        if (equal && static_cast<size_t>(equal - cursor) == name_len &&
            strncmp(cursor, name, name_len) == 0) {
            return std::string(equal + 1, end);
        }
        cursor = *end ? end + 1 : end;
    }
    return {};
}
}  // namespace

AdminServer& AdminServer::GetInstance() {
    static AdminServer instance;
    return instance;
}

bool AdminServer::HasPassword() const {
    nvs_handle_t handle;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t length = 0;
    const bool exists = nvs_get_blob(handle, "passhash", nullptr, &length) == ESP_OK && length == 32;
    nvs_close(handle);
    return exists;
}

bool AdminServer::SetPassword(const std::string& password) {
    uint8_t salt[16], hash[32];
    esp_fill_random(salt, sizeof(salt));
    std::vector<uint8_t> input(sizeof(salt) + password.size());
    memcpy(input.data(), salt, sizeof(salt));
    memcpy(input.data() + sizeof(salt), password.data(), password.size());
    if (mbedtls_sha256(input.data(), input.size(), hash, 0) != 0) return false;
    nvs_handle_t handle;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, "salt", salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_set_blob(handle, "passhash", hash, sizeof(hash));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool AdminServer::CheckPassword(const std::string& password) const {
    uint8_t salt[16], stored[32], actual[32];
    size_t salt_len = sizeof(salt), hash_len = sizeof(stored);
    nvs_handle_t handle;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    bool ok = nvs_get_blob(handle, "salt", salt, &salt_len) == ESP_OK &&
              nvs_get_blob(handle, "passhash", stored, &hash_len) == ESP_OK &&
              salt_len == sizeof(salt) && hash_len == sizeof(stored);
    nvs_close(handle);
    if (!ok) return false;
    std::vector<uint8_t> input(sizeof(salt) + password.size());
    memcpy(input.data(), salt, sizeof(salt));
    memcpy(input.data() + sizeof(salt), password.data(), password.size());
    if (mbedtls_sha256(input.data(), input.size(), actual, 0) != 0) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(actual); ++i) diff |= actual[i] ^ stored[i];
    return diff == 0;
}

void AdminServer::CreateSession(std::string& sid, std::string& csrf) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    session_id_ = HexRandom(16);
    csrf_token_ = HexRandom(16);
    session_seen_us_ = esp_timer_get_time();
    sid = session_id_;
    csrf = csrf_token_;
}

bool AdminServer::IsAuthorized(httpd_req_t* req, bool csrf) {
    char cookie[384] = {};
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) return false;
    const std::string sid = CookieValue(cookie, "sid");
    if (sid.empty()) return false;
    std::lock_guard<std::mutex> lock(session_mutex_);
    const int64_t now = esp_timer_get_time();
    if (session_id_.empty() || sid != session_id_ || now - session_seen_us_ > kSessionTimeoutUs) {
        session_id_.clear();
        return false;
    }
    if (csrf) {
        char token[80] = {};
        if (httpd_req_get_hdr_value_str(req, "X-CSRF-Token", token, sizeof(token)) != ESP_OK ||
            token != csrf_token_) return false;
    }
    session_seen_us_ = now;
    return true;
}

std::string AdminServer::GetApiToken(bool regenerate) {
    nvs_handle_t handle;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle) != ESP_OK) return "";
    char token[80] = {}; size_t len = sizeof(token);
    if (regenerate || nvs_get_str(handle, "api_token", token, &len) != ESP_OK) {
        std::string generated = HexRandom(24);
        nvs_set_str(handle, "api_token", generated.c_str()); nvs_commit(handle); nvs_close(handle);
        return generated;
    }
    nvs_close(handle); return token;
}

bool AdminServer::IsApiAuthorized(httpd_req_t* req, bool csrf) {
    if (IsAuthorized(req, csrf)) return true;
    char auth[128] = {};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK) return false;
    constexpr const char* prefix = "Bearer ";
    return strncmp(auth, prefix, strlen(prefix)) == 0 && GetApiToken(false) == auth + strlen(prefix);
}

bool AdminServer::Start() {
    if (server_) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.ctrl_port = 32769;
    config.max_uri_handlers = 34;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "后台 HTTP 服务启动失败");
        return false;
    }
    const httpd_uri_t routes[] = {
        {"/admin", HTTP_GET, PageHandler, this}, {"/", HTTP_GET, PageHandler, this},
        {"/api/setup", HTTP_POST, SetupHandler, this}, {"/api/login", HTTP_POST, LoginHandler, this},
        {"/api/logout", HTTP_POST, LogoutHandler, this}, {"/api/status", HTTP_GET, StatusHandler, this},
        {"/api/pages", HTTP_GET, PagesGetHandler, this}, {"/api/pages", HTTP_PUT, PagesPutHandler, this},
        {"/api/quotas", HTTP_GET, QuotasGetHandler, this}, {"/api/quotas", HTTP_PUT, QuotasPutHandler, this},
        {"/api/refresh", HTTP_POST, RefreshHandler, this},
        {"/api/refresh-interval", HTTP_GET, RefreshIntervalGetHandler, this},
        {"/api/refresh-interval", HTTP_PUT, RefreshIntervalPutHandler, this},
        {"/api/proxy-diagnostic", HTTP_POST, ProxyDiagnosticHandler, this},
        {"/api/todos", HTTP_GET, TodosHandler, this}, {"/api/todos", HTTP_POST, TodosHandler, this},
        {"/api/todos/*", HTTP_GET, TodoItemHandler, this}, {"/api/todos/*", HTTP_PUT, TodoItemHandler, this},
        {"/api/todos/*", HTTP_DELETE, TodoItemHandler, this},
        {"/api/weather", HTTP_GET, WeatherGetHandler, this}, {"/api/weather", HTTP_PUT, WeatherPutHandler, this},
        {"/api/weather-diagnostic", HTTP_GET, WeatherDiagnosticHandler, this},
        {"/api/calendar", HTTP_GET, CalendarGetHandler, this}, {"/api/calendar", HTTP_PUT, CalendarPutHandler, this},
        {"/api/calendar/sync", HTTP_POST, CalendarSyncHandler, this},
        {"/api/api-token", HTTP_GET, ApiTokenHandler, this}, {"/api/api-token", HTTP_POST, ApiTokenHandler, this},
        {"/api/device", HTTP_GET, DeviceHandler, this}, {"/api/device", HTTP_PUT, DeviceHandler, this},
        {"/api/display/switch", HTTP_POST, DisplaySwitchHandler, this},
    };
    for (const auto& route : routes) httpd_register_uri_handler(server_, &route);
    ESP_LOGI(TAG, "局域网后台已启动: http://<device-ip>:8080/admin");
    return true;
}

esp_err_t AdminServer::PageHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kAdminHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t AdminServer::SetupHandler(httpd_req_t* req) {
    auto* self = Self(req);
    if (self->HasPassword()) return Error(req, 403, "管理员已设置");
    std::string body;
    if (!ReadBody(req, body)) return Error(req, 400, "请求无效");
    const std::string password = PasswordFromBody(body);
    if (password.size() < 8 || password.size() > 72) return Error(req, 400, "密码需为 8-72 位");
    if (!self->SetPassword(password)) return Error(req, 400, "保存密码失败");
    std::string sid, csrf;
    self->CreateSession(sid, csrf);
    const std::string cookie = "sid=" + sid + "; Path=/; HttpOnly; SameSite=Strict";
    httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
    return Json(req, "{\"csrf\":\"" + csrf + "\"}");
}

esp_err_t AdminServer::LoginHandler(httpd_req_t* req) {
    auto* self = Self(req);
    std::string body;
    if (!ReadBody(req, body)) return Error(req, 400, "请求无效");
    if (!self->CheckPassword(PasswordFromBody(body))) {
        vTaskDelay(pdMS_TO_TICKS(400));
        return Error(req, 401, "密码错误");
    }
    std::string sid, csrf;
    self->CreateSession(sid, csrf);
    const std::string cookie = "sid=" + sid + "; Path=/; HttpOnly; SameSite=Strict";
    httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
    return Json(req, "{\"csrf\":\"" + csrf + "\"}");
}

esp_err_t AdminServer::LogoutHandler(httpd_req_t* req) {
    auto* self = Self(req);
    if (!self->IsAuthorized(req, true)) return Error(req, 401, "未登录");
    {
        std::lock_guard<std::mutex> lock(self->session_mutex_);
        self->session_id_.clear();
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    return Json(req, "{\"ok\":true}");
}

esp_err_t AdminServer::StatusHandler(httpd_req_t* req) {
    auto* self = Self(req);
    const bool setup = !self->HasPassword();
    const bool authenticated = !setup && self->IsAuthorized(req, false);
    cJSON* root = cJSON_CreateObject();
    if (rlcd::StatusVisibilityFor(authenticated) == rlcd::StatusVisibility::kIncludePrivateQuota) {
        cJSON* private_status = cJSON_Parse(QuotaManager::GetInstance().GetStatusJson().c_str());
        if (private_status) {
            cJSON_Delete(root);
            root = private_status;
        }
    }
    cJSON_AddBoolToObject(root, "setup_required", setup);
    cJSON_AddBoolToObject(root, "authenticated", authenticated);
    cJSON_AddStringToObject(root, "ip", WifiManager::GetInstance().GetIpAddress().c_str());
    if (authenticated) {
        std::lock_guard<std::mutex> lock(self->session_mutex_);
        cJSON_AddStringToObject(root, "csrf", self->csrf_token_.c_str());
    }
    char* raw = cJSON_PrintUnformatted(root);
    std::string out = raw ? raw : "{}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);
    return Json(req, out);
}

esp_err_t AdminServer::PagesGetHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, false)) return Error(req, 401, "未登录");
    return Json(req, QuotaManager::GetInstance().GetPageConfigJson());
}

esp_err_t AdminServer::PagesPutHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, true)) return Error(req, 401, "未登录");
    std::string body, error;
    if (!ReadBody(req, body)) return Error(req, 400, "请求无效");
    if (!QuotaManager::GetInstance().ApplyPageConfigJson(body.c_str(), error)) return Error(req, 400, error);
    return Json(req, "{\"ok\":true}");
}

esp_err_t AdminServer::QuotasGetHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, false)) return Error(req, 401, "未登录");
    return Json(req, QuotaManager::GetInstance().GetConfigJson());
}

esp_err_t AdminServer::QuotasPutHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, true)) return Error(req, 401, "未登录");
    std::string body, error;
    if (!ReadBody(req, body)) return Error(req, 400, "请求无效");
    if (!QuotaManager::GetInstance().ApplyConfigJson(body.c_str(), error)) return Error(req, 400, error);
    return Json(req, "{\"ok\":true}");
}

esp_err_t AdminServer::RefreshHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, true)) return Error(req, 401, "未登录");
    QuotaManager::GetInstance().RequestRefresh();
    return Json(req, "{\"ok\":true}");
}

esp_err_t AdminServer::RefreshIntervalGetHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, false)) return Error(req, 401, "未登录");
    return Json(req, "{\"minutes\":" + std::to_string(QuotaManager::GetInstance().GetRefreshIntervalMinutes()) + "}");
}

esp_err_t AdminServer::RefreshIntervalPutHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, true)) return Error(req, 401, "未登录");
    std::string body, error;
    if (!ReadBody(req, body)) return Error(req, 400, "请求无效");
    cJSON* root = cJSON_Parse(body.c_str());
    cJSON* minutes = root ? cJSON_GetObjectItem(root, "minutes") : nullptr;
    const int value = cJSON_IsNumber(minutes) ? static_cast<int>(minutes->valuedouble) : 0;
    const bool is_integer = cJSON_IsNumber(minutes) && minutes->valuedouble == value;
    if (root) cJSON_Delete(root);
    if (!is_integer || !QuotaManager::GetInstance().SetRefreshIntervalMinutes(value, error)) {
        return Error(req, 400, error.empty() ? "刷新间隔必须为 1-60 分钟" : error);
    }
    return Json(req, "{\"ok\":true,\"minutes\":" + std::to_string(value) + "}");
}

esp_err_t AdminServer::ProxyDiagnosticHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, true)) return Error(req, 401, "未登录");
    return Json(req, QuotaManager::GetInstance().GetProxyDiagnosticJson());
}

esp_err_t AdminServer::TodosHandler(httpd_req_t* req) {
    auto* self = Self(req);
    if (!self->IsApiAuthorized(req, req->method != HTTP_GET)) return Error(req, 401, "未授权");
    auto& manager = TodoManager::GetInstance(); manager.Init();
    if (req->method == HTTP_GET) return Json(req, manager.ToJson());
    std::string body, error; TodoItem created;
    if (!ReadBody(req, body) || !manager.Create(body.c_str(), created, error)) return Error(req, 400, error.empty() ? "请求无效" : error);
    ScheduleTodoDisplayRefresh();
    cJSON* root = cJSON_Parse(manager.ToJson().c_str());
    std::string out = "{\"ok\":true,\"id\":\"" + created.id + "\"}";
    if (root) cJSON_Delete(root);
    return Json(req, out);
}

esp_err_t AdminServer::TodoItemHandler(httpd_req_t* req) {
    auto* self = Self(req);
    if (!self->IsApiAuthorized(req, req->method != HTTP_GET)) return Error(req, 401, "未授权");
    const char* slash = strrchr(req->uri, '/');
    std::string id = slash ? slash + 1 : "";
    if (id.empty()) return Error(req, 400, "缺少待办 ID");
    auto& manager = TodoManager::GetInstance(); manager.Init();
    if (req->method == HTTP_GET) {
        for (const auto& item : manager.List()) if (item.id == id) {
            cJSON* root = cJSON_CreateObject(); cJSON_AddStringToObject(root, "id", item.id.c_str());
            cJSON_AddStringToObject(root, "content", item.content.c_str()); cJSON_AddStringToObject(root, "due_date", item.due_date.c_str());
            cJSON_AddStringToObject(root, "due_time", item.due_time.c_str()); cJSON_AddBoolToObject(root, "completed", item.completed);
            char* raw = cJSON_PrintUnformatted(root); std::string out = raw ? raw : "{}"; if (raw) cJSON_free(raw); cJSON_Delete(root); return Json(req, out);
        }
        return Error(req, 404, "待办不存在");
    }
    std::string error;
    if (req->method == HTTP_DELETE) {
        if (!manager.Remove(id, error)) return Error(req, 404, error);
        ScheduleTodoDisplayRefresh();
    } else {
        std::string body; if (!ReadBody(req, body) || !manager.Update(id, body.c_str(), error)) return Error(req, 400, error.empty() ? "请求无效" : error);
        ScheduleTodoDisplayRefresh();
    }
    return Json(req, "{\"ok\":true}");
}

esp_err_t AdminServer::WeatherGetHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, false)) return Error(req, 401, "未登录");
    return Json(req, WeatherManager::getInstance().getLocationConfigJson());
}
esp_err_t AdminServer::WeatherPutHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, true)) return Error(req, 401, "未登录");
    std::string body, error; if (!ReadBody(req, body) || !WeatherManager::getInstance().applyLocationConfigJson(body.c_str(), error)) return Error(req, 400, error);
    return Json(req, "{\"ok\":true}");
}
esp_err_t AdminServer::WeatherDiagnosticHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, false)) return Error(req, 401, "未登录");
    return Json(req, WeatherManager::getInstance().GetDiagnosticJson());
}
esp_err_t AdminServer::CalendarGetHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, false)) return Error(req, 401, "未登录");
    return Json(req, CalendarManager::GetInstance().GetConfigJson());
}
esp_err_t AdminServer::CalendarPutHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, true)) return Error(req, 401, "未登录");
    std::string body, error; if (!ReadBody(req, body) || !CalendarManager::GetInstance().ApplyConfigJson(body.c_str(), error)) return Error(req, 400, error);
    return Json(req, "{\"ok\":true}");
}
esp_err_t AdminServer::CalendarSyncHandler(httpd_req_t* req) {
    if (!Self(req)->IsAuthorized(req, true)) return Error(req, 401, "未登录");
    time_t now = time(nullptr); struct tm local = {}; localtime_r(&now, &local);
    if (!CalendarManager::GetInstance().SyncYear(local.tm_year + 1900)) return Error(req, 400, "同步失败");
    return Json(req, "{\"ok\":true}");
}
esp_err_t AdminServer::ApiTokenHandler(httpd_req_t* req) {
    auto* self = Self(req); if (!self->IsAuthorized(req, req->method == HTTP_POST)) return Error(req, 401, "未登录");
    std::string token = self->GetApiToken(req->method == HTTP_POST);
    return Json(req, "{\"token\":\"" + token + "\"}");
}

esp_err_t AdminServer::DeviceHandler(httpd_req_t* req) {
    auto* self = Self(req);
    if (!self->IsAuthorized(req, req->method == HTTP_PUT)) return Error(req, 401, "未登录");
    if (req->method == HTTP_PUT) {
        std::string body;
        if (!ReadBody(req, body)) return Error(req, 400, "请求无效");
        cJSON* root = cJSON_Parse(body.c_str());
        cJSON* volume_item = root ? cJSON_GetObjectItem(root, "volume") : nullptr;
        if (!cJSON_IsNumber(volume_item) || volume_item->valuedouble < 0 ||
            volume_item->valuedouble > 100 ||
            volume_item->valuedouble != static_cast<int>(volume_item->valuedouble)) {
            if (root) cJSON_Delete(root);
            return Error(req, 400, "音量必须是 0-100 的整数");
        }
        const int volume = volume_item->valueint;
        cJSON_Delete(root);
        Application::GetInstance().Schedule([volume]() {
            auto* codec = Board::GetInstance().GetAudioCodec();
            if (codec) codec->SetOutputVolume(volume);
        });
        return Json(req, "{\"ok\":true,\"volume\":" + std::to_string(volume) + "}");
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "volume", codec ? codec->output_volume() : 0);
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000LL);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "minimum_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(root, "ip", WifiManager::GetInstance().GetIpAddress().c_str());
    char* raw = cJSON_PrintUnformatted(root);
    std::string out = raw ? raw : "{}";
    if (raw) cJSON_free(raw);
    cJSON_Delete(root);
    return Json(req, out);
}

esp_err_t AdminServer::DisplaySwitchHandler(httpd_req_t* req) {
    auto* self = Self(req);
    if (!self->IsAuthorized(req, true)) return Error(req, 401, "未登录");
    std::string body;
    if (!ReadBody(req, body)) return Error(req, 400, "请求无效");
    cJSON* root = cJSON_Parse(body.c_str());
    cJSON* mode_item = root ? cJSON_GetObjectItem(root, "mode") : nullptr;
    std::string mode = cJSON_IsString(mode_item) ? mode_item->valuestring : "";
    if (root) cJSON_Delete(root);
    // 白名单校验，与 ScheduleDisplaySwitch 的分支一致
    if (mode != "toggle" && mode != "overview" && mode != "weather" && mode != "calendar" &&
        mode != "forecast" && mode != "quota") {
        return Error(req, 400, "无效的页面标识");
    }
    ScheduleDisplaySwitch(mode);
    return Json(req, "{\"ok\":true,\"mode\":\"" + mode + "\"}");
}
