#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiUdp.h>
#include <ESPping.h>
#include "esp_netif.h"
#include "esp_wifi.h"
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"

// Telegram (HTTPS)
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// --------------------------- Ajustes ---------------------------
static const char* DEFAULT_AP_SSID = "ESP32-SETUP";
static const char* DEFAULT_AP_PASS = "12345678";
static const unsigned long WIFI_CONNECT_TIMEOUT = 15000;
static const int MAX_DEVICES = 25;
static const unsigned long PING_CHECK_EVERY_MS = 8000;
static const unsigned long AUTO_WOL_MIN_GAP_MS = 5000;

// --------------------------- Globals ---------------------------
WebServer server(80);
Preferences prefs;

// Credenciales Web (dinámicas, guardadas en Preferences)
String g_web_user = "admin";
String g_web_pass = "admin123";

// Nombre del dispositivo
String g_device_name = "ESP32-Panel";

// STA
String g_sta_ssid;
String g_sta_pass;
bool g_sta_dhcp = true;
IPAddress g_sta_ip, g_sta_gw, g_sta_mask, g_sta_dns1, g_sta_dns2;

// AP
bool g_ap_enabled = false;
String g_ap_ssid = DEFAULT_AP_SSID;
String g_ap_pass = DEFAULT_AP_PASS;
IPAddress g_ap_ip(192, 168, 4, 1);
IPAddress g_ap_gw(192, 168, 4, 1);
IPAddress g_ap_mask(255, 255, 255, 0);
IPAddress g_ap_dhcp_start(192, 168, 4, 100);
IPAddress g_ap_dhcp_end(192, 168, 4, 200);

// WOL
uint16_t g_wol_port = 9;

// Telegram
bool g_tg_enabled = false;
String g_tg_token;
String g_tg_chatid;

// Devices
struct Device {
  String name;
  String mac;
  String ip;
  String subnetMask;   // para WOL dirigido
  bool online = false;
  unsigned long lastCheckMs = 0;
  bool autoWol = false;
  uint16_t autoEveryMin = 5;
  unsigned long lastAutoWolMs = 0;
};
Device g_devices[MAX_DEVICES];
int g_deviceCount = 0;

// --------------------------- Utilidades ---------------------------
bool authOrRequest() {
  if (!server.authenticate(g_web_user.c_str(), g_web_pass.c_str())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

String htmlEscape(const String& s) {
  String out; out.reserve(s.length() + 20);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

String ipToString(const IPAddress& ip) {
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}

bool parseIP(const String& s, IPAddress& out) {
  String t = s; t.trim();
  if (!t.length()) return false;
  return out.fromString(t);
}

bool isValidMac(const String& mac) {
  if (mac.length() != 17) return false;
  for (int i = 0; i < 17; i++) {
    if ((i % 3) == 2) {
      if (mac[i] != ':' && mac[i] != '-') return false;
    } else {
      char c = mac[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
  }
  return true;
}

String normalizeMac(String mac) {
  mac.replace('-', ':');
  mac.toUpperCase();
  return mac;
}

bool sameSubnet(const IPAddress& a, const IPAddress& b, const IPAddress& mask) {
  return ((uint32_t)a & (uint32_t)mask) == ((uint32_t)b & (uint32_t)mask);
}

String staState() {
  return (WiFi.status() == WL_CONNECTED) ? "Conectado" : "No conectado";
}

// --------------------------- Telegram ---------------------------
bool telegramNotifyResult(const String& msg) {
  if (!g_tg_enabled) return false;
  if (WiFi.status() != WL_CONNECTED) return false; // requiere STA con Internet
  if (!g_tg_token.length() || !g_tg_chatid.length()) return false;

  WiFiClientSecure client;
  client.setInsecure(); // simplifica TLS (sin validar certificado)

  HTTPClient https;
  String url = "https://api.telegram.org/bot" + g_tg_token + "/sendMessage";

  if (!https.begin(client, url)) return false;
  https.addHeader("Content-Type", "application/json");

  String payload = "{\"chat_id\":\"" + g_tg_chatid + "\",\"text\":\"" + msg + "\"}";
  int code = https.POST(payload);
  String body = https.getString();
  https.end();

  if (code != 200) return false;
  if (body.indexOf("\"ok\":true") < 0) return false;
  return true;
}

void telegramNotify(const String& msg) {
  (void)telegramNotifyResult(msg);
}

// --------------------------- Storage ---------------------------
IPAddress readIPFromPrefs(Preferences& p, const char* key, const IPAddress& def) {
  String s = p.getString(key, def.toString());
  IPAddress ip;
  if (ip.fromString(s)) return ip;
  return def;
}

// Web Auth
void loadWebAuth() {
  prefs.begin("webauth", true);
  g_web_user = prefs.getString("user", "admin");
  g_web_pass = prefs.getString("pass", "admin123");
  prefs.end();
}
void saveWebAuth(const String& user, const String& pass) {
  prefs.begin("webauth", false);
  prefs.putString("user", user);
  prefs.putString("pass", pass);
  prefs.end();
  g_web_user = user;
  g_web_pass = pass;
}

void loadConfig() {
  prefs.begin("general", true);
  g_device_name = prefs.getString("name", "ESP32-Panel");
  prefs.end();

  prefs.begin("sta", true);
  g_sta_ssid = prefs.getString("ssid", "");
  g_sta_pass = prefs.getString("pass", "");
  prefs.end();

  prefs.begin("staip", true);
  g_sta_dhcp = prefs.getBool("dhcp", true);
  g_sta_ip = readIPFromPrefs(prefs, "ip", IPAddress(0,0,0,0));
  g_sta_gw = readIPFromPrefs(prefs, "gw", IPAddress(0,0,0,0));
  g_sta_mask = readIPFromPrefs(prefs, "mask", IPAddress(0,0,0,0));
  g_sta_dns1 = readIPFromPrefs(prefs, "dns1", IPAddress(0,0,0,0));
  g_sta_dns2 = readIPFromPrefs(prefs, "dns2", IPAddress(0,0,0,0));
  prefs.end();

  prefs.begin("ap", true);
  g_ap_enabled = prefs.getBool("enabled", false);
  g_ap_ssid = prefs.getString("ssid", DEFAULT_AP_SSID);
  g_ap_pass = prefs.getString("pass", DEFAULT_AP_PASS);
  prefs.end();

  prefs.begin("apip", true);
  g_ap_ip = readIPFromPrefs(prefs, "ip", IPAddress(192,168,4,1));
  g_ap_gw = readIPFromPrefs(prefs, "gw", IPAddress(192,168,4,1));
  g_ap_mask = readIPFromPrefs(prefs, "mask", IPAddress(255,255,255,0));
  prefs.end();

  prefs.begin("apdhcp", true);
  g_ap_dhcp_start = readIPFromPrefs(prefs, "start", IPAddress(192,168,4,100));
  g_ap_dhcp_end = readIPFromPrefs(prefs, "end", IPAddress(192,168,4,200));
  prefs.end();

  prefs.begin("wol", true);
  g_wol_port = prefs.getUShort("port", 9);
  prefs.end();

  // Telegram
  prefs.begin("telegram", true);
  g_tg_enabled = prefs.getBool("enabled", false);
  g_tg_token   = prefs.getString("token", "");
  g_tg_chatid  = prefs.getString("chatid", "");
  prefs.end();

  prefs.begin("devices", true);
  String blob = prefs.getString("list", "");
  prefs.end();

  g_deviceCount = 0;
  int start = 0;
  while (start < (int)blob.length() && g_deviceCount < MAX_DEVICES) {
    int end = blob.indexOf('\n', start);
    if (end < 0) end = blob.length();
    String line = blob.substring(start, end);
    line.trim();
    if (line.length() == 0) { start = end + 1; continue; }

    int p1 = line.indexOf('|');
    int p2 = (p1 >= 0) ? line.indexOf('|', p1 + 1) : -1;
    int p3 = (p2 >= 0) ? line.indexOf('|', p2 + 1) : -1;
    int p4 = (p3 >= 0) ? line.indexOf('|', p3 + 1) : -1;
    int p5 = (p4 >= 0) ? line.indexOf('|', p4 + 1) : -1;

    if (p1 <= 0 || p2 <= p1 || p3 < p2) { start = end + 1; continue; }

    String name = line.substring(0, p1); name.trim();
    String mac = line.substring(p1 + 1, p2); mac.trim();
    String ip = line.substring(p2 + 1, p3); ip.trim();
    String mask = (p4 >= 0) ? line.substring(p3 + 1, p4) : ""; mask.trim();
    String autoStr = (p5 >= 0) ? line.substring(p4 + 1, p5) : line.substring(p3 + 1); autoStr.trim();
    String minStr = (p5 >= 0) ? line.substring(p5 + 1) : "5"; minStr.trim();

    bool autoWol = (autoStr == "1");
    int mins = minStr.toInt();
    if (mins < 1 || mins > 1440) mins = 5;

    mac = normalizeMac(mac);
    if (name.length() == 0 || !isValidMac(mac)) { start = end + 1; continue; }

    if (ip.length()) {
      IPAddress tmp;
      if (!tmp.fromString(ip)) ip = "";
    }
    if (mask.length()) {
      IPAddress tmp;
      if (!tmp.fromString(mask)) mask = "";
    }

    g_devices[g_deviceCount].name = name;
    g_devices[g_deviceCount].mac = mac;
    g_devices[g_deviceCount].ip = ip;
    g_devices[g_deviceCount].subnetMask = mask;
    g_devices[g_deviceCount].online = false;
    g_devices[g_deviceCount].lastCheckMs = 0;
    g_devices[g_deviceCount].autoWol = autoWol;
    g_devices[g_deviceCount].autoEveryMin = mins;
    g_devices[g_deviceCount].lastAutoWolMs = 0;
    g_deviceCount++;
    start = end + 1;
  }
}

void saveDeviceName(const String& name) {
  prefs.begin("general", false);
  prefs.putString("name", name);
  prefs.end();
  g_device_name = name;
  WiFi.setHostname(name.c_str());
}

void saveSTA(const String& ssid, const String& pass) {
  prefs.begin("sta", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  g_sta_ssid = ssid;
  g_sta_pass = pass;
}

void saveSTAIP(bool dhcp, const IPAddress& ip, const IPAddress& gw,
               const IPAddress& mask, const IPAddress& dns1, const IPAddress& dns2) {
  prefs.begin("staip", false);
  prefs.putBool("dhcp", dhcp);
  prefs.putString("ip", ip.toString());
  prefs.putString("gw", gw.toString());
  prefs.putString("mask", mask.toString());
  prefs.putString("dns1", dns1.toString());
  prefs.putString("dns2", dns2.toString());
  prefs.end();
  g_sta_dhcp = dhcp;
  g_sta_ip = ip; g_sta_gw = gw; g_sta_mask = mask;
  g_sta_dns1 = dns1; g_sta_dns2 = dns2;
}

void saveAP(bool enabled, const String& ssid, const String& pass) {
  prefs.begin("ap", false);
  prefs.putBool("enabled", enabled);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  g_ap_enabled = enabled;
  g_ap_ssid = ssid;
  g_ap_pass = pass;
}

void saveAPNet(const IPAddress& ip, const IPAddress& gw, const IPAddress& mask) {
  prefs.begin("apip", false);
  prefs.putString("ip", ip.toString());
  prefs.putString("gw", gw.toString());
  prefs.putString("mask", mask.toString());
  prefs.end();
  g_ap_ip = ip; g_ap_gw = gw; g_ap_mask = mask;
}

void saveAPDhcpRange(const IPAddress& start, const IPAddress& end) {
  prefs.begin("apdhcp", false);
  prefs.putString("start", start.toString());
  prefs.putString("end", end.toString());
  prefs.end();
  g_ap_dhcp_start = start;
  g_ap_dhcp_end = end;
}

void saveWOLPort(uint16_t port) {
  prefs.begin("wol", false);
  prefs.putUShort("port", port);
  prefs.end();
  g_wol_port = port;
}

void saveTelegram(bool enabled, const String& token, const String& chatid) {
  prefs.begin("telegram", false);
  prefs.putBool("enabled", enabled);
  prefs.putString("token", token);
  prefs.putString("chatid", chatid);
  prefs.end();

  g_tg_enabled = enabled;
  g_tg_token = token;
  g_tg_chatid = chatid;
}

String serializeDevices() {
  String s;
  for (int i = 0; i < g_deviceCount; i++) {
    String name = g_devices[i].name;
    name.replace("|", " ");
    name.replace("\n", " ");
    s += name + "|" + g_devices[i].mac + "|" + g_devices[i].ip + "|" +
         g_devices[i].subnetMask + "|" +
         (g_devices[i].autoWol ? "1" : "0") + "|" +
         String(g_devices[i].autoEveryMin) + "\n";
  }
  return s;
}

void saveDevices() {
  prefs.begin("devices", false);
  prefs.putString("list", serializeDevices());
  prefs.end();
}

static void clearNamespace(const char* ns) {
  Preferences p;
  p.begin(ns, false);
  p.clear();
  p.end();
}

void clearAll() {
  // Limpia namespaces usados
  clearNamespace("general");
  clearNamespace("sta");
  clearNamespace("staip");
  clearNamespace("ap");
  clearNamespace("apip");
  clearNamespace("apdhcp");
  clearNamespace("wol");
  clearNamespace("devices");
  clearNamespace("telegram");
  clearNamespace("webauth");

  // Resetea variables
  g_sta_ssid = ""; g_sta_pass = "";
  g_sta_dhcp = true;
  g_sta_ip = IPAddress(0,0,0,0);
  g_sta_gw = IPAddress(0,0,0,0);
  g_sta_mask = IPAddress(0,0,0,0);
  g_sta_dns1 = IPAddress(0,0,0,0);
  g_sta_dns2 = IPAddress(0,0,0,0);

  g_ap_enabled = false;
  g_ap_ssid = DEFAULT_AP_SSID;
  g_ap_pass = DEFAULT_AP_PASS;
  g_ap_ip = IPAddress(192,168,4,1);
  g_ap_gw = IPAddress(192,168,4,1);
  g_ap_mask = IPAddress(255,255,255,0);
  g_ap_dhcp_start = IPAddress(192,168,4,100);
  g_ap_dhcp_end = IPAddress(192,168,4,200);

  g_wol_port = 9;
  g_deviceCount = 0;
  g_device_name = "ESP32-Panel";

  g_tg_enabled = false;
  g_tg_token = "";
  g_tg_chatid = "";

  // Web auth default
  g_web_user = "admin";
  g_web_pass = "admin123";
}

// --------------------------- WiFi + AP ---------------------------
bool connectSTA(const String& ssid, const String& pass) {
  if (!ssid.length()) return false;
  WiFi.mode(g_ap_enabled ? WIFI_AP_STA : WIFI_STA);
  WiFi.disconnect(true);
  delay(200);

  if (g_sta_dhcp) {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  } else {
    if (!sameSubnet(g_sta_ip, g_sta_gw, g_sta_mask)) return false;
    if ((uint32_t)g_sta_dns2 != 0) {
      WiFi.config(g_sta_ip, g_sta_gw, g_sta_mask, g_sta_dns1, g_sta_dns2);
    } else if ((uint32_t)g_sta_dns1 != 0) {
      WiFi.config(g_sta_ip, g_sta_gw, g_sta_mask, g_sta_dns1);
    } else {
      WiFi.config(g_sta_ip, g_sta_gw, g_sta_mask);
    }
  }

  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool applyAPConfigAndDhcp() {
  if (!WiFi.softAPConfig(g_ap_ip, g_ap_gw, g_ap_mask)) return false;
  esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap_netif) return false;

  esp_netif_dhcps_stop(ap_netif);
  dhcps_lease_t lease;
  lease.enable = true;
  lease.start_ip.addr = static_cast<uint32_t>(g_ap_dhcp_start);
  lease.end_ip.addr = static_cast<uint32_t>(g_ap_dhcp_end);
  esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease));
  esp_netif_dhcps_start(ap_netif);
  return true;
}

bool startAP() {
  if (g_ap_ssid.length() < 1 || g_ap_pass.length() < 8) return false;
  WiFi.mode(WiFi.status() == WL_CONNECTED ? WIFI_AP_STA : WIFI_AP);
  if (!applyAPConfigAndDhcp()) return false;
  return WiFi.softAP(g_ap_ssid.c_str(), g_ap_pass.c_str());
}

void stopAP() {
  WiFi.softAPdisconnect(true);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_OFF);
  } else {
    WiFi.mode(WIFI_STA);
  }
}

// --------------------------- Ping & WOL ---------------------------
bool isHostOnline(const String& ipStr) {
  if (!ipStr.length()) return false;
  IPAddress ip;
  if (!ip.fromString(ipStr)) return false;
  return Ping.ping(ip, 1);
}

void sendWOL(const String& macIn, const String& ipStr = "", const String& maskStr = "") {
  String mac = normalizeMac(macIn);

  unsigned int tmp[6];
  int n = sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x",
                 &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4], &tmp[5]);
  if (n != 6) return;

  uint8_t macBytes[6];
  for (int i = 0; i < 6; i++) macBytes[i] = (uint8_t) tmp[i];

  uint8_t packet[102];
  memset(packet, 0xFF, 6);
  for (int i = 0; i < 16; i++) {
    memcpy(packet + 6 + i * 6, macBytes, 6);
  }

  WiFiUDP udp;
  udp.begin(0);

  IPAddress targetIP(255, 255, 255, 255);
  if (ipStr.length() > 0 && maskStr.length() > 0) {
    IPAddress ip, mask;
    if (ip.fromString(ipStr) && mask.fromString(maskStr)) {
      targetIP = (ip & mask) | ~mask;
    }
  }

  udp.beginPacket(targetIP, g_wol_port);
  udp.write(packet, sizeof(packet));
  udp.endPacket();
  udp.stop();

  // Notificación opcional
  if (g_tg_enabled) telegramNotify("⚡ WOL enviado a " + mac);
}

// --------------------------- UI ---------------------------
String layoutTop(const String& active) {
  String ipSTA = (WiFi.status() == WL_CONNECTED) ? ipToString(WiFi.localIP()) : "-";
  String ipAP = g_ap_enabled ? ipToString(WiFi.softAPIP()) : "-";
  String h;
  h += "<!doctype html><html><head><meta charset='utf-8'>"
       "<meta name='viewport' content='width=device-width, initial-scale=1'>"
       "<title>" + htmlEscape(g_device_name) + "</title>"
       "<style>"
       "body{margin:0;font-family:Arial,Helvetica,sans-serif;background:#fff;color:#111;}"
       ".wrap{display:flex;min-height:100vh;}"
       ".side{width:240px;border-right:1px solid #eee;padding:14px;box-sizing:border-box;background:#f9f9f9;}"
       ".brand{font-weight:800;margin-bottom:10px;font-size:16px;}"
       ".meta{font-size:12px;color:#444;line-height:1.5;margin-bottom:12px;}"
       ".nav a{display:block;padding:10px;border-radius:10px;color:#111;text-decoration:none;font-weight:700;margin:6px 0;}"
       ".nav a.active{background:#111;color:#fff;}"
       ".main{flex:1;padding:18px;box-sizing:border-box;max-width:1100px;}"
       ".card{border:1px solid #ddd;border-radius:12px;padding:14px;margin:12px 0;background:#fff;}"
       "label{display:block;margin:10px 0 6px 0;font-weight:700;}"
       "input,select{width:100%;padding:10px;border:1px solid #ccc;border-radius:10px;box-sizing:border-box;}"
       "button{padding:10px 14px;border:0;border-radius:10px;cursor:pointer;font-weight:800;}"
       ".btn{background:#111;color:#fff;}"
       ".btn2{background:#eee;color:#111;}"
       ".danger{background:#b00020;color:#fff;}"
       ".row{display:flex;gap:10px;flex-wrap:wrap;}"
       ".row>div{flex:1;min-width:220px;}"
       "table{width:100%;border-collapse:collapse;margin-top:10px;}"
       "th,td{border-bottom:1px solid #eee;padding:10px;text-align:left;vertical-align:top;}"
       "th{background:#fafafa;font-size:13px;color:#333;}"
       ".hint{font-size:12px;color:#444;margin-top:8px;}"
       ".pill{display:inline-block;padding:4px 10px;border-radius:999px;font-weight:800;font-size:12px;}"
       ".on{background:#eaffea;color:#0b6b0b;border:1px solid #b7e8b7;}"
       ".off{background:#fff0f0;color:#8a1010;border:1px solid #f0bcbc;}"
       "</style></head><body>";
  h += "<div class='wrap'><div class='side'>";
  h += "<div class='brand'>" + htmlEscape(g_device_name) + "</div>";
  h += "<div class='meta'>STA: <b>" + staState() + "</b><br>IP STA: <b>" + ipSTA + "</b><br>"
       "AP: <b>" + String(g_ap_enabled ? "On" : "Off") + "</b><br>IP AP: <b>" + ipAP + "</b></div>";
  h += "<div class='nav'>";
  h += (active == "wifi" ? "<a class='active' href='/wifi'>Wi-Fi (STA)</a>" : "<a href='/wifi'>Wi-Fi (STA)</a>");
  h += (active == "ap" ? "<a class='active' href='/ap'>Access Point</a>" : "<a href='/ap'>Access Point</a>");
  h += (active == "devices" ? "<a class='active' href='/devices'>Equipos WOL</a>" : "<a href='/devices'>Equipos WOL</a>");
  h += (active == "config" ? "<a class='active' href='/config'>Configuración</a>" : "<a href='/config'>Configuración</a>");
  h += (active == "status" ? "<a class='active' href='/status'>Estado</a>" : "<a href='/status'>Estado</a>");
  h += (active == "maint" ? "<a class='active' href='/maint'>Mantenimiento</a>" : "<a href='/maint'>Mantenimiento</a>");
  h += "</div></div><div class='main'>";
  return h;
}

String layoutBottom() {
  return "</div></div></body></html>";
}

String pageWifi() {
  String h = layoutTop("wifi");
  h += "<h2>Wi-Fi (STA)</h2>";
  h += "<div class='card'><form method='POST' action='/wifi_save'>";
  h += "<h3>Credenciales</h3>";
  h += "<div class='row'><div><label>SSID</label><input name='ssid' value='" + htmlEscape(g_sta_ssid) + "'></div>";
  h += "<div><label>Password</label><input name='pass' type='password' placeholder='(vacío = no cambiar)'></div></div>";
  h += "<h3 style='margin-top:14px'>IP del cliente</h3>";
  h += "<div class='row'><div><label>Modo</label><select name='mode'>"
       "<option value='dhcp'" + String(g_sta_dhcp ? " selected" : "") + ">DHCP</option>"
       "<option value='static'" + String(!g_sta_dhcp ? " selected" : "") + ">IP Estática</option>"
       "</select></div></div>";
  h += "<div class='row'>"
       "<div><label>IP</label><input name='ip' value='" + htmlEscape(g_sta_ip.toString()) + "'></div>"
       "<div><label>Gateway</label><input name='gw' value='" + htmlEscape(g_sta_gw.toString()) + "'></div>"
       "</div>";
  h += "<div class='row'>"
       "<div><label>Máscara</label><input name='mask' value='" + htmlEscape(g_sta_mask.toString()) + "'></div>"
       "<div><label>DNS1 (opcional)</label><input name='dns1' value='" + htmlEscape(g_sta_dns1.toString()) + "'></div>"
       "</div>";
  h += "<div class='row'>"
       "<div><label>DNS2 (opcional)</label><input name='dns2' value='" + htmlEscape(g_sta_dns2.toString()) + "'></div>"
       "<div style='display:flex;align-items:end'><button class='btn' type='submit'>Guardar y conectar</button></div>"
       "</div>";
  h += "<div class='hint'>Si eliges IP estática: IP/GW/Máscara deben estar en la misma subred.</div>";
  h += "</form></div>";
  h += layoutBottom();
  return h;
}

String pageAP() {
  String h = layoutTop("ap");
  h += "<h2>Access Point (AP)</h2>";
  h += "<div class='card'><form method='POST' action='/ap_save'>";
  h += "<h3>Credenciales AP</h3>";
  h += "<div class='row'><div><label>SSID AP</label><input name='ssid' value='" + htmlEscape(g_ap_ssid) + "'></div>";
  h += "<div><label>Password AP (mín 8)</label><input name='pass' type='password' placeholder='(vacío = no cambiar)'></div></div>";
  h += "<div style='margin-top:12px' class='row'><div><button class='btn' type='submit'>Guardar credenciales</button></div></div>";
  h += "<div class='hint'>Cambios en SSID/clave requieren reiniciar AP.</div></form></div>";

  h += "<div class='card'><form method='POST' action='/ap_net_save'>";
  h += "<h3>Red del AP</h3>";
  h += "<div class='row'><div><label>IP AP</label><input name='ip' value='" + htmlEscape(g_ap_ip.toString()) + "'></div>";
  h += "<div><label>Gateway</label><input name='gw' value='" + htmlEscape(g_ap_gw.toString()) + "'></div></div>";
  h += "<div class='row'><div><label>Máscara</label><input name='mask' value='" + htmlEscape(g_ap_mask.toString()) + "'></div>";
  h += "<div style='display:flex;align-items:end'><button class='btn' type='submit'>Guardar red</button></div></div>";
  h += "<div class='hint'>Requiere reiniciar AP para aplicar completamente.</div></form></div>";

  h += "<div class='card'><form method='POST' action='/ap_dhcp_save'>";
  h += "<h3>Rango DHCP (AP)</h3>";
  h += "<div class='row'>"
       "<div><label>Inicio</label><input name='start' value='" + htmlEscape(g_ap_dhcp_start.toString()) + "'></div>"
       "<div><label>Fin</label><input name='end' value='" + htmlEscape(g_ap_dhcp_end.toString()) + "'></div>"
       "</div>";
  h += "<div class='row'><div style='display:flex;align-items:end'><button class='btn' type='submit'>Guardar DHCP</button></div></div>";
  h += "<div class='hint'>Inicio/Fin deben estar en la misma subred del AP y Start &lt;= End.</div></form></div>";

  h += "<div class='card'><div class='row'>";
  if (g_ap_enabled) {
    h += "<div><button class='btn2' type='button' onclick=\"location.href='/ap_off'\">Desactivar AP</button></div>";
    h += "<div><button class='btn' type='button' onclick=\"location.href='/ap_restart'\">Reiniciar AP</button></div>";
  } else {
    h += "<div><button class='btn' type='button' onclick=\"location.href='/ap_on'\">Activar AP</button></div>";
  }
  h += "</div><div class='hint'>Modo AP+STA: puedes usar ambos al mismo tiempo.</div></div>";
  h += layoutBottom();
  return h;
}

String pageDevices() {
  String h = layoutTop("devices");
  h += "<h2>Equipos WOL</h2>";
  h += "<div class='card'><form method='POST' action='/wol_port_save'>";
  h += "<h3>Puerto WOL (UDP)</h3>";
  h += "<div class='row'>"
       "<div><label>Puerto</label><input name='port' value='" + String(g_wol_port) + "'></div>"
       "<div style='display:flex;align-items:end'><button class='btn' type='submit'>Guardar</button></div>"
       "</div>";
  h += "<div class='hint'>Ejemplo típico: 7 o 9.</div>";
  h += "</form></div>";

  h += "<div class='card'><form method='POST' action='/dev_add'>";
  h += "<h3>Agregar equipo</h3>";
  h += "<div class='row'>"
       "<div><label>Nombre</label><input name='name' placeholder='PC-Oficina'></div>"
       "<div><label>MAC</label><input name='mac' placeholder='AA:BB:CC:DD:EE:FF'></div>"
       "</div>";
  h += "<div class='row'>"
       "<div><label>IP (para Ping)</label><input name='ip' placeholder='192.168.1.50'></div>"
       "<div><label>Máscara subred (opcional)</label><input name='mask' placeholder='255.255.255.0'></div>"
       "</div>";
  h += "<div class='row'>"
       "<div><label>Auto-encendido</label><select name='auto'><option value='0' selected>Off</option><option value='1'>On</option></select></div>"
       "<div><label>Intervalo (min)</label><input name='mins' value='5'></div>"
       "</div>";
  h += "<div class='row'><div style='display:flex;align-items:end'><button class='btn' type='submit'>Agregar</button></div></div>";
  h += "<div class='hint'>Máscara opcional → WOL dirigido (más eficiente). Vacío = broadcast global.</div>";
  h += "</form></div>";

  h += "<div class='card'><h3>Lista</h3>";
  if (g_deviceCount == 0) {
    h += "<div class='hint'>No hay equipos guardados.</div>";
  } else {
    h += "<table><tr><th>#</th><th>Nombre</th><th>MAC</th><th>IP</th><th>Máscara</th><th>Estado</th><th>Auto</th><th>Acciones</th></tr>";
    for (int i = 0; i < g_deviceCount; i++) {
      String st = g_devices[i].online ? "<span class='pill on'>Online</span>" : "<span class='pill off'>Offline</span>";
      String au = g_devices[i].autoWol ? ("On / " + String(g_devices[i].autoEveryMin) + " min") : "Off";
      String maskDisplay = g_devices[i].subnetMask.length() ? htmlEscape(g_devices[i].subnetMask) : "-";
      h += "<tr><td>" + String(i+1) + "</td><td>" + htmlEscape(g_devices[i].name) + "</td><td>" + htmlEscape(g_devices[i].mac) + "</td>"
           "<td>" + htmlEscape(g_devices[i].ip) + "</td><td>" + maskDisplay + "</td><td>" + st + "</td><td>" + htmlEscape(au) + "</td>"
           "<td><button class='btn2' onclick=\"location.href='/dev_wol?id=" + String(i) + "'\">WOL</button> "
           "<button class='btn2' onclick=\"location.href='/dev_ping?id=" + String(i) + "'\">Ping</button> "
           "<button class='btn2' onclick=\"location.href='/dev_edit?id=" + String(i) + "'\">Editar</button> "
           "<button class='danger' onclick=\"location.href='/dev_del?id=" + String(i) + "'\">Borrar</button></td></tr>";
    }
    h += "</table>";
  }
  h += "</div>";
  h += layoutBottom();
  return h;
}

String pageEditDevice(int id) {
  if (id < 0 || id >= g_deviceCount) return layoutTop("devices") + "<h2>Editar</h2><div class='card'>ID inválido.</div>" + layoutBottom();
  Device &d = g_devices[id];
  String h = layoutTop("devices");
  h += "<h2>Editar equipo</h2>";
  h += "<div class='card'><form method='POST' action='/dev_edit_save'>";
  h += "<input type='hidden' name='id' value='" + String(id) + "'>";
  h += "<div class='row'><div><label>Nombre</label><input name='name' value='" + htmlEscape(d.name) + "'></div>"
       "<div><label>MAC</label><input name='mac' value='" + htmlEscape(d.mac) + "'></div></div>";
  h += "<div class='row'><div><label>IP (Ping)</label><input name='ip' value='" + htmlEscape(d.ip) + "'></div>"
       "<div><label>Máscara subred (opcional)</label><input name='mask' value='" + htmlEscape(d.subnetMask) + "' placeholder='255.255.255.0'></div></div>";
  h += "<div class='row'><div><label>Auto-encendido</label><select name='auto'>"
       "<option value='0'" + String(d.autoWol ? "" : " selected") + ">Off</option>"
       "<option value='1'" + String(d.autoWol ? " selected" : "") + ">On</option></select></div>"
       "<div><label>Intervalo (min)</label><input name='mins' value='" + String(d.autoEveryMin) + "'></div></div>";
  h += "<div class='row'><div style='display:flex;align-items:end'><button class='btn' type='submit'>Guardar</button></div></div>";
  h += "<div class='hint'>Máscara opcional para WOL dirigido. Vacío = broadcast global.</div>";
  h += "</form></div>";
  h += layoutBottom();
  return h;
}

String pageStatus() {
  String h = layoutTop("status");
  h += "<h2>Estado</h2>";
  h += "<div class='card'><h3>Interfaces</h3>";
  h += "<div class='hint'>STA: " + staState() + "</div>";
  if (WiFi.status() == WL_CONNECTED) h += "<div class='hint'>IP STA: <b>" + WiFi.localIP().toString() + "</b></div>";
  h += "<div class='hint'>AP: " + String(g_ap_enabled ? "On" : "Off") + "</div>";
  if (g_ap_enabled) {
    h += "<div class='hint'>IP AP: <b>" + WiFi.softAPIP().toString() + "</b></div>";
    h += "<div class='hint'>DHCP: <b>" + g_ap_dhcp_start.toString() + " - " + g_ap_dhcp_end.toString() + "</b></div>";
  }
  h += "</div>";

  h += "<div class='card'><h3>Equipos</h3>";
  if (g_deviceCount == 0) {
    h += "<div class='hint'>No hay equipos guardados.</div>";
  } else {
    h += "<table><tr><th>Nombre</th><th>IP</th><th>Estado</th><th>Auto</th></tr>";
    for (int i = 0; i < g_deviceCount; i++) {
      String st = g_devices[i].online ? "<span class='pill on'>Online</span>" : "<span class='pill off'>Offline</span>";
      String au = g_devices[i].autoWol ? ("On / " + String(g_devices[i].autoEveryMin) + " min") : "Off";
      h += "<tr><td>" + htmlEscape(g_devices[i].name) + "</td><td>" + htmlEscape(g_devices[i].ip) + "</td><td>" + st + "</td><td>" + au + "</td></tr>";
    }
    h += "</table>";
    h += "<div class='hint'>El estado se actualiza con ping ICMP. Si la red bloquea ICMP, puede mostrar Offline aunque esté encendido.</div>";
  }
  h += "</div>";
  h += layoutBottom();
  return h;
}

String pageConfig() {
  String h = layoutTop("config");
  h += "<h2>Configuración del dispositivo</h2>";

  // Nombre
  h += "<div class='card'><form method='POST' action='/config_save'>";
  h += "<label>Nombre del dispositivo</label>";
  h += "<input name='name' value='" + htmlEscape(g_device_name) + "' placeholder='Ej: Mi-WOL-Server'>";
  h += "<div class='hint'>Se usa como hostname y título. Máx 31 caracteres.</div>";
  h += "<div style='margin-top:20px;display:flex;align-items:end'><button class='btn' type='submit'>Guardar nombre</button></div>";
  h += "</form></div>";

  // Credenciales Web
  h += "<div class='card'><form method='POST' action='/auth_save'>";
  h += "<h3>Acceso Web</h3>";
  h += "<label>Usuario</label>";
  h += "<input name='user' value='" + htmlEscape(g_web_user) + "'>";
  h += "<label>Nueva contraseña</label>";
  h += "<input name='pass' type='password' placeholder='(vacío = no cambiar)'>";
  h += "<div class='hint'>La contraseña no se muestra. Cambiar credenciales forzará reautenticación en el navegador.</div>";
  h += "<div style='margin-top:14px'><button class='btn' type='submit'>Guardar credenciales</button></div>";
  h += "</form></div>";

  // Telegram
  h += "<div class='card'><form method='POST' action='/telegram_save'>";
  h += "<h3>Notificaciones Telegram</h3>";

  h += "<label>Habilitar</label>";
  h += "<select name='enabled'>"
       "<option value='0'" + String(!g_tg_enabled ? " selected" : "") + ">No</option>"
       "<option value='1'" + String(g_tg_enabled ? " selected" : "") + ">Sí</option>"
       "</select>";

  h += "<label>Bot Token</label>";
  h += "<input name='token' value='" + htmlEscape(g_tg_token) + "' placeholder='123456:ABC...'>";

  h += "<label>Chat ID</label>";
  h += "<input name='chatid' value='" + htmlEscape(g_tg_chatid) + "' placeholder='-1001234567890'>";

  h += "<div style='margin-top:14px' class='row'>"
       "<div><button class='btn' type='submit'>Guardar Telegram</button></div>"
       "<div><button class='btn2' type='button' onclick=\"location.href='/telegram_test'\">Probar Telegram</button></div>"
       "</div>";

  h += "<div class='hint'>La prueba requiere que el STA esté conectado a Internet.</div>";
  h += "</form></div>";

  h += layoutBottom();
  return h;
}

String pageMaint() {
  String h = layoutTop("maint");
  h += "<h2>Mantenimiento</h2>";
  h += "<div class='card'><h3>Acciones</h3>";
  h += "<div class='row'>"
       "<div><button class='danger' type='button' onclick=\"if(confirm('¿Borrar toda la configuración?')) location.href='/clear_all'\">Borrar TODO</button></div>"
       "<div><button class='btn2' type='button' onclick=\"if(confirm('¿Reiniciar ESP32?')) location.href='/reboot'\">Reiniciar ESP32</button></div>"
       "</div>";
  h += "<div class='hint'>Borrar TODO elimina Wi-Fi, AP, dispositivos, Telegram, credenciales web, etc.</div></div>";
  h += layoutBottom();
  return h;
}

void updateDevices() {
  unsigned long now = millis();
  for (int i = 0; i < g_deviceCount; i++) {
    Device &d = g_devices[i];

    if (d.ip.length() && (now - d.lastCheckMs) >= PING_CHECK_EVERY_MS) {
      d.lastCheckMs = now;
      bool prev = d.online;
      d.online = isHostOnline(d.ip);

      // Notifica solo si hay cambio de estado
      if (g_tg_enabled && prev != d.online) {
        telegramNotify("📡 " + d.name + " ahora está " + (d.online ? "ONLINE" : "OFFLINE"));
      }
    }

    if (d.autoWol && !d.online && d.ip.length()) {
      unsigned long intervalMs = (unsigned long)d.autoEveryMin * 60000UL;
      if (intervalMs < 60000UL) intervalMs = 60000UL;
      if ((now - d.lastAutoWolMs) >= intervalMs && (now - d.lastAutoWolMs) >= AUTO_WOL_MIN_GAP_MS) {
        d.lastAutoWolMs = now;
        sendWOL(d.mac, d.ip, d.subnetMask);
      }
    }
  }
}

void setupRoutes() {
  server.on("/", HTTP_GET, [](){ if(!authOrRequest()) return; server.sendHeader("Location","/status"); server.send(303); });

  server.on("/status", HTTP_GET, [](){ if(!authOrRequest()) return; server.send(200, "text/html", pageStatus()); });
  server.on("/wifi", HTTP_GET, [](){ if(!authOrRequest()) return; server.send(200, "text/html", pageWifi()); });
  server.on("/ap", HTTP_GET, [](){ if(!authOrRequest()) return; server.send(200, "text/html", pageAP()); });
  server.on("/devices", HTTP_GET, [](){ if(!authOrRequest()) return; server.send(200, "text/html", pageDevices()); });
  server.on("/config", HTTP_GET, [](){ if(!authOrRequest()) return; server.send(200, "text/html", pageConfig()); });
  server.on("/maint", HTTP_GET, [](){ if(!authOrRequest()) return; server.send(200, "text/html", pageMaint()); });

  server.on("/wifi_save", HTTP_POST, [](){
    if(!authOrRequest()) return;
    String ssid = server.arg("ssid"); ssid.trim();
    String pass = server.arg("pass");
    String mode = server.arg("mode");
    bool useDhcp = (mode != "static");
    IPAddress ip(0,0,0,0), gw(0,0,0,0), mask(0,0,0,0), dns1(0,0,0,0), dns2(0,0,0,0);
    if (!useDhcp) {
      if (!parseIP(server.arg("ip"), ip) || !parseIP(server.arg("gw"), gw) || !parseIP(server.arg("mask"), mask)) {
        server.send(400,"text/plain","IP/GW/Máscara inválidos."); return;
      }
      if (!sameSubnet(ip, gw, mask)) { server.send(400,"text/plain","IP y Gateway no en misma subred."); return; }
      IPAddress tmp;
      if (server.arg("dns1").length() && parseIP(server.arg("dns1"), tmp)) dns1 = tmp;
      if (server.arg("dns2").length() && parseIP(server.arg("dns2"), tmp)) dns2 = tmp;
    }
    if (pass.length()) saveSTA(ssid, pass); else saveSTA(ssid, g_sta_pass);
    saveSTAIP(useDhcp, ip, gw, mask, dns1, dns2);
    connectSTA(g_sta_ssid, g_sta_pass);
    server.sendHeader("Location","/status"); server.send(303);
  });

  server.on("/ap_save", HTTP_POST, [](){
    if(!authOrRequest()) return;
    String ssid = server.arg("ssid"); ssid.trim();
    String pass = server.arg("pass");
    if (ssid.length() < 1) { server.send(400,"text/plain","SSID inválido."); return; }
    if (pass.length() && pass.length() < 8) { server.send(400,"text/plain","Password AP debe tener mínimo 8."); return; }
    if (pass.length()) saveAP(g_ap_enabled, ssid, pass); else saveAP(g_ap_enabled, ssid, g_ap_pass);
    server.sendHeader("Location","/ap"); server.send(303);
  });

  server.on("/ap_net_save", HTTP_POST, [](){
    if(!authOrRequest()) return;
    IPAddress ip, gw, mask;
    if (!parseIP(server.arg("ip"), ip) || !parseIP(server.arg("gw"), gw) || !parseIP(server.arg("mask"), mask)) {
      server.send(400,"text/plain","IP/GW/Máscara inválidos."); return;
    }
    if (!sameSubnet(ip, gw, mask)) { server.send(400,"text/plain","IP y Gateway no en misma subred."); return; }
    saveAPNet(ip, gw, mask);
    server.sendHeader("Location","/ap"); server.send(303);
  });

  server.on("/ap_dhcp_save", HTTP_POST, [](){
    if(!authOrRequest()) return;
    IPAddress s, e;
    if (!parseIP(server.arg("start"), s) || !parseIP(server.arg("end"), e)) {
      server.send(400,"text/plain","Start/End inválidos."); return;
    }
    if (!sameSubnet(s, g_ap_ip, g_ap_mask) || !sameSubnet(e, g_ap_ip, g_ap_mask)) {
      server.send(400,"text/plain","Start/End deben estar en la misma subred."); return;
    }
    if ((uint32_t)s > (uint32_t)e) { server.send(400,"text/plain","Start debe ser <= End."); return; }
    saveAPDhcpRange(s, e);
    server.sendHeader("Location","/ap"); server.send(303);
  });

  server.on("/ap_on", HTTP_GET, [](){ if(!authOrRequest()) return; g_ap_enabled = true; saveAP(true, g_ap_ssid, g_ap_pass); startAP(); server.sendHeader("Location","/ap"); server.send(303); });
  server.on("/ap_off", HTTP_GET, [](){ if(!authOrRequest()) return; g_ap_enabled = false; saveAP(false, g_ap_ssid, g_ap_pass); stopAP(); server.sendHeader("Location","/ap"); server.send(303); });
  server.on("/ap_restart", HTTP_GET, [](){ if(!authOrRequest()) return; stopAP(); delay(250); if (g_ap_enabled) startAP(); server.sendHeader("Location","/ap"); server.send(303); });

  server.on("/wol_port_save", HTTP_POST, [](){
    if(!authOrRequest()) return;
    int p = server.arg("port").toInt();
    if (p < 1 || p > 65535) { server.send(400,"text/plain","Puerto inválido."); return; }
    saveWOLPort((uint16_t)p);
    server.sendHeader("Location","/devices"); server.send(303);
  });

  server.on("/dev_add", HTTP_POST, [](){
    if(!authOrRequest()) return;
    if (g_deviceCount >= MAX_DEVICES) { server.send(400,"text/plain","Máximo alcanzado."); return; }
    String name = server.arg("name"); name.trim();
    String mac = normalizeMac(server.arg("mac"));
    String ip = server.arg("ip"); ip.trim();
    String mask = server.arg("mask"); mask.trim();
    if (!name.length() || !isValidMac(mac)) { server.send(400,"text/plain","Nombre o MAC inválidos."); return; }
    if (ip.length()) { IPAddress tmp; if (!tmp.fromString(ip)) { server.send(400,"text/plain","IP inválida."); return; } }
    if (mask.length()) { IPAddress tmp; if (!tmp.fromString(mask)) mask = ""; }
    bool aw = (server.arg("auto") == "1");
    int mins = server.arg("mins").toInt(); if (mins < 1) mins = 1; if (mins > 1440) mins = 1440;
    Device &d = g_devices[g_deviceCount++];
    d.name = name; d.mac = mac; d.ip = ip; d.subnetMask = mask; d.autoWol = aw; d.autoEveryMin = mins;
    saveDevices();
    server.sendHeader("Location","/devices"); server.send(303);
  });

  server.on("/dev_del", HTTP_GET, [](){
    if(!authOrRequest()) return;
    int id = server.arg("id").toInt();
    if (id < 0 || id >= g_deviceCount) { server.send(400,"text/plain","ID inválido."); return; }
    for (int i = id; i < g_deviceCount - 1; i++) g_devices[i] = g_devices[i + 1];
    g_deviceCount--; saveDevices();
    server.sendHeader("Location","/devices"); server.send(303);
  });

  server.on("/dev_wol", HTTP_GET, [](){
    if(!authOrRequest()) return;
    int id = server.arg("id").toInt();
    if (id < 0 || id >= g_deviceCount) return;
    sendWOL(g_devices[id].mac, g_devices[id].ip, g_devices[id].subnetMask);
    server.sendHeader("Location","/devices"); server.send(303);
  });

  server.on("/dev_ping", HTTP_GET, [](){
    if(!authOrRequest()) return;
    int id = server.arg("id").toInt();
    if (id < 0 || id >= g_deviceCount) return;
    g_devices[id].online = isHostOnline(g_devices[id].ip);
    g_devices[id].lastCheckMs = millis();
    server.sendHeader("Location","/devices"); server.send(303);
  });

  server.on("/dev_edit", HTTP_GET, [](){
    if(!authOrRequest()) return;
    int id = server.arg("id").toInt();
    server.send(200, "text/html", pageEditDevice(id));
  });

  server.on("/dev_edit_save", HTTP_POST, [](){
    if(!authOrRequest()) return;
    int id = server.arg("id").toInt();
    if (id < 0 || id >= g_deviceCount) { server.send(400,"text/plain","ID inválido."); return; }
    String name = server.arg("name"); name.trim();
    String mac = normalizeMac(server.arg("mac"));
    String ip = server.arg("ip"); ip.trim();
    String mask = server.arg("mask"); mask.trim();
    bool aw = (server.arg("auto") == "1");
    int mins = server.arg("mins").toInt(); if (mins < 1) mins = 1; if (mins > 1440) mins = 1440;
    if (!name.length() || !isValidMac(mac)) { server.send(400,"text/plain","Nombre o MAC inválidos."); return; }
    if (ip.length()) { IPAddress tmp; if (!tmp.fromString(ip)) { server.send(400,"text/plain","IP inválida."); return; } }
    if (mask.length()) { IPAddress tmp; if (!tmp.fromString(mask)) mask = ""; }
    Device &d = g_devices[id];
    d.name = name; d.mac = mac; d.ip = ip; d.subnetMask = mask; d.autoWol = aw; d.autoEveryMin = mins;
    saveDevices();
    server.sendHeader("Location","/devices"); server.send(303);
  });

  server.on("/config_save", HTTP_POST, [](){
    if(!authOrRequest()) return;
    String name = server.arg("name"); name.trim();
    if (name.length() == 0) name = "ESP32-Panel";
    if (name.length() > 31) name = name.substring(0, 31);
    saveDeviceName(name);
    server.sendHeader("Location","/config"); server.send(303);
  });

  // Guardar credenciales Web
  server.on("/auth_save", HTTP_POST, [](){
    if(!authOrRequest()) return;

    String user = server.arg("user"); user.trim();
    String pass = server.arg("pass");

    if (user.length() < 3) { server.send(400,"text/plain","Usuario inválido (mín 3)."); return; }

    if (pass.length()) {
      if (pass.length() < 6) { server.send(400,"text/plain","Password inválido (mín 6)."); return; }
      saveWebAuth(user, pass);
    } else {
      // si viene vacío, NO cambia password
      saveWebAuth(user, g_web_pass);
    }

    server.sendHeader("Location","/config");
    server.send(303);
  });

  // Telegram save
  server.on("/telegram_save", HTTP_POST, [](){
    if(!authOrRequest()) return;

    bool enabled = (server.arg("enabled") == "1");
    String token = server.arg("token"); token.trim();
    String chat  = server.arg("chatid"); chat.trim();

    saveTelegram(enabled, token, chat);

    if (enabled && WiFi.status() == WL_CONNECTED && token.length() && chat.length()) {
      telegramNotify("✅ Telegram guardado en " + g_device_name);
    }

    server.sendHeader("Location","/config");
    server.send(303);
  });

  // Telegram test
  server.on("/telegram_test", HTTP_GET, [](){
    if(!authOrRequest()) return;

    if (!g_tg_enabled) { server.send(400, "text/plain", "Telegram está deshabilitado."); return; }
    if (WiFi.status() != WL_CONNECTED) { server.send(400, "text/plain", "STA no está conectado a Internet."); return; }
    if (!g_tg_token.length() || !g_tg_chatid.length()) { server.send(400, "text/plain", "Falta Token o Chat ID."); return; }

    bool ok = telegramNotifyResult("✅ Prueba OK desde " + g_device_name);
    if (!ok) { server.send(500, "text/plain", "No se pudo enviar el mensaje a Telegram. Verifica Token/ChatID/Internet."); return; }

    server.send(200, "text/plain", "OK: Mensaje enviado. Revisa tu Telegram.");
  });

  server.on("/clear_all", HTTP_GET, [](){ if(!authOrRequest()) return; clearAll(); server.sendHeader("Location","/status"); server.send(303); });
  server.on("/reboot", HTTP_GET, [](){ if(!authOrRequest()) return; server.send(200,"text/plain","Reiniciando..."); delay(300); ESP.restart(); });
}

void setup() {
  Serial.begin(115200);
  delay(300);

  loadConfig();
  loadWebAuth();
  WiFi.setHostname(g_device_name.c_str());

  bool staOk = false;
  if (g_sta_ssid.length()) {
    staOk = connectSTA(g_sta_ssid, g_sta_pass);
  }

  if (g_ap_enabled) startAP();

  if (!staOk && !g_ap_enabled) {
    g_ap_enabled = true;
    g_ap_ssid = DEFAULT_AP_SSID;
    g_ap_pass = DEFAULT_AP_PASS;
    startAP();
    Serial.println("Fallback AP activado");
  }

  Serial.println("STA: " + String(staOk ? "Conectado" : "No conectado"));
  if (WiFi.status() == WL_CONNECTED) Serial.println("IP STA: " + WiFi.localIP().toString());
  if (g_ap_enabled) Serial.println("IP AP: " + WiFi.softAPIP().toString());

  setupRoutes();
  server.begin();
  Serial.println("Servidor HTTP iniciado");
}

void loop() {
  server.handleClient();
  updateDevices();
}
