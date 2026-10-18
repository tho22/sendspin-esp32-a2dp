#include "web_ui.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

static const char *const TAG = "web_ui";

// The log is only captured while a page shows it: each captured line costs CPU in the task that logs it
static constexpr int64_t LOG_CAPTURE_TIMEOUT_US = 10000000;
static constexpr size_t LOG_BUFFER_SIZE = 8192;
static constexpr size_t LOG_LINE_MAX = 256;
static constexpr size_t LOG_RESPONSE_MAX = 6144;

static A2dpOutput *g_output = nullptr;
static WebUiPlayer *g_player = nullptr;

static RingbufHandle_t g_log_ring = nullptr;
static std::atomic<int64_t> g_log_poll_us{0};
static vprintf_like_t g_prev_vprintf = nullptr;
static esp_log_level_t g_log_level = ESP_LOG_INFO;
// DEBUG only applies to the application's own tags; the IDF libraries stay at INFO at most (their DEBUG output,
// e.g. httpd per packet, floods the log and costs Bluetooth airtime)
static constexpr const char *APP_TAGS[] = {"a2dp", "main", "web_ui"};

static const char INDEX_HTML[] = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>)html" CONFIG_APP_FRIENDLY_NAME R"html(</title>
<style>
:root{--bg:#fff;--fg:#222;--box:#eef1f6;--muted:#666}
@media (prefers-color-scheme:dark){:root{--bg:#111;--fg:#ddd;--box:#1d2330;--muted:#999}}
body{font-family:sans-serif;margin:16px auto;max-width:600px;padding:0 16px;background:var(--bg);color:var(--fg)}
h1{font-size:1.3em}h2{font-size:1.05em;margin:20px 0 6px}
.box{padding:10px 12px;border-radius:6px;background:var(--box);margin:8px 0}
.muted{color:var(--muted)}
button{padding:8px 14px;margin:4px 4px 4px 0;font-size:1em}
input[type=range]{width:100%}
li{margin:6px 0}
pre{background:var(--box);padding:8px;max-height:320px;overflow:auto;font-size:.8em;white-space:pre-wrap}
</style></head><body>
<h1>)html" CONFIG_APP_FRIENDLY_NAME R"html(</h1>

<h2>Now playing</h2>
<div class="box"><b id="title">-</b><br><span id="artist"></span><br><span class="muted" id="album"></span></div>
<button onclick="post('command?name=previous')">&#9198; Previous</button>
<button onclick="post('command?name=play_pause')" id="pp">Play/Pause</button>
<button onclick="post('command?name=next')">&#9197; Next</button>
<div>Volume <span id="volval"></span>
<input type="range" id="volume" min="0" max="100" step="1"
 oninput="dragging=true;$('volval').textContent=this.value+' %'"
 onchange="dragging=false;post('volume?value='+this.value)"></div>

<h2>Bluetooth speaker</h2>
<div class="box">Status: <b id="status">...</b></div>
<button id="scan" onclick="post('scan')">Search speakers</button>
<button onclick="if(confirm('Forget the speaker?'))post('unpair')">Unpair speaker</button>
<button onclick="post('repair')">Re-pair speaker</button>
<p class="muted">Put the speaker into pairing mode, search, then pair it:</p>
<ul id="devices"></ul>

<h2>Diagnostics</h2>
<div class="box">WiFi signal: <span id="rssi"></span> dBm &middot; Free internal RAM: <span id="heap"></span> B</div>
<div>Log level <select id="level" onchange="post('log_level?level='+this.value)">
<option value="1">ERROR</option><option value="2">WARN</option><option value="3">INFO</option>
<option value="4">DEBUG</option></select>
<label><input type="checkbox" id="showlog" onchange="$('logbox').hidden=!this.checked"> Show log</label></div>
<pre id="logbox" hidden></pre>

<script>
let dragging=false;
const $=id=>document.getElementById(id);
function post(action){fetch('/api/'+action,{method:'POST'}).then(update);}
function update(){
  fetch('/api/status').then(r=>r.json()).then(s=>{
    $('status').textContent=s.status;
    $('scan').disabled=s.searching;
    $('title').textContent=s.title||'-';
    $('artist').textContent=s.artist;
    $('album').textContent=s.album;
    $('pp').innerHTML=s.playing?'&#9208; Pause':'&#9654; Play';
    if(!dragging){$('volume').value=s.volume;$('volval').textContent=s.volume+' %';}
    $('rssi').textContent=s.rssi;
    $('heap').textContent=s.heap;
    if(document.activeElement!==$('level'))$('level').value=s.log_level;
    const list=$('devices');
    list.innerHTML='';
    if(!s.devices.length){list.innerHTML='<li class="muted">No speakers found yet</li>';}
    s.devices.forEach((d,i)=>{
      const li=document.createElement('li');
      const b=document.createElement('button');
      b.textContent='Pair';b.onclick=()=>post('pair?index='+i);
      li.append(b,' '+d);list.append(li);
    });
  }).catch(()=>{});
}
function pollLog(){
  if(!$('showlog').checked)return;
  fetch('/api/log').then(r=>r.text()).then(t=>{
    if(!t)return;
    const box=$('logbox');
    const atEnd=box.scrollTop+box.clientHeight>=box.scrollHeight-4;
    box.textContent=(box.textContent+t.replace(/\x1b\[[0-9;]*m/g,'')).slice(-20000);
    if(atEnd)box.scrollTop=box.scrollHeight;
  }).catch(()=>{});
}
update();setInterval(update,2000);setInterval(pollLog,1000);
</script></body></html>)html";

// ---------------------------------------------------------------------------------------------------------------
// Log capture
// ---------------------------------------------------------------------------------------------------------------

static int log_vprintf(const char *format, va_list args) {
  if (g_log_ring != nullptr && esp_timer_get_time() - g_log_poll_us.load() < LOG_CAPTURE_TIMEOUT_US) {
    char line[LOG_LINE_MAX];
    va_list copy;
    va_copy(copy, args);
    const int len = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    if (len > 0) {
      // Never blocks: a full buffer drops the line
      xRingbufferSend(g_log_ring, line, std::min<size_t>(len, sizeof(line) - 1), 0);
    }
  }
  return g_prev_vprintf(format, args);
}

// ---------------------------------------------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------------------------------------------

static void append_json_string(std::string &out, const std::string &value) {
  out += '"';
  for (const char c : value) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += c;
    }
  }
  out += '"';
}

static bool query_value(httpd_req_t *req, const char *key, char *value, size_t size) {
  char query[64];
  return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
         httpd_query_key_value(query, key, value, size) == ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req) {
  const WebUiPlayer::State state = g_player->get_state();
  wifi_ap_record_t ap{};
  const int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;

  std::string json = "{\"status\":";
  append_json_string(json, g_output->get_status_text());
  json += ",\"searching\":";
  json += g_output->is_searching() ? "true" : "false";
  json += ",\"devices\":[";
  bool first = true;
  for (const auto &device : g_output->get_found_devices()) {
    if (!first) {
      json += ',';
    }
    first = false;
    append_json_string(json, device);
  }
  json += "],\"title\":";
  append_json_string(json, state.title);
  json += ",\"artist\":";
  append_json_string(json, state.artist);
  json += ",\"album\":";
  append_json_string(json, state.album);
  char buf[128];
  snprintf(buf, sizeof(buf), ",\"volume\":%u,\"playing\":%s,\"rssi\":%d,\"heap\":%u,\"log_level\":%d}",
           static_cast<unsigned>(state.volume), state.playing ? "true" : "false", rssi,
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)), static_cast<int>(g_log_level));
  json += buf;
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json.c_str(), json.size());
}

static esp_err_t log_handler(httpd_req_t *req) {
  if (g_log_ring == nullptr) {
    g_log_ring = xRingbufferCreateWithCaps(LOG_BUFFER_SIZE, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
  }
  g_log_poll_us = esp_timer_get_time();
  std::string text;
  size_t size = 0;
  void *item;
  while (text.size() < LOG_RESPONSE_MAX && g_log_ring != nullptr &&
         (item = xRingbufferReceive(g_log_ring, &size, 0)) != nullptr) {
    text.append(static_cast<const char *>(item), size);
    vRingbufferReturnItem(g_log_ring, item);
  }
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, text.data(), text.size());
}

static esp_err_t action_handler(httpd_req_t *req) {
  const char *action = static_cast<const char *>(req->user_ctx);
  char value[16];
  if (strcmp(action, "scan") == 0) {
    g_output->request_scan();
  } else if (strcmp(action, "unpair") == 0) {
    g_output->request_unpair();
  } else if (strcmp(action, "repair") == 0) {
    g_output->request_repair();
  } else if (strcmp(action, "pair") == 0) {
    if (!query_value(req, "index", value, sizeof(value))) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "index missing");
    }
    g_output->request_pair(static_cast<size_t>(atoi(value)));
  } else if (strcmp(action, "volume") == 0) {
    if (!query_value(req, "value", value, sizeof(value))) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "value missing");
    }
    g_player->request_volume(static_cast<uint8_t>(std::clamp(atoi(value), 0, 100)));
  } else if (strcmp(action, "command") == 0) {
    if (!query_value(req, "name", value, sizeof(value))) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name missing");
    }
    if (strcmp(value, "play_pause") == 0) {
      g_player->request_command(WebUiPlayer::Command::PLAY_PAUSE);
    } else if (strcmp(value, "next") == 0) {
      g_player->request_command(WebUiPlayer::Command::NEXT);
    } else if (strcmp(value, "previous") == 0) {
      g_player->request_command(WebUiPlayer::Command::PREVIOUS);
    }
  } else if (strcmp(action, "log_level") == 0) {
    if (!query_value(req, "level", value, sizeof(value))) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "level missing");
    }
    g_log_level = static_cast<esp_log_level_t>(std::clamp(atoi(value), 1, 4));
    esp_log_level_set("*", std::min(g_log_level, ESP_LOG_INFO));
    for (const char *tag : APP_TAGS) {
      esp_log_level_set(tag, g_log_level);
    }
    ESP_LOGI(TAG, "Log level %d", static_cast<int>(g_log_level));
  }
  return httpd_resp_send(req, nullptr, 0);
}

bool web_ui_start(A2dpOutput *output, WebUiPlayer *player) {
  g_output = output;
  g_player = player;
  g_prev_vprintf = esp_log_set_vprintf(log_vprintf);

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  // Default control port: Sendspin's own httpd moves to the next one to make room for a web server
  config.max_open_sockets = 3;
  config.max_uri_handlers = 12;
  config.lru_purge_enable = true;
  // Off the Bluetooth core
  config.core_id = 1;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start the web server");
    return false;
  }
  static const httpd_uri_t URIS[] = {
      {"/", HTTP_GET, index_handler, nullptr},
      {"/api/status", HTTP_GET, status_handler, nullptr},
      {"/api/log", HTTP_GET, log_handler, nullptr},
      {"/api/scan", HTTP_POST, action_handler, const_cast<char *>("scan")},
      {"/api/unpair", HTTP_POST, action_handler, const_cast<char *>("unpair")},
      {"/api/repair", HTTP_POST, action_handler, const_cast<char *>("repair")},
      {"/api/pair", HTTP_POST, action_handler, const_cast<char *>("pair")},
      {"/api/volume", HTTP_POST, action_handler, const_cast<char *>("volume")},
      {"/api/command", HTTP_POST, action_handler, const_cast<char *>("command")},
      {"/api/log_level", HTTP_POST, action_handler, const_cast<char *>("log_level")},
  };
  for (const auto &uri : URIS) {
    httpd_register_uri_handler(server, &uri);
  }
  ESP_LOGI(TAG, "Web interface on port 80");
  return true;
}
