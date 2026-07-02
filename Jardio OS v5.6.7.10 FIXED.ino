/* Jardio OS v5.6.7.10 FIXED - Complete UI Reorganization & JavaScript Fixes */
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Audio.h>
#include "time.h"

#define SD_CS 4 
#define SD_MOSI 5 
#define SD_SCK 6 
#define SD_MISO 7
#define I2S_BCLK 17
#define I2S_LRC 16
#define I2S_DOUT 15

// ========== EVENT STRUCTURE ==========
struct ScheduleEvent {
  String id;
  int day;
  String time;
  String type;
  int target;
  int repeat;
  int volume;
  bool active;
  unsigned long spotEndTime;
  int spotRepeatCount;
  unsigned long pauseUntil;
};

// ========== FORWARD DECLARATIONS ==========
String getDashboard();
String getMediaUI();
String getPlaylistUI();
String getSchedulerUI();
String getSystemUI();
String getSetupUI();
void audioTask(void *pvParameters);
void schedulerTask(void *pvParameters);
void indexTracks();
String shortenName(String name);
String urlEncode(String str);
String urlDecode(String str);
void initPlaylists();
void initScheduler();
bool isInPlaylist(const String& filename, int playlistNum);
void loadScheduleEvents();
void saveScheduleEvents();
void processScheduleEvents();
void playPlaylist(int playlistNum);
void playSpot(String filename, int volume);
void resumeMusic();
int getSpotPrefix(String filename);
String findSpotFile(int spotNum);

// ========== GLOBAL VARIABLES ==========
Audio audio;
WebServer server(80);
File uploadFile;
SPIClass sdSPI(HSPI);

String musicListCache = "";
String spotListCache = "";
String spotTriggersCache = "";

int defaultMusicVolume = 15;
int defaultSpotVolume = 20;
int currentMusicVolume = 15;
int currentSpotVolume = 20;

bool isSpotPlaying = false;
bool isMusicPlaying = false;
int currentPlaylistNum = 0;

String nowPlayingTrack = "";
String lastPlayedFile = "";
String currentPlaylistName = "";

ScheduleEvent scheduleEvents[100];
int scheduleEventCount = 0;
int lastActivePlaylistNum = -1;

struct Config {
  String ssid = ""; String pass = "";
  bool dhcp = true;
  String ip = "192.168.1.100"; String gw = "192.168.1.1";
  String mask = "255.255.255.0"; String dns = "8.8.8.8";
  String mdns = "jardio";
  int musicVol = 15;
  int spotVol = 20;
  String ntpServer = "pool.ntp.org";
  String tz = "CET-1CEST,M3.5.0,M10.5.0/3"; 
} cfg;

bool apMode = false;
TaskHandle_t schedulerTaskHandle = NULL;

// ========== UTILITY FUNCTIONS ==========
String shortenName(String name) {
  int dot = name.lastIndexOf('.');
  if (dot > 0) name = name.substring(0, dot);
  if (name.length() > 20) name = name.substring(0,20) + "...";
  return name;
}

String urlEncode(String str) {
  String encoded = "";
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') encoded += c;
    else { encoded += '%'; encoded += String(c, HEX); }
  }
  return encoded;
}

String urlDecode(String str) {
  String decoded = "";
  for (unsigned int i = 0; i < str.length(); i++) {
    if (str.charAt(i) == '%') {
      if (i + 2 < str.length()) {
        char c = strtol(str.substring(i+1, i+3).c_str(), NULL, 16);
        decoded += c;
        i += 2;
      }
    } else if (str.charAt(i) == '+') decoded += ' ';
    else decoded += str.charAt(i);
  }
  return decoded;
}

int getSpotPrefix(String filename) {
  int underscorePos = filename.indexOf('_');
  if (underscorePos <= 0) return -1;
  String prefix = filename.substring(0, underscorePos);
  int num = prefix.toInt();
  if (num >= 1 && num <= 10) return num;
  return -1;
}

String findSpotFile(int spotNum) {
  File root = SD.open("/spot");
  if (!root) return "";
  File f = root.openNextFile();
  while(f) {
    if (!f.isDirectory()) {
      String n = f.name();
      if (getSpotPrefix(n) == spotNum) {
        String result = n;
        f.close();
        root.close();
        return result;
      }
    }
    f = root.openNextFile();
  }
  root.close();
  return "";
}

// ========== PLAYLIST FUNCTIONS ==========
void initPlaylists() {
  if (!SD.exists("/playlist")) SD.mkdir("/playlist");
  for(int i = 1; i <= 10; i++) {
    String path = "/playlist/pl" + String(i) + ".csv";
    if (!SD.exists(path)) {
      File f = SD.open(path, FILE_WRITE);
      if (f) f.close();
    }
  }
}

bool isInPlaylist(const String& filename, int playlistNum) {
  String path = "/playlist/pl" + String(playlistNum) + ".csv";
  File f = SD.open(path);
  if (!f) return false;
  while(f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line == filename) {
      f.close();
      return true;
    }
  }
  f.close();
  return false;
}

// ========== SCHEDULER FUNCTIONS ==========
void initScheduler() {
  if (!SD.exists("/scheduler")) SD.mkdir("/scheduler");
}

void loadScheduleEvents() {
  scheduleEventCount = 0;
  File f = SD.open("/scheduler/events.json");
  if (!f) {
    Serial.println("[SCHEDULER] No events file found");
    return;
  }
  
  StaticJsonDocument<16384> doc;
  if (deserializeJson(doc, f)) {
    Serial.println("[SCHEDULER] Failed to parse events.json");
    f.close();
    return;
  }
  f.close();
  
  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    if (scheduleEventCount >= 100) break;
    ScheduleEvent& evt = scheduleEvents[scheduleEventCount];
    evt.id = obj["id"].as<String>();
    evt.day = obj["day"] | 0;
    evt.time = obj["time"].as<String>();
    evt.type = obj["type"].as<String>();
    evt.target = obj["target"] | 1;
    evt.repeat = obj["repeat"] | 1;
    evt.volume = obj["volume"] | 15;
    evt.active = false;
    evt.spotRepeatCount = 0;
    evt.pauseUntil = 0;
    scheduleEventCount++;
  }
  Serial.printf("[SCHEDULER] Loaded %d events\n", scheduleEventCount);
}

void saveScheduleEvents() {
  File f = SD.open("/scheduler/events.json", FILE_WRITE);
  if (!f) {
    Serial.println("[SCHEDULER] Failed to open events.json");
    return;
  }
  StaticJsonDocument<16384> doc;
  JsonArray arr = doc.createNestedArray();
  for (int i = 0; i < scheduleEventCount; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["id"] = scheduleEvents[i].id;
    obj["day"] = scheduleEvents[i].day;
    obj["time"] = scheduleEvents[i].time;
    obj["type"] = scheduleEvents[i].type;
    obj["target"] = scheduleEvents[i].target;
    obj["repeat"] = scheduleEvents[i].repeat;
    obj["volume"] = scheduleEvents[i].volume;
  }
  serializeJson(doc, f);
  f.close();
  Serial.printf("[SCHEDULER] Saved %d events\n", scheduleEventCount);
}

bool timeMatch(String eventTime) {
  time_t now;
  struct tm ti;
  time(&now);
  localtime_r(&now, &ti);
  int currentHour = ti.tm_hour;
  int currentMin = ti.tm_min;
  int eventH = eventTime.substring(0, 2).toInt();
  int eventM = eventTime.substring(3, 5).toInt();
  return (currentHour == eventH && currentMin == eventM);
}

int getCurrentDayOfWeek() {
  time_t now;
  struct tm ti;
  time(&now);
  localtime_r(&now, &ti);
  return (ti.tm_wday + 6) % 7;
}

void playPlaylist(int playlistNum) {
  String playlistPath = "/playlist/pl" + String(playlistNum) + ".csv";
  File f = SD.open(playlistPath);
  if (!f) {
    Serial.printf("[PLAYLIST] File not found: %s\n", playlistPath.c_str());
    return;
  }
  String firstTrack = "";
  if (f.available()) {
    firstTrack = f.readStringUntil('\n');
    firstTrack.trim();
  }
  f.close();
  
  if (firstTrack.length() == 0) {
    Serial.printf("[PLAYLIST] Playlist %d is empty\n", playlistNum);
    isMusicPlaying = false;
    currentPlaylistNum = 0;
    return;
  }
  
  lastPlayedFile = "/music/" + firstTrack;
  nowPlayingTrack = firstTrack;
  currentPlaylistNum = playlistNum;
  currentPlaylistName = "PL" + String(playlistNum);
  isMusicPlaying = true;
  isSpotPlaying = false;
  audio.setVolume(currentMusicVolume);
  audio.connecttoFS(SD, lastPlayedFile.c_str());
  Serial.printf("[PLAYLIST] Started PL%d: %s (vol=%d)\n", playlistNum, firstTrack.c_str(), currentMusicVolume);
}

void playSpot(String filename, int volume) {
  Serial.printf("[AUDIO] Playing spot: %s (vol=%d)\n", filename.c_str(), volume);
  isSpotPlaying = true;
  currentSpotVolume = volume;
  audio.setVolume(currentSpotVolume);
  audio.connecttoFS(SD, ("/spot/" + filename).c_str());
}

void resumeMusic() {
  if (currentPlaylistNum > 0) {
    Serial.printf("[AUDIO] Resuming PL%d (vol=%d)\n", currentPlaylistNum, currentMusicVolume);
    isSpotPlaying = false;
    audio.setVolume(currentMusicVolume);
    if (lastPlayedFile != "") {
      audio.connecttoFS(SD, lastPlayedFile.c_str());
      isMusicPlaying = true;
    }
  } else {
    Serial.println("[AUDIO] No playlist to resume, silence");
    isSpotPlaying = false;
    isMusicPlaying = false;
    audio.stopSong();
  }
}

void processScheduleEvents() {
  int currentDay = getCurrentDayOfWeek();
  unsigned long now = millis();
  
  int activePlaylistNum = -1;
  for (int i = 0; i < scheduleEventCount; i++) {
    ScheduleEvent& evt = scheduleEvents[i];
    if (evt.type != "playlist") continue;
    if (evt.day != currentDay) continue;
    if (!timeMatch(evt.time)) continue;
    if (!evt.active) {
      evt.active = true;
      activePlaylistNum = evt.target;
      Serial.printf("[SCHEDULER] Playlist event activated: PL%d\n", evt.target);
    }
  }
  
  for (int i = 0; i < scheduleEventCount; i++) {
    ScheduleEvent& evt = scheduleEvents[i];
    if (evt.type != "playlist") continue;
    if (evt.day != currentDay) continue;
    int eventH = evt.time.substring(0, 2).toInt();
    int eventM = evt.time.substring(3, 5).toInt();
    time_t t;
    struct tm ti;
    time(&t);
    localtime_r(&t, &ti);
    if (ti.tm_hour == eventH && ti.tm_min == eventM) {
      activePlaylistNum = evt.target;
      evt.active = true;
    } else {
      evt.active = false;
    }
  }
  
  if (activePlaylistNum > 0 && activePlaylistNum != currentPlaylistNum) {
    playPlaylist(activePlaylistNum);
    lastActivePlaylistNum = activePlaylistNum;
  } else if (activePlaylistNum < 0 && isMusicPlaying) {
    audio.stopSong();
    isMusicPlaying = false;
    currentPlaylistNum = 0;
    Serial.println("[SCHEDULER] No active playlist - silence");
  }
  
  for (int i = 0; i < scheduleEventCount; i++) {
    ScheduleEvent& evt = scheduleEvents[i];
    if (evt.type != "volume") continue;
    if (evt.day != currentDay) continue;
    if (!timeMatch(evt.time)) continue;
    if (!evt.active) {
      evt.active = true;
      currentMusicVolume = evt.volume;
      if (isMusicPlaying && !isSpotPlaying) {
        audio.setVolume(currentMusicVolume);
      }
      Serial.printf("[SCHEDULER] Volume event activated: %d\n", evt.volume);
    }
  }
  
  for (int i = 0; i < scheduleEventCount; i++) {
    ScheduleEvent& evt = scheduleEvents[i];
    if (evt.type != "volume") continue;
    int eventH = evt.time.substring(0, 2).toInt();
    int eventM = evt.time.substring(3, 5).toInt();
    time_t t;
    struct tm ti;
    time(&t);
    localtime_r(&t, &ti);
    if (!(ti.tm_hour == eventH && ti.tm_min == eventM)) {
      if (evt.active) {
        evt.active = false;
        currentMusicVolume = defaultMusicVolume;
        if (isMusicPlaying && !isSpotPlaying) {
          audio.setVolume(currentMusicVolume);
        }
        Serial.printf("[SCHEDULER] Volume reset to default: %d\n", defaultMusicVolume);
      }
    }
  }
  
  for (int i = 0; i < scheduleEventCount; i++) {
    ScheduleEvent& evt = scheduleEvents[i];
    if (evt.type != "spot") continue;
    if (evt.day != currentDay) continue;
    if (evt.pauseUntil > 0 && now < evt.pauseUntil) {
      continue;
    }
    evt.pauseUntil = 0;
    if (timeMatch(evt.time)) {
      if (!evt.active) {
        evt.active = true;
        evt.spotRepeatCount = 0;
        String spotFile = findSpotFile(evt.target);
        if (spotFile.length() > 0) {
          playSpot(spotFile, evt.volume);
          evt.spotRepeatCount++;
          Serial.printf("[SCHEDULER] Spot event started: Spot%d (%dx)\n", evt.target, evt.repeat);
        }
      }
    } else {
      evt.active = false;
      evt.spotRepeatCount = 0;
    }
  }
  
  if (isSpotPlaying && !audio.isRunning()) {
    for (int i = 0; i < scheduleEventCount; i++) {
      ScheduleEvent& evt = scheduleEvents[i];
      if (evt.type != "spot") continue;
      if (!evt.active) continue;
      if (evt.spotRepeatCount < evt.repeat) {
        evt.pauseUntil = now + 2000;
        Serial.printf("[SCHEDULER] Spot pause 2sec, replay %d/%d\n", evt.spotRepeatCount + 1, evt.repeat);
      } else {
        evt.active = false;
        resumeMusic();
        Serial.printf("[SCHEDULER] Spot finished all replays, resuming music\n");
      }
      break;
    }
  }
  
  if (isSpotPlaying && !audio.isRunning()) {
    for (int i = 0; i < scheduleEventCount; i++) {
      ScheduleEvent& evt = scheduleEvents[i];
      if (evt.type != "spot") continue;
      if (!evt.active) continue;
      if (evt.pauseUntil == 0) continue;
      if (now >= evt.pauseUntil) {
        String spotFile = findSpotFile(evt.target);
        if (spotFile.length() > 0) {
          playSpot(spotFile, evt.volume);
          evt.spotRepeatCount++;
          Serial.printf("[SCHEDULER] Spot replayed: %d/%d\n", evt.spotRepeatCount, evt.repeat);
        }
      }
      break;
    }
  }
}

// ========== CONFIG MANAGEMENT ==========
bool loadConfig() {
  File f = SD.open("/config/system.json");
  if (!f) return false;
  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, f)) { f.close(); return false; }
  cfg.ssid = doc["wifi"]["ssid"] | ""; 
  cfg.pass = doc["wifi"]["pass"] | "";
  cfg.dhcp = doc["net"]["dhcp"] | true;
  cfg.ip = doc["net"]["ip"] | "192.168.1.100";
  cfg.gw = doc["net"]["gw"] | "192.168.1.1";
  cfg.mask = doc["net"]["mask"] | "255.255.255.0";
  cfg.dns = doc["net"]["dns"] | "8.8.8.8";
  cfg.mdns = doc["mdns"] | "jardio";
  cfg.musicVol = doc["volume"]["music"] | 15;
  cfg.spotVol = doc["volume"]["spot"] | 20;
  cfg.ntpServer = doc["time"]["ntp"] | "pool.ntp.org";
  cfg.tz = doc["time"]["tz"] | "CET-1CEST,M3.5.0,M10.5.0/3";
  defaultMusicVolume = cfg.musicVol;
  defaultSpotVolume = cfg.spotVol;
  currentMusicVolume = cfg.musicVol;
  currentSpotVolume = cfg.spotVol;
  f.close(); 
  return true;
}

void saveConfig() {
  File f = SD.open("/config/system.json", FILE_WRITE);
  StaticJsonDocument<2048> doc;
  doc["wifi"]["ssid"] = cfg.ssid; 
  doc["wifi"]["pass"] = cfg.pass;
  doc["net"]["dhcp"] = cfg.dhcp; 
  doc["net"]["ip"] = cfg.ip;
  doc["net"]["gw"] = cfg.gw; 
  doc["net"]["mask"] = cfg.mask; 
  doc["net"]["dns"] = cfg.dns;
  doc["mdns"] = cfg.mdns;
  doc["volume"]["music"] = cfg.musicVol;
  doc["volume"]["spot"] = cfg.spotVol;
  doc["time"]["ntp"] = cfg.ntpServer;
  doc["time"]["tz"] = cfg.tz;
  serializeJson(doc, f); 
  f.close();
}

// ========== AUDIO TASKS ==========
void audioTask(void *pvParameters) {
  for(;;) {
    audio.loop();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void schedulerTask(void *pvParameters) {
  for(;;) {
    processScheduleEvents();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ========== MEDIA INDEXING ==========
void indexTracks() {
  musicListCache = "";
  spotListCache = "";
  spotTriggersCache = "";
  
  File rootMusic = SD.open("/music");
  if (rootMusic) {
    File f = rootMusic.openNextFile();
    while(f) {
      if(!f.isDirectory()) {
        String n = f.name();
        String shortN = shortenName(n);
        String enc = urlEncode(n);
        musicListCache += "<div class='row'><span>" + shortN + "</span><button onclick='cmdPlay(\"/api/play?f=" + enc + "\")'>PLAY</button><button class='btn-del' onclick='cmdDel(\"/music/" + enc + "\")'>DEL</button></div>";
      }
      f = rootMusic.openNextFile();
    }
    rootMusic.close();
  }
  
  File rootSpot = SD.open("/spot");
  if (rootSpot) {
    File f = rootSpot.openNextFile();
    while(f) {
      if(!f.isDirectory()) {
        String n = f.name();
        String shortN = shortenName(n);
        String enc = urlEncode(n);
        spotListCache += "<div class='row'><span>" + shortN + "</span><button onclick='cmdPlay(\"/api/play-spot?f=" + enc + "&vol=20\")'>PLAY</button><button class='btn-del' onclick='cmdDel(\"/spot/" + enc + "\")'>DEL</button></div>";
      }
      f = rootSpot.openNextFile();
    }
    rootSpot.close();
  }
  
  spotTriggersCache = "";
  for (int i = 1; i <= 10; i++) {
    String spotFile = findSpotFile(i);
    String buttonText = spotFile.length() > 0 ? spotFile : String(i);
    String enc = spotFile.length() > 0 ? urlEncode(spotFile) : "";
    String disabled = spotFile.length() > 0 ? "" : " disabled style='opacity:0.4;'";
    String onclick = spotFile.length() > 0 ? "onclick='cmdPlay(\"/api/play-spot?f=" + enc + "&vol=" + String(defaultSpotVolume) + "\")'" : "";
    spotTriggersCache += "<button class='spot-btn' " + onclick + disabled + ">" + buttonText + "</button>";
  }
}

// ========== HTML UI FUNCTIONS ==========
String getHeader() {
  String h = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<style>*{margin:0;padding:0;box-sizing:border-box;}body{font-family:sans-serif;background:#0b0e14;color:#cfd3d7;display:flex;min-height:100vh;}"
       ".sidebar{width:70px;background:#161a23;display:flex;flex-direction:column;align-items:center;padding-top:12px;border-right:1px solid #282c34;flex-shrink:0;}"
       ".sidebar>div{color:#007bff;font-size:16px;font-weight:bold;margin-bottom:15px;}"
       ".sidebar a{color:#8b949e;text-decoration:none;font-size:11px;margin-bottom:14px;text-align:center;cursor:pointer;width:50px;}"
       ".sidebar a:hover{color:#007bff;}"
       ".main{flex:1;padding:12px;overflow-y:auto;}"
       "h1{font-size:18px;margin-bottom:10px;}"
       "h2{font-size:14px;margin-bottom:8px;}"
       ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:10px;}"
       ".tile{background:#1c212c;border-radius:6px;padding:10px;border:1px solid #282c34;}"
       "input,select,button{padding:7px;border-radius:4px;border:none;background:#282c34;color:#fff;font-size:13px;margin:4px 0;}"
       "input,select{width:100%;}"
       "button{background:#007bff;cursor:pointer;font-weight:bold;}"
       ".btn-del{background:#d73a49;padding:5px 10px;font-size:11px;margin-left:4px;}"
       ".spot-btn{background:#f39c12;padding:6px 4px;font-size:10px;margin:2px;width:calc(20% - 4px);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}"
       ".row{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid #2d333b;font-size:12px;}"
       ".row span{flex:1;}"
       ".row button{padding:4px 8px;font-size:10px;margin-left:4px;width:auto;}"
       "table{width:100%;border-collapse:collapse;font-size:11px;}"
       "th{background:#161a23;padding:5px 2px;text-align:center;}"
       "td{border:1px solid #333;padding:2px;height:20px;vertical-align:top;font-size:8px;}"
       ".evt-box{padding:1px;margin:0.5px;border-radius:2px;font-size:7px;color:white;cursor:pointer;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;}"
       ".evt-pl{background:#28a745;}.evt-vol{background:#ffc107;color:#000;}.evt-sp{background:#dc3545;}"
       ".modal{display:none;position:fixed;z-index:1000;left:0;top:0;width:100%;height:100%;background:rgba(0,0,0,0.7);}"
       ".modal-box{background:#1c212c;margin:5% auto;padding:12px;border:1px solid #007bff;width:90%;max-width:380px;border-radius:6px;}"
       ".close{color:#aaa;float:right;font-size:22px;cursor:pointer;}"
       ".close:hover{color:#fff;}"
       ".modal-btns{display:flex;gap:6px;margin-top:10px;flex-wrap:wrap;}"
       ".modal-btns button{flex:1;min-width:60px;}"
       ".checkbox-list{max-height:100px;overflow-y:auto;}"
       ".checkbox-list label{display:block;padding:3px;font-size:11px;}"
       ".checkbox-list input{margin-right:3px;}"
       "</style>";
  
  h += "<script>"
       "function cmdPlay(url){fetch(url).then(r=>{console.log('Play response:',r.status);if(r.ok)setTimeout(()=>location.reload(),100);}).catch(e=>console.error('Play error:',e));}"
       "function cmdDel(path){if(confirm('Smazat?'))fetch('/api/delete?p='+path).then(r=>{console.log('Delete response:',r.status);if(r.ok)setTimeout(()=>location.reload(),100);}).catch(e=>console.error('Delete error:',e));}"
       "function openModal(id){document.getElementById(id).style.display='block';}"
       "function closeModal(id){document.getElementById(id).style.display='none';}"
       "window.onclick=function(e){if(e.target.classList.contains('modal'))e.target.style.display='none';}"
       "function toggleAllDays(cb){document.querySelectorAll('.day-cb').forEach(b=>b.checked=cb.checked);}"
       "</script></head><body>";
  h += "<div class='sidebar'><div>J</div><a href='/'>DASH</a><a href='/media'>MEDIA</a><a href='/playlists'>PL</a><a href='/scheduler'>SCHED</a><a href='/system'>SYST</a></div>";
  return h;
}

String getDashboard() {
  time_t now; struct tm ti; time(&now); localtime_r(&now, &ti);
  char tS[9] = {0}, dS[11] = {0};
  strftime(tS, sizeof(tS), "%H:%M:%S", &ti);
  strftime(dS, sizeof(dS), "%d.%m.%Y", &ti);
  String h = getHeader();
  h += "<div class='main'><h1>Jardio SP1</h1><div class='grid'>";
  h += "<div class='tile'><h2>Čas & IP</h2><div style='font-size:16px;font-weight:bold;color:#fff;'>" + String(tS) + "</div><div style='color:#8b949e;font-size:11px;margin-top:4px;'>" + String(dS) + "<br>" + WiFi.localIP().toString() + "</div></div>";
  h += "<div class='tile'><h2>Playback</h2><div style='background:#282c34;padding:8px;border-radius:4px;font-size:13px;margin-bottom:6px;'>" + (isMusicPlaying && currentPlaylistNum > 0 ? "PL" + String(currentPlaylistNum) : "Ticho") + "</div><div style='display:flex;gap:4px;'><button onclick='cmdPlay(\"/api/resume\")' style='flex:1;'>PAUSE</button><button onclick='cmdPlay(\"/api/stop\")' style='background:#d73a49;flex:1;'>STOP</button></div></div>";
  h += "<div class='tile'><h2>Hlasitost</h2><div style='font-size:13px;color:#cfd3d7;'>Hudba: <strong>" + String(currentMusicVolume) + "</strong><br>Spoty: <strong>" + String(currentSpotVolume) + "</strong></div></div>";
  h += "<div class='tile'><h2>SPOTy 1-10</h2>" + spotTriggersCache + "</div>";
  h += "</div></div></body></html>";
  return h;
}

String getMediaUI() {
  String h = getHeader();
  h += "<div class='main'><h1>Media</h1><div class='grid'>";
  h += "<div class='tile'><h2>Music Upload</h2><form method='POST' action='/api/upload/music' enctype='multipart/form-data'><input type='file' name='u' multiple><button type='submit'>UPLOAD</button></form></div>";
  h += "<div class='tile'><h2>Spot Upload</h2><form method='POST' action='/api/upload/spot' enctype='multipart/form-data'><input type='file' name='u' multiple><button type='submit'>UPLOAD</button></form></div>";
  h += "<div class='tile'><h2>Music</h2>" + musicListCache + "</div>";
  h += "<div class='tile'><h2>Spoty</h2>" + spotListCache + "</div>";
  h += "</div></div></body></html>";
  return h;
}

String getPlaylistUI() {
  String h = getHeader();
  h += "<div class='main'><h1>Playlisty</h1><div class='tile'><form action='/api/saveplaylists' method='POST'>";
  h += "<div style='display:flex;gap:4px;margin-bottom:8px;flex-wrap:wrap;'>";
  for(int i=1; i<=10; i++) {
    h += "<label style='font-size:11px;'><input type='checkbox' onchange='toggleAllDays(this)'> " + String(i) + "</label>";
  }
  h += "</div>";
  File root = SD.open("/music");
  if (root) {
    File f = root.openNextFile();
    while(f) {
      if(!f.isDirectory()) {
        String n = f.name();
        String shortN = shortenName(n);
        String enc = urlEncode(n);
        h += "<div class='row'><span>" + shortN + "</span><div style='display:flex;gap:3px;'>";
        for(int i=1; i<=10; i++) {
          bool checked = isInPlaylist(n, i);
          h += "<label style='font-size:10px;'><input type='checkbox' name='pl" + String(i) + "' value='" + enc + "' " + (checked?"checked":"") + ">" + String(i) + "</label>";
        }
        h += "</div></div>";
      }
      f = root.openNextFile();
    }
    root.close();
  }
  h += "<button type='submit' style='width:100%;margin-top:8px;background:#28a745;'>SAVE</button></form></div></div></body></html>";
  return h;
}

String getSchedulerUI() {
  String h = getHeader();
  h += "<div class='main'><h1>Scheduler</h1><p style='color:#8b949e;font-size:10px;margin-bottom:6px;'>🟢 PL | 🟡 Vol | 🔴 Spot</p>";
  h += "<button onclick=\"openModal('createModal')\" style='background:#28a745;padding:7px 10px;margin-bottom:6px;'>+ NEW</button>";
  h += "<div style='overflow-x:auto;'><table><tr><th style='width:30px;'>H</th><th>Po</th><th>Út</th><th>St</th><th>Čt</th><th>Pá</th><th>So</th><th>Ne</th></tr>";
  
  for(int hour = 0; hour <= 23; hour++) {
    h += "<tr><td style='font-weight:bold;background:#161a23;'>" + String(hour) + "</td>";
    for(int day = 0; day < 7; day++) {
      h += "<td>";
      for(int i = 0; i < scheduleEventCount; i++) {
        ScheduleEvent& evt = scheduleEvents[i];
        if (evt.day != day) continue;
        int eventH = evt.time.substring(0, 2).toInt();
        if (eventH != hour) continue;
        String typeClass = (evt.type == "playlist") ? "evt-pl" : (evt.type == "volume") ? "evt-vol" : "evt-sp";
        String typeShort = (evt.type == "playlist") ? "PL" + String(evt.target) : (evt.type == "volume") ? "V" + String(evt.volume) : "S" + String(evt.target);
        h += "<div class='evt-box " + typeClass + "' onclick=\"openModal('editModal_" + evt.id + "')\" title='" + evt.time + "'>" + typeShort + "</div>";
      }
      h += "</td>";
    }
    h += "</tr>";
  }
  h += "</table></div>";
  
  // CREATE MODAL
  h += "<div id='createModal' class='modal'><div class='modal-box'><span class='close' onclick=\"closeModal('createModal')\">&times;</span><h2>New Event</h2><form id='createForm'>";
  h += "<label>Day:</label><select name='day' required>";
  String dayNames[] = {"Pondělí","Úterý","Středa","Čtvrtek","Pátek","Sobota","Neděle"};
  for(int i=0;i<7;i++) h += "<option value='" + String(i) + "'>" + dayNames[i] + "</option>";
  h += "</select><label style='margin-top:6px;display:block;'>Time:</label><input type='time' name='time' required>";
  h += "<label style='margin-top:6px;display:block;'>Type:</label><select name='type' onchange='updateForm()' required>";
  h += "<option value='playlist'>Playlist</option><option value='volume'>Volume</option><option value='spot'>Spot</option></select>";
  h += "<div id='playlistFields'><label style='margin-top:6px;display:block;'>Playlist:</label><select name='target'>";
  for(int i=1;i<=10;i++) h += "<option value='" + String(i) + "'>PL " + String(i) + "</option>";
  h += "</select></div>";
  h += "<div id='volumeFields' style='display:none;'><label style='margin-top:6px;display:block;'>Volume:</label><input type='number' name='volume' min='0' max='21' value='15'></div>";
  h += "<div id='spotFields' style='display:none;'><label style='margin-top:6px;display:block;'>Spot:</label><select name='target'>";
  for(int i=1;i<=10;i++) h += "<option value='" + String(i) + "'>Spot " + String(i) + "</option>";
  h += "</select><label style='margin-top:6px;display:block;'>Repeat:</label><select name='repeat'>";
  for(int i=1;i<=5;i++) h += "<option value='" + String(i) + "'>" + String(i) + "x</option>";
  h += "</select><label style='margin-top:6px;display:block;'>Volume:</label><input type='number' name='volume' min='0' max='21' value='20'></div>";
  h += "<div class='modal-btns'><button type='button' onclick='saveNewEvent()' style='background:#28a745;'>SAVE</button><button type='button' onclick=\"closeModal('createModal')\" style='background:#6c757d;'>CANCEL</button></div></form></div></div>";
  
  // EDIT MODALS
  for(int i = 0; i < scheduleEventCount; i++) {
    ScheduleEvent& evt = scheduleEvents[i];
    h += "<div id='editModal_" + evt.id + "' class='modal'><div class='modal-box'><span class='close' onclick=\"closeModal('editModal_" + evt.id + "')\">&times;</span><h2>Edit Event</h2><form id='editForm_" + evt.id + "'>";
    h += "<label>Day:</label><select name='day' required>";
    for(int d=0;d<7;d++) {
      String sel = (d == evt.day) ? "selected" : "";
      h += "<option value='" + String(d) + "' " + sel + ">" + dayNames[d] + "</option>";
    }
    h += "</select><label style='margin-top:6px;display:block;'>Time:</label><input type='time' name='time' value='" + evt.time + "' required>";
    h += "<label style='margin-top:6px;display:block;'>Type:</label><select disabled>";
    h += "<option>" + evt.type + "</option></select>";
    if(evt.type == "playlist") {
      h += "<label style='margin-top:6px;display:block;'>Playlist:</label><select name='target'>";
      for(int t=1;t<=10;t++) h += "<option value='" + String(t) + "' " + (t==evt.target?"selected":"") + ">PL " + String(t) + "</option>";
      h += "</select>";
    } else if(evt.type == "volume") {
      h += "<label style='margin-top:6px;display:block;'>Volume:</label><input type='number' name='volume' min='0' max='21' value='" + String(evt.volume) + "'>";
    } else if(evt.type == "spot") {
      h += "<label style='margin-top:6px;display:block;'>Spot:</label><select name='target'>";
      for(int t=1;t<=10;t++) h += "<option value='" + String(t) + "' " + (t==evt.target?"selected":"") + ">Spot " + String(t) + "</option>";
      h += "</select><label style='margin-top:6px;display:block;'>Repeat:</label><select name='repeat'>";
      for(int r=1;r<=5;r++) h += "<option value='" + String(r) + "' " + (r==evt.repeat?"selected":"") + ">" + String(r) + "x</option>";
      h += "</select><label style='margin-top:6px;display:block;'>Volume:</label><input type='number' name='volume' min='0' max='21' value='" + String(evt.volume) + "'>";
    }
    h += "<div class='modal-btns'><button type='button' onclick=\"saveEditEvent('" + evt.id + "')\" style='background:#28a745;'>SAVE</button>";
    h += "<button type='button' onclick=\"copyEvent('" + evt.id + "')\" style='background:#17a2b8;'>COPY</button>";
    h += "<button type='button' onclick=\"deleteEvent('" + evt.id + "')\" style='background:#dc3545;'>DEL</button>";
    h += "<button type='button' onclick=\"closeModal('editModal_" + evt.id + "')\" style='background:#6c757d;'>CLOSE</button></div></form></div></div>";
  }
  
  // COPY MODAL
  h += "<div id='copyModal' class='modal'><div class='modal-box'><span class='close' onclick=\"closeModal('copyModal')\">&times;</span><h2>Copy to Days</h2><div class='checkbox-list'>";
  h += "<label><input type='checkbox' onchange='toggleAllDays(this)'> <strong>ALL</strong></label>";
  for(int d=0;d<7;d++) h += "<label><input type='checkbox' class='day-cb' value='" + String(d) + "'> " + dayNames[d] + "</label>";
  h += "</div><div class='modal-btns'><button type='button' onclick='confirmCopy()' style='background:#28a745;'>COPY</button><button type='button' onclick=\"closeModal('copyModal')\" style='background:#6c757d;'>CANCEL</button></div></div></div>";
  
  h += "<script>";
  h += "let copyEventId='';";
  h += "function updateForm(){let t=document.querySelector('select[name=\"type\"]').value;document.getElementById('playlistFields').style.display=t==='playlist'?'block':'none';document.getElementById('volumeFields').style.display=t==='volume'?'block':'none';document.getElementById('spotFields').style.display=t==='spot'?'block':'none';}";
  h += "function saveNewEvent(){let f=document.getElementById('createForm');let d=new FormData(f);fetch('/api/event/add',{method:'POST',body:new URLSearchParams(d)}).then(r=>{console.log('Add:',r.status);if(r.ok||r.status===303)setTimeout(()=>location.reload(),100);}).catch(e=>console.error('Error:',e));}";
  h += "function saveEditEvent(id){let f=document.getElementById('editForm_'+id);let d=new FormData(f);fetch('/api/event/edit?id='+id,{method:'POST',body:new URLSearchParams(d)}).then(r=>{console.log('Edit:',r.status);if(r.ok||r.status===303)setTimeout(()=>location.reload(),100);}).catch(e=>console.error('Error:',e));}";
  h += "function deleteEvent(id){if(confirm('Delete?'))fetch('/api/event/delete?id='+id).then(r=>{console.log('Del:',r.status);if(r.ok)setTimeout(()=>location.reload(),100);}).catch(e=>console.error('Error:',e));}";
  h += "function copyEvent(id){copyEventId=id;openModal('copyModal');}";
  h += "function confirmCopy(){let days=[];document.querySelectorAll('.day-cb:checked').forEach(c=>days.push(c.value));fetch('/api/event/copy?id='+copyEventId+'&days='+days.join(',')).then(r=>{console.log('Copy:',r.status);if(r.ok||r.status===303)setTimeout(()=>location.reload(),100);}).catch(e=>console.error('Error:',e));}";
  h += "updateForm();";
  h += "</script>";
  
  h += "</div></body></html>";
  return h;
}

String getSystemUI() {
  String h = getHeader();
  h += "<div class='main'><h1>System</h1><div class='grid'>";
  h += "<div class='tile'><h2>Network</h2><form action='/api/net' method='POST'><label>Mode:</label><select name='dhcp'><option value='1' " + String(cfg.dhcp?"selected":"") + ">DHCP</option><option value='0' " + String(!cfg.dhcp?"selected":"") + ">Static</option></select>";
  h += "<label style='margin-top:6px;display:block;'>IP:</label><input name='ip' value='" + cfg.ip + "'><label style='margin-top:6px;display:block;'>GW:</label><input name='gw' value='" + cfg.gw + "'><label style='margin-top:6px;display:block;'>Mask:</label><input name='mask' value='" + cfg.mask + "'><label style='margin-top:6px;display:block;'>DNS:</label><input name='dns' value='" + cfg.dns + "'>";
  h += "<button type='submit' style='width:100%;margin-top:6px;'>SAVE</button></form></div>";
  h += "<div class='tile'><h2>Volume</h2><form action='/api/vol' method='POST'><label>Music:</label><select name='musicvol'>";
  for(int i=0;i<=21;i++) h += "<option value='" + String(i) + "' " + (defaultMusicVolume==i?"selected":"") + ">" + String(i) + "</option>";
  h += "</select><label style='margin-top:6px;display:block;'>Spot:</label><select name='spotvol'>";
  for(int i=0;i<=21;i++) h += "<option value='" + String(i) + "' " + (defaultSpotVolume==i?"selected":"") + ">" + String(i) + "</option>";
  h += "</select><button type='submit' style='width:100%;margin-top:6px;'>SAVE</button></form></div>";
  h += "<div class='tile'><h2>Time</h2><form action='/api/time' method='POST'><label>NTP:</label><input name='ntp' value='" + cfg.ntpServer + "'><label style='margin-top:6px;display:block;'>TZ:</label><input name='tz' value='" + cfg.tz + "'><button type='submit' style='width:100%;margin-top:6px;'>SAVE</button></form></div>";
  h += "<div class='tile'><button onclick=\"if(confirm('Restart?'))fetch('/api/restart').then(()=>setTimeout(()=>location.reload(),500));\" style='background:#d73a49;width:100%;padding:10px;'>RESTART</button></div>";
  h += "</div></div></body></html>";
  return h;
}

String getSetupUI() { 
  return "<h1 style='text-align:center;margin-top:50px;'>Setup Mode<br>Connect to: Jardio-SP1 / admin123</h1>"; 
}

// ========== REQUEST HANDLERS ==========
void handleSavePlaylists() {
  for(int i = 1; i <= 10; i++) {
    String filename = "/playlist/pl" + String(i) + ".csv";
    SD.remove(filename);
    File f = SD.open(filename, FILE_WRITE);
    if (f) {
      for(int j = 0; j < server.args(); j++) {
        if (server.argName(j) == "pl" + String(i)) {
          String val = server.arg(j);
          val = urlDecode(val);
          f.println(val);
        }
      }
      f.close();
    }
  }
  server.sendHeader("Location", "/playlists");
  server.send(303);
}

void handleFileUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String folder = server.uri().indexOf("/music") != -1 ? "/music/" : "/spot/";
    uploadFile = SD.open(folder + upload.filename, FILE_WRITE);
  } else if (upload.status == UPLOAD_FILE_WRITE && uploadFile) {
    uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END && uploadFile) {
    uploadFile.close();
  }
}

String generateEventId() {
  static unsigned long counter = 0;
  return "evt_" + String(millis()) + "_" + String(counter++);
}

void handleAddEvent() {
  if (server.hasArg("day") && server.hasArg("time") && server.hasArg("type")) {
    if (scheduleEventCount < 100) {
      ScheduleEvent& evt = scheduleEvents[scheduleEventCount];
      evt.id = generateEventId();
      evt.day = server.arg("day").toInt();
      evt.time = server.arg("time");
      evt.type = server.arg("type");
      evt.target = server.hasArg("target") ? server.arg("target").toInt() : 1;
      evt.repeat = server.hasArg("repeat") ? server.arg("repeat").toInt() : 1;
      evt.volume = server.hasArg("volume") ? server.arg("volume").toInt() : 15;
      evt.active = false;
      evt.spotRepeatCount = 0;
      evt.pauseUntil = 0;
      scheduleEventCount++;
      saveScheduleEvents();
      Serial.printf("[API] Event added: %s\n", evt.id.c_str());
      server.sendHeader("Location", "/scheduler");
      server.send(303);
      return;
    }
  }
  server.send(400);
}

void handleEditEvent() {
  if (server.hasArg("id")) {
    String eventId = server.arg("id");
    for (int i = 0; i < scheduleEventCount; i++) {
      if (scheduleEvents[i].id == eventId) {
        if (server.hasArg("day")) scheduleEvents[i].day = server.arg("day").toInt();
        if (server.hasArg("time")) scheduleEvents[i].time = server.arg("time");
        if (server.hasArg("target")) scheduleEvents[i].target = server.arg("target").toInt();
        if (server.hasArg("repeat")) scheduleEvents[i].repeat = server.arg("repeat").toInt();
        if (server.hasArg("volume")) scheduleEvents[i].volume = server.arg("volume").toInt();
        saveScheduleEvents();
        server.sendHeader("Location", "/scheduler");
        server.send(303);
        return;
      }
    }
  }
  server.send(404);
}

void handleDeleteEvent() {
  if (server.hasArg("id")) {
    String eventId = server.arg("id");
    for (int i = 0; i < scheduleEventCount; i++) {
      if (scheduleEvents[i].id == eventId) {
        for (int j = i; j < scheduleEventCount - 1; j++) {
          scheduleEvents[j] = scheduleEvents[j + 1];
        }
        scheduleEventCount--;
        saveScheduleEvents();
        server.send(200);
        return;
      }
    }
  }
  server.send(404);
}

void handleCopyEvent() {
  if (server.hasArg("id") && server.hasArg("days")) {
    String eventId = server.arg("id");
    String daysStr = server.arg("days");
    int srcIdx = -1;
    for (int i = 0; i < scheduleEventCount; i++) {
      if (scheduleEvents[i].id == eventId) {
        srcIdx = i;
        break;
      }
    }
    if (srcIdx < 0) {
      server.send(404);
      return;
    }
    int days[7];
    int dayCount = 0;
    String currentNum = "";
    for (int i = 0; i <= daysStr.length(); i++) {
      char c = (i < daysStr.length()) ? daysStr[i] : ',';
      if (c == ',') {
        if (currentNum.length() > 0) {
          days[dayCount++] = currentNum.toInt();
          currentNum = "";
        }
      } else {
        currentNum += c;
      }
    }
    for (int d = 0; d < dayCount; d++) {
      if (scheduleEventCount >= 100) break;
      ScheduleEvent src = scheduleEvents[srcIdx];
      ScheduleEvent& newEvt = scheduleEvents[scheduleEventCount];
      newEvt = src;
      newEvt.id = generateEventId();
      newEvt.day = days[d];
      newEvt.active = false;
      newEvt.spotRepeatCount = 0;
      newEvt.pauseUntil = 0;
      scheduleEventCount++;
    }
    saveScheduleEvents();
    server.sendHeader("Location", "/scheduler");
    server.send(303);
    return;
  }
  server.send(400);
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n[BOOT] Jardio OS v5.6.7.10 FIXED starting...");
  
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, 20000000)) { 
    Serial.println("[ERROR] SD card init FAILED!");
    while(1) delay(1000); 
  }
  Serial.println("[OK] SD card initialized");

  initPlaylists();
  initScheduler();
  const char* dirs[] = {"/config", "/music", "/playlist", "/scheduler", "/spot"};
  for(const char* d : dirs) if(!SD.exists(d)) SD.mkdir(d);

  indexTracks();
  loadConfig();
  loadScheduleEvents();
  Serial.printf("[CONFIG] Loaded - Music Vol: %d, Spot Vol: %d\n", defaultMusicVolume, defaultSpotVolume);

  if (cfg.ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    if (!cfg.dhcp) {
      IPAddress ip, gw, nm, d1; 
      ip.fromString(cfg.ip); gw.fromString(cfg.gw); nm.fromString(cfg.mask); d1.fromString(cfg.dns);
      WiFi.config(ip, gw, nm, d1);
    }
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    unsigned long st = millis(); 
    while (WiFi.status() != WL_CONNECTED && millis()-st < 8000) delay(500);
  }

  if (WiFi.status() != WL_CONNECTED) { 
    apMode = true; 
    WiFi.mode(WIFI_AP); 
    WiFi.softAP("Jardio-SP1", "admin123"); 
    Serial.println("[WIFI] AP Mode: Jardio-SP1 / admin123");
  } else { 
    Serial.printf("[WIFI] Connected: %s - IP: %s\n", cfg.ssid.c_str(), WiFi.localIP().toString().c_str());
    MDNS.begin(cfg.mdns.c_str()); 
    configTime(0, 0, cfg.ntpServer.c_str()); 
    setenv("TZ", cfg.tz.c_str(), 1); 
    tzset(); 
  }

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(currentMusicVolume);
  Serial.println("[AUDIO] I2S configured");

  xTaskCreatePinnedToCore(audioTask, "AudioTask", 20000, NULL, 6, NULL, 1);
  xTaskCreatePinnedToCore(schedulerTask, "SchedulerTask", 5000, NULL, 4, &schedulerTaskHandle, 0);
  Serial.println("[TASKS] Created");

  server.on("/", []() { server.send(200, "text/html", apMode ? getSetupUI() : getDashboard()); });
  server.on("/media", []() { server.send(200, "text/html", getMediaUI()); });
  server.on("/playlists", []() { server.send(200, "text/html", getPlaylistUI()); });
  server.on("/scheduler", []() { server.send(200, "text/html", getSchedulerUI()); });
  server.on("/system", []() { server.send(200, "text/html", getSystemUI()); });

  server.on("/api/saveplaylists", HTTP_POST, handleSavePlaylists);
  server.on("/api/event/add", HTTP_POST, handleAddEvent);
  server.on("/api/event/edit", HTTP_POST, handleEditEvent);
  server.on("/api/event/delete", handleDeleteEvent);
  server.on("/api/event/copy", handleCopyEvent);

  server.on("/api/play", []() { if (server.hasArg("f")) { String fName = urlDecode(server.arg("f")); playPlaylist(1); } server.send(200); });
  server.on("/api/play-spot", []() { if (server.hasArg("f")) { String fName = urlDecode(server.arg("f")); int vol = server.hasArg("vol") ? server.arg("vol").toInt() : defaultSpotVolume; playSpot(fName, vol); } server.send(200); });
  server.on("/api/stop", []() { audio.stopSong(); isSpotPlaying = false; isMusicPlaying = false; currentPlaylistNum = 0; server.send(200); });
  server.on("/api/resume", []() { audio.pauseResume(); server.send(200); });
  server.on("/api/vol", []() { if(server.hasArg("musicvol")) { defaultMusicVolume = server.arg("musicvol").toInt(); currentMusicVolume = defaultMusicVolume; cfg.musicVol = defaultMusicVolume; } if(server.hasArg("spotvol")) { defaultSpotVolume = server.arg("spotvol").toInt(); currentSpotVolume = defaultSpotVolume; cfg.spotVol = defaultSpotVolume; } saveConfig(); server.sendHeader("Location", "/system"); server.send(303); });
  server.on("/api/time", []() { if(server.hasArg("ntp")) cfg.ntpServer = server.arg("ntp"); if(server.hasArg("tz")) cfg.tz = server.arg("tz"); saveConfig(); configTime(0, 0, cfg.ntpServer.c_str()); setenv("TZ", cfg.tz.c_str(), 1); tzset(); server.sendHeader("Location", "/system"); server.send(303); });
  server.on("/api/net", []() { cfg.dhcp = (server.arg("dhcp") == "1"); cfg.ip = server.arg("ip"); cfg.gw = server.arg("gw"); cfg.mask = server.arg("mask"); cfg.dns = server.arg("dns"); saveConfig(); server.sendHeader("Location", "/system"); server.send(303); });
  server.on("/api/restart", []() { server.send(200); delay(500); ESP.restart(); });
  server.on("/api/upload/music", HTTP_POST, [](){ indexTracks(); server.sendHeader("Location", "/media"); server.send(303); }, handleFileUpload);
  server.on("/api/upload/spot", HTTP_POST, [](){ indexTracks(); server.sendHeader("Location", "/media"); server.send(303); }, handleFileUpload);
  server.on("/api/delete", []() { if (server.hasArg("p")) { SD.remove(server.arg("p")); indexTracks(); } server.send(200); });

  server.begin(); 
  Serial.println("[OK] Web server started");
  Serial.println("[BOOT] Jardio OS v5.6.7.10 READY!");
}

void loop() {
  server.handleClient();
  delay(1);
}
