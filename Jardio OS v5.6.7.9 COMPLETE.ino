/* Jardio OS v5.6.7.9 - Multi-Timeline Calendar Scheduler */
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
  String id;              // Unique ID
  int day;                // 0=Mon, 1=Tue ... 6=Sun
  String time;            // "HH:MM"
  String type;            // "playlist" / "volume" / "spot"
  int target;             // 1-10 (playlist/spot number)
  int repeat;             // 1-5 (for spot)
  int volume;             // 0-21 (for spot/volume)
  bool active;            // Is currently active
  unsigned long spotEndTime;   // When current spot ends (for replay)
  int spotRepeatCount;    // How many times spot has played
  unsigned long pauseUntil;    // Pause until timestamp
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
void setMusicVolume(int vol);
void playPlaylist(int playlistNum);
void playSpot(String filename, int volume);
void resumeMusic();
int getSpotPrefix(String filename);
String findSpotFile(int spotNum);

// ========== GLOBAL VARIABLES ==========
Audio audio;
WebServer server(80);
WiFiServer apiServer(11000);
File uploadFile;
SPIClass sdSPI(HSPI);

String musicListCache = "";
String spotListCache = "";
String spotTriggersCache = "";

int defaultMusicVolume = 15;  // Default from System settings
int defaultSpotVolume = 20;
int currentMusicVolume = 15;  // Current music volume
int currentSpotVolume = 20;

bool isSpotPlaying = false;
bool isMusicPlaying = false;
int currentPlaylistNum = 0;   // Which playlist is playing

String nowPlayingTrack = "";
String lastPlayedFile = "";
String currentPlaylistName = "";

// Schedule events array
ScheduleEvent scheduleEvents[100];
int scheduleEventCount = 0;
int lastActivePlaylistNum = -1;  // Track last active playlist

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

// ========== SPOT PREFIX PARSER ==========
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
  
  // Read array directly from root
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
    Serial.println("[SCHEDULER] Failed to open events.json for writing");
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
  return (ti.tm_wday + 6) % 7;  // 0=Mon, 1=Tue ... 6=Sun
}

String getCurrentTimeString() {
  time_t now;
  struct tm ti;
  time(&now);
  localtime_r(&now, &ti);
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", ti.tm_hour, ti.tm_min);
  return String(buf);
}

void playPlaylist(int playlistNum) {
  // Get first file from playlist
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
  
  // ========== TIMELINE A: PLAYLIST EVENTS ==========
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
  
  // Check if any playlist event is still active in this minute
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
    
    // Event is active during its minute
    if (ti.tm_hour == eventH && ti.tm_min == eventM) {
      activePlaylistNum = evt.target;
      evt.active = true;
    } else {
      evt.active = false;
    }
  }
  
  // Apply playlist change if needed
  if (activePlaylistNum > 0 && activePlaylistNum != currentPlaylistNum) {
    playPlaylist(activePlaylistNum);
    lastActivePlaylistNum = activePlaylistNum;
  } else if (activePlaylistNum < 0 && isMusicPlaying) {
    // No active playlist event - stop music
    audio.stopSong();
    isMusicPlaying = false;
    currentPlaylistNum = 0;
    Serial.println("[SCHEDULER] No active playlist - silence");
  }
  
  // ========== TIMELINE B: VOLUME EVENTS ==========
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
  
  // Clear volume event active flag after minute passes
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
  
  // ========== TIMELINE C: SPOT EVENTS ==========
  for (int i = 0; i < scheduleEventCount; i++) {
    ScheduleEvent& evt = scheduleEvents[i];
    
    if (evt.type != "spot") continue;
    if (evt.day != currentDay) continue;
    
    // Check if pause timer is active
    if (evt.pauseUntil > 0 && now < evt.pauseUntil) {
      continue;  // Still in pause
    }
    evt.pauseUntil = 0;  // Clear pause
    
    // Check if we're in the event's minute
    if (timeMatch(evt.time)) {
      if (!evt.active) {
        evt.active = true;
        evt.spotRepeatCount = 0;
        
        // Play spot
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
  
  // ========== SPOT REPLAY LOGIC ==========
  if (isSpotPlaying && !audio.isRunning()) {
    // Spot finished playing
    for (int i = 0; i < scheduleEventCount; i++) {
      ScheduleEvent& evt = scheduleEvents[i];
      if (evt.type != "spot") continue;
      if (!evt.active) continue;
      
      if (evt.spotRepeatCount < evt.repeat) {
        // Need to replay
        evt.pauseUntil = now + 2000;  // 2 second pause
        Serial.printf("[SCHEDULER] Spot pause 2sec, replay %d/%d\n", evt.spotRepeatCount + 1, evt.repeat);
      } else {
        // All replays done, resume music
        evt.active = false;
        resumeMusic();
        Serial.printf("[SCHEDULER] Spot finished all replays, resuming music\n");
      }
      break;
    }
  }
  
  // Replay spot after pause
  if (isSpotPlaying && audio.isRunning() == false) {
    for (int i = 0; i < scheduleEventCount; i++) {
      ScheduleEvent& evt = scheduleEvents[i];
      if (evt.type != "spot") continue;
      if (!evt.active) continue;
      if (evt.pauseUntil == 0) continue;
      if (now >= evt.pauseUntil) {
        // Time to replay
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
    vTaskDelay(pdMS_TO_TICKS(100));  // Check every 100ms for precise timing
  }
}

// ========== MEDIA INDEXING ==========
void indexTracks() {
  musicListCache = "";
  spotListCache = "";
  spotTriggersCache = "";
  
  // Index music files
  File rootMusic = SD.open("/music");
  if (rootMusic) {
    File f = rootMusic.openNextFile();
    while(f) {
      if(!f.isDirectory()) {
        String n = f.name();
        String shortN = shortenName(n);
        String enc = urlEncode(n);
        musicListCache += "<div class='media-item'><span>" + shortN + "</span><div><button class='btn-s' onclick=\"cmd('/api/play?f=" + enc + "')\">PLAY</button><button class='btn-s del' onclick=\"del('/music/" + enc + "')\">DEL</button></div></div>";
      }
      f = rootMusic.openNextFile();
    }
    rootMusic.close();
  }
  
  // Index spot files
  File rootSpot = SD.open("/spot");
  if (rootSpot) {
    File f = rootSpot.openNextFile();
    while(f) {
      if(!f.isDirectory()) {
        String n = f.name();
        String shortN = shortenName(n);
        String enc = urlEncode(n);
        spotListCache += "<div class='media-item'><span>" + shortN + "</span><div><button class='btn-s' onclick=\"cmd('/api/play-spot?f=" + enc + "&vol=20')\">PLAY</button><button class='btn-s del' onclick=\"del('/spot/" + enc + "')\">DEL</button></div></div>";
      }
      f = rootSpot.openNextFile();
    }
    rootSpot.close();
  }
  
  // Generate ALL 10 spot trigger buttons
  spotTriggersCache = "";
  for (int i = 1; i <= 10; i++) {
    String spotFile = findSpotFile(i);
    String buttonText = spotFile.length() > 0 ? spotFile : String(i);
    String enc = spotFile.length() > 0 ? urlEncode(spotFile) : "";
    String onclick = spotFile.length() > 0 ? "onclick=\"cmd('/api/play-spot?f=" + enc + "&vol=" + String(defaultSpotVolume) + "')\"" : "disabled";
    String disabled = spotFile.length() > 0 ? "" : " style='opacity:0.5;cursor:not-allowed;'";
    
    spotTriggersCache += "<button class='spot-trigger' " + onclick + disabled + ">" + buttonText + "</button>";
  }
}

// ========== HTML UI FUNCTIONS ==========
String getHeader() {
  String h = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<style>body{font-family:sans-serif;background:#0b0e14;color:#cfd3d7;margin:0;display:flex;height:100vh;}"
       ".sidebar{width:100px;background:#161a23;display:flex;flex-direction:column;align-items:center;padding-top:20px;border-right:1px solid #282c34;}"
       ".sidebar a{color:#8b949e;text-decoration:none;font-size:11px;margin-bottom:25px;text-align:center;cursor:pointer;}"
       ".main{flex:1;padding:25px;overflow-y:auto;} .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(350px,1fr));gap:20px;}"
       ".tile{background:#1c212c;border-radius:12px;padding:20px;} .clock{font-size:32px;font-weight:bold;color:#fff;}"
       "input, select, button{padding:10px;border-radius:6px;margin:5px 0;border:none;box-sizing:border-box;background:#282c34;color:#fff;}"
       "button{background:#007bff;cursor:pointer;font-weight:bold;} .btn-s{width:auto;padding:5px 10px;font-size:11px;margin-left:5px;} .del{background:#d73a49;}"
       ".media-item{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #2d333b;align-items:center;}"
       ".spot-trigger{background:#f39c12;margin:4px;width:calc(20% - 8px);font-size:11px;padding:8px 4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}"
       "table{width:100%;border-collapse:collapse;background:#1c212c;}"
       "th{background:#161a23;padding:8px;text-align:center;font-size:12px;}"
       "td{border:1px solid #333;padding:4px;height:60px;vertical-align:top;font-size:10px;position:relative;}"
       ".event-box{padding:2px;margin:1px;border-radius:3px;font-size:9px;color:white;cursor:pointer;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
       ".evt-playlist{background:#28a745;}"
       ".evt-volume{background:#ffc107;color:#000;}"
       ".evt-spot{background:#dc3545;}"
       ".modal{display:none;position:fixed;z-index:1000;left:0;top:0;width:100%;height:100%;background-color:rgba(0,0,0,0.5);}"
       ".modal-content{background:#1c212c;margin:5% auto;padding:20px;border:1px solid #007bff;width:80%;max-width:400px;border-radius:8px;}"
       ".close{color:#aaa;float:right;font-size:28px;font-weight:bold;cursor:pointer;}"
       ".close:hover{color:#fff;}"
       "input[type=text],input[type=time],input[type=number],select{width:100%;margin:8px 0;}"
       ".modal-buttons{display:flex;gap:10px;margin-top:20px;}"
       ".modal-buttons button{flex:1;}"
       ".checkbox-list{max-height:150px;overflow-y:auto;}"
       ".checkbox-list label{display:block;padding:5px;cursor:pointer;}"
       ".checkbox-list input{margin-right:5px;}"
       "</style>";
  
  h += "<script>"
       "function cmd(u){fetch(u).then(()=>{location.reload();});}"
       "function del(p){if(confirm('Smazat?')) fetch('/api/delete?p='+p).then(()=>location.reload());}"
       "function openModal(id){document.getElementById(id).style.display='block';}"
       "function closeModal(id){document.getElementById(id).style.display='none';}"
       "window.onclick=function(e){if(e.target.classList.contains('modal')) e.target.style.display='none';}"
       "function toggleAll(cb){let boxes=document.querySelectorAll('.day-checkbox');boxes.forEach(b=>b.checked=cb.checked);}"
       "</script></head><body>";
  h += "<div class='sidebar'><div style='color:#007bff;font-size:24px;font-weight:bold;margin-bottom:30px;'>J</div>";
  h += "<a href='/'>DASHBOARD</a><a href='/media'>MEDIA</a><a href='/playlists'>PLAYLISTS</a><a href='/scheduler'>SCHEDULER</a><a href='/system'>SYSTEM</a></div>";
  return h;
}

String getDashboard() {
  time_t now; struct tm ti; time(&now); localtime_r(&now, &ti);
  char tS[9] = {0}; char dS[11] = {0}; 
  strftime(tS, sizeof(tS), "%H:%M:%S", &ti);
  strftime(dS, sizeof(dS), "%d.%m.%Y", &ti);
  String h = getHeader();
  h += "<div class='main'><h1>Jardio SP1</h1><div class='grid'>";
  h += "<div class='tile'><h2>Čas a Stav</h2><div class='clock'>" + String(tS) + "</div><div style='color:#8b949e;'>" + String(dS) + "<br>IP: " + WiFi.localIP().toString() + "</div></div>";
  h += "<div class='tile'><h2>Právě hraje</h2><div style='background:#282c34;padding:15px;border-radius:8px;'>" + (isMusicPlaying && currentPlaylistNum > 0 ? "PL" + String(currentPlaylistNum) : "Ticho") + "</div>";
  h += "<div style='display:flex;gap:10px;margin-top:15px;'><button onclick=\"cmd('/api/resume')\">PAUSE/RESUME</button><button onclick=\"cmd('/api/stop')\" style='background:#d73a49;'>STOP</button></div></div>";
  h += "<div class='tile'><h2>Hlasitost</h2><div style='color:#cfd3d7;'>Hudba: <strong>" + String(currentMusicVolume) + "</strong><br>Spoty: <strong>" + String(currentSpotVolume) + "</strong></div></div>";
  h += "<div class='tile'><h2>Rychlá hlášení (SPOTy 1-10)</h2>" + spotTriggersCache + "</div>";
  h += "</div></div></body></html>";
  return h;
}

String getMediaUI() {
  String h = getHeader();
  h += "<div class='main'><h1>Správa Media</h1><div class='grid'>";
  h += "<div class='tile'><h2>Import Music</h2><form method='POST' action='/api/upload/music' enctype='multipart/form-data'><input type='file' name='u' multiple><button type='submit'>NAHRÁT DO /music</button></form></div>";
  h += "<div class='tile'><h2>Import Spoty</h2><form method='POST' action='/api/upload/spot' enctype='multipart/form-data'><input type='file' name='u' multiple><button type='submit'>NAHRÁT DO /spot</button></form></div>";
  h += "<div class='tile'><h2>Hudba (/music)</h2>" + musicListCache + "</div>";
  h += "<div class='tile'><h2>Spoty (/spot)</h2>" + spotListCache + "</div>";
  h += "</div></div></body></html>";
  return h;
}

String getPlaylistUI() {
  String h = getHeader();
  h += "<div class='main'><h1>Playlisty - Multiselect</h1><div class='tile'>";
  h += "<p><strong>Vyberte skladby a zaškrtněte playlisty.</strong></p>";
  h += "<form action='/api/saveplaylists' method='POST'>";
  h += "<div class='media-item' style='background:#282c34;padding:10px;font-weight:bold;'><span><strong>Označit vše</strong></span>";
  h += "<div style='display:flex;gap:12px;flex-wrap:wrap;'>";
  for(int i=1; i<=10; i++) {
    h += "<label><input type='checkbox' onclick=\"toggleAll(this)\" id='all" + String(i) + "'> " + String(i) + "</label>";
  }
  h += "</div></div>";
  File root = SD.open("/music");
  if (root) {
    File f = root.openNextFile();
    while(f) {
      if(!f.isDirectory()) {
        String n = f.name();
        String shortN = shortenName(n);
        String enc = urlEncode(n);
        h += "<div class='media-item'><span>" + shortN + "</span>";
        h += "<div style='display:flex;gap:8px;flex-wrap:wrap;'>";
        for(int i=1; i<=10; i++) {
          bool checked = isInPlaylist(n, i);
          h += "<label style='margin:2px 6px; white-space:nowrap;'><input type='checkbox' name='pl" + String(i) + "' value='" + enc + "' " + (checked ? "checked" : "") + "> " + String(i) + "</label>";
        }
        h += "</div></div>";
      }
      f = root.openNextFile();
    }
    root.close();
  }
  h += "<br><button type='submit' style='background:#28a745;padding:12px;'>ULOŽIT VŠECHNY PLAYLISTY</button>";
  h += "</form></div></div></body></html>";
  return h;
}

String getSchedulerUI() {
  String h = getHeader();
  h += "<div class='main'><h1>Týdenní Kalendář - Timeline Scheduler</h1>";
  h += "<p style='color:#8b949e;font-size:12px;'>🟢 Playlist | 🟡 Volume | 🔴 Spot | Klik na event = Edit</p>";
  
  h += "<div style='overflow-x:auto;'><table>";
  h += "<tr><th style='width:50px;'>Čas</th><th>Po</th><th>Út</th><th>St</th><th>Čt</th><th>Pá</th><th>So</th><th>Ne</th></tr>";
  
  // For each hour
  for(int hour = 0; hour <= 23; hour++) {
    h += "<tr>";
    h += "<td style='font-weight:bold;background:#161a23;'>" + String(hour) + ":00</td>";
    
    // For each day
    for(int day = 0; day < 7; day++) {
      h += "<td>";
      
      // Find events for this hour/day
      for(int i = 0; i < scheduleEventCount; i++) {
        ScheduleEvent& evt = scheduleEvents[i];
        if (evt.day != day) continue;
        
        int eventH = evt.time.substring(0, 2).toInt();
        if (eventH != hour) continue;
        
        String typeClass = "evt-" + evt.type;
        String typeShort = (evt.type == "playlist") ? "PL" + String(evt.target) : 
                           (evt.type == "volume") ? "V" + String(evt.volume) : 
                           "S" + String(evt.target);
        
        h += "<div class='event-box " + typeClass + "' onclick=\"openModal('editModal_" + evt.id + "')\" title='" + evt.time + " " + typeShort + "'>" + typeShort + "</div>";
      }
      
      h += "</td>";
    }
    h += "</tr>";
  }
  
  h += "</table></div>";
  
  // NEW EVENT BUTTON
  h += "<div style='margin-top:20px;'>";
  h += "<button style='background:#28a745;padding:12px;width:auto;' onclick=\"openModal('createModal')\">+ NOVÝ EVENT</button>";
  h += "</div>";
  
  // CREATE EVENT MODAL
  h += "<div id='createModal' class='modal'>";
  h += "<div class='modal-content'>";
  h += "<span class='close' onclick=\"closeModal('createModal')\">&times;</span>";
  h += "<h2>Nový Event</h2>";
  h += "<form id='createForm'>";
  
  h += "Den:<br><select name='day' required>";
  String dayNames[] = {"Pondělí", "Úterý", "Středa", "Čtvrtek", "Pátek", "Sobota", "Neděle"};
  for(int i=0; i<7; i++) h += "<option value='" + String(i) + "'>" + dayNames[i] + "</option>";
  h += "</select><br>";
  
  h += "Čas:<br><input type='time' name='time' required><br>";
  
  h += "Typ:<br><select name='type' onchange='updateEventForm()' required>";
  h += "<option value='playlist'>Playlist</option>";
  h += "<option value='volume'>Hlasitost</option>";
  h += "<option value='spot'>Spot</option>";
  h += "</select><br>";
  
  h += "<div id='playlistFields'>";
  h += "Playlist:<br><select name='target'>";
  for(int i=1; i<=10; i++) h += "<option value='" + String(i) + "'>PL " + String(i) + "</option>";
  h += "</select><br>";
  h += "</div>";
  
  h += "<div id='volumeFields' style='display:none;'>";
  h += "Hlasitost:<br><input type='number' name='volume' min='0' max='21' value='15'><br>";
  h += "</div>";
  
  h += "<div id='spotFields' style='display:none;'>";
  h += "Spot:<br><select name='target'>";
  for(int i=1; i<=10; i++) h += "<option value='" + String(i) + "'>Spot " + String(i) + "</option>";
  h += "</select><br>";
  h += "Opakování:<br><select name='repeat'>";
  for(int i=1; i<=5; i++) h += "<option value='" + String(i) + "'>" + String(i) + "x</option>";
  h += "</select><br>";
  h += "Hlasitost:<br><input type='number' name='volume' min='0' max='21' value='20'><br>";
  h += "</div>";
  
  h += "<div class='modal-buttons'>";
  h += "<button type='button' onclick=\"saveNewEvent()\" style='background:#28a745;'>ULOŽIT</button>";
  h += "<button type='button' onclick=\"closeModal('createModal')\" style='background:#6c757d;'>ZRUŠIT</button>";
  h += "</div>";
  h += "</form>";
  h += "</div></div>";
  
  // EDIT MODALS (for each event)
  for(int i = 0; i < scheduleEventCount; i++) {
    ScheduleEvent& evt = scheduleEvents[i];
    
    h += "<div id='editModal_" + evt.id + "' class='modal'>";
    h += "<div class='modal-content'>";
    h += "<span class='close' onclick=\"closeModal('editModal_" + evt.id + "')\">&times;</span>";
    h += "<h2>Editovat Event</h2>";
    h += "<form id='editForm_" + evt.id + "'>";
    
    h += "Den:<br><select name='day' required>";
    for(int d=0; d<7; d++) {
      String sel = (d == evt.day) ? "selected" : "";
      h += "<option value='" + String(d) + "' " + sel + ">" + dayNames[d] + "</option>";
    }
    h += "</select><br>";
    
    h += "Čas:<br><input type='time' name='time' value='" + evt.time + "' required><br>";
    
    h += "Typ:<br><select name='type' disabled>";
    h += "<option selected>" + evt.type + "</option>";
    h += "</select><br>";
    
    if(evt.type == "playlist") {
      h += "Playlist:<br><select name='target'>";
      for(int t=1; t<=10; t++) {
        String sel = (t == evt.target) ? "selected" : "";
        h += "<option value='" + String(t) + "' " + sel + ">PL " + String(t) + "</option>";
      }
      h += "</select><br>";
    } else if(evt.type == "volume") {
      h += "Hlasitost:<br><input type='number' name='volume' min='0' max='21' value='" + String(evt.volume) + "'><br>";
    } else if(evt.type == "spot") {
      h += "Spot:<br><select name='target'>";
      for(int t=1; t<=10; t++) {
        String sel = (t == evt.target) ? "selected" : "";
        h += "<option value='" + String(t) + "' " + sel + ">Spot " + String(t) + "</option>";
      }
      h += "</select><br>";
      h += "Opakování:<br><select name='repeat'>";
      for(int r=1; r<=5; r++) {
        String sel = (r == evt.repeat) ? "selected" : "";
        h += "<option value='" + String(r) + "' " + sel + ">" + String(r) + "x</option>";
      }
      h += "</select><br>";
      h += "Hlasitost:<br><input type='number' name='volume' min='0' max='21' value='" + String(evt.volume) + "'><br>";
    }
    
    h += "<div class='modal-buttons'>";
    h += "<button type='button' onclick=\"saveEditEvent('" + evt.id + "')\" style='background:#28a745;'>ULOŽIT</button>";
    h += "<button type='button' onclick=\"copyEvent('" + evt.id + "')\" style='background:#17a2b8;'>KOPÍROVAT</button>";
    h += "<button type='button' onclick=\"deleteEvent('" + evt.id + "')\" style='background:#dc3545;'>SMAZAT</button>";
    h += "<button type='button' onclick=\"closeModal('editModal_" + evt.id + "')\" style='background:#6c757d;'>ZAVŘÍT</button>";
    h += "</div>";
    h += "</form>";
    h += "</div></div>";
  }
  
  // COPY MODAL
  h += "<div id='copyModal' class='modal'>";
  h += "<div class='modal-content'>";
  h += "<span class='close' onclick=\"closeModal('copyModal')\">&times;</span>";
  h += "<h2>Kopírovat Event do Dní</h2>";
  h += "<div class='checkbox-list'>";
  h += "<label><input type='checkbox' onchange='toggleAll(this)'> <strong>VŠECHNY DNY</strong></label>";
  for(int d=0; d<7; d++) {
    h += "<label><input type='checkbox' class='day-checkbox' value='" + String(d) + "'> " + dayNames[d] + "</label>";
  }
  h += "</div>";
  h += "<div class='modal-buttons'>";
  h += "<button type='button' onclick=\"confirmCopy()\" style='background:#28a745;'>KOPÍROVAT</button>";
  h += "<button type='button' onclick=\"closeModal('copyModal')\" style='background:#6c757d;'>ZRUŠIT</button>";
  h += "</div>";
  h += "</div></div>";
  
  h += "<script>";
  h += "let copyEventId = '';";
  h += "function updateEventForm(){";
  h += "  let type = document.querySelector('select[name=\"type\"]').value;";
  h += "  document.getElementById('playlistFields').style.display = type==='playlist'?'block':'none';";
  h += "  document.getElementById('volumeFields').style.display = type==='volume'?'block':'none';";
  h += "  document.getElementById('spotFields').style.display = type==='spot'?'block':'none';";
  h += "}";
  h += "function saveNewEvent(){";
  h += "  let form = document.getElementById('createForm');";
  h += "  let data = new FormData(form);";
  h += "  fetch('/api/event/add', {method:'POST', body: new URLSearchParams(data)}).then(()=>location.reload());";
  h += "}";
  h += "function saveEditEvent(id){";
  h += "  let form = document.getElementById('editForm_'+id);";
  h += "  let data = new FormData(form);";
  h += "  fetch('/api/event/edit?id='+id, {method:'POST', body: new URLSearchParams(data)}).then(()=>location.reload());";
  h += "}";
  h += "function deleteEvent(id){";
  h += "  if(confirm('Smazat event?')) fetch('/api/event/delete?id='+id).then(()=>location.reload());";
  h += "}";
  h += "function copyEvent(id){";
  h += "  copyEventId = id;";
  h += "  openModal('copyModal');";
  h += "}";
  h += "function confirmCopy(){";
  h += "  let days = [];";
  h += "  document.querySelectorAll('.day-checkbox:checked').forEach(cb => days.push(cb.value));";
  h += "  fetch('/api/event/copy?id='+copyEventId+'&days='+days.join(',')).then(()=>location.reload());";
  h += "}";
  h += "updateEventForm();";
  h += "</script>";
  
  h += "</div></body></html>";
  return h;
}

String getSystemUI() {
  String h = getHeader();
  h += "<div class='main'><h1>Nastavení systému</h1><div class='grid'>";
  h += "<div class='tile'><h2>Síťové nastavení</h2><form action='/api/net' method='POST'>";
  h += "Režim <select name='dhcp'><option value='1' "+String(cfg.dhcp?"selected":"")+">DHCP</option><option value='0' "+String(!cfg.dhcp?"selected":"")+">Statika</option></select>";
  h += "IP <input name='ip' value='"+cfg.ip+"'> GW <input name='gw' value='"+cfg.gw+"'> Mask <input name='mask' value='"+cfg.mask+"'> DNS <input name='dns' value='"+cfg.dns+"'>";
  h += "<button type='submit'>ULOŽIT SÍŤ</button></form></div>";
  h += "<div class='tile'><h2>Hlasitost</h2><form action='/api/vol' method='POST'>";
  h += "Hudba: <select name='musicvol'>";
  for(int i=0; i<=21; i++) h += "<option value='"+String(i)+"' "+String(defaultMusicVolume==i?"selected":"")+">"+String(i)+"</option>";
  h += "</select><br>Spoty: <select name='spotvol'>";
  for(int i=0; i<=21; i++) h += "<option value='"+String(i)+"' "+String(defaultSpotVolume==i?"selected":"")+">"+String(i)+"</option>";
  h += "</select><button type='submit'>ULOŽIT HLASITOST</button></form></div>";
  h += "<div class='tile'><h2>Časová synchronizace</h2><form action='/api/time' method='POST'>";
  h += "NTP: <input name='ntp' value='"+cfg.ntpServer+"'><br>Timezone: <input name='tz' value='"+cfg.tz+"'>";
  h += "<button type='submit'>ULOŽIT ČAS</button></form></div>";
  h += "<div class='tile'><button onclick=\"if(confirm('Restartovat?')) fetch('/api/restart').then(()=>location.reload());\" style='background:#d73a49;padding:15px;'>RESTART ESP32</button></div>";
  h += "</div></div></body></html>";
  return h;
}

String getSetupUI() { 
  return "<h1 style='text-align:center;margin-top:50px;'>Setup Mode - Připojte se k: Jardio-SP1 (admin123)</h1>"; 
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
      
      Serial.printf("[API] Event added: %s (day=%d, time=%s, type=%s)\n", evt.id.c_str(), evt.day, evt.time.c_str(), evt.type.c_str());
      
      server.sendHeader("Location", "/scheduler");
      server.send(303);
      return;
    }
  }
  server.send(400, "text/plain", "Missing parameters");
}

void handleEditEvent() {
  if (server.hasArg("id")) {
    String eventId = server.arg("id");
    
    for (int i = 0; i < scheduleEventCount; i++) {
      if (scheduleEvents[i].id == eventId) {
        scheduleEvents[i].day = server.hasArg("day") ? server.arg("day").toInt() : scheduleEvents[i].day;
        scheduleEvents[i].time = server.hasArg("time") ? server.arg("time") : scheduleEvents[i].time;
        if (server.hasArg("target")) scheduleEvents[i].target = server.arg("target").toInt();
        if (server.hasArg("repeat")) scheduleEvents[i].repeat = server.arg("repeat").toInt();
        if (server.hasArg("volume")) scheduleEvents[i].volume = server.arg("volume").toInt();
        
        saveScheduleEvents();
        Serial.printf("[API] Event edited: %s\n", eventId.c_str());
        
        server.sendHeader("Location", "/scheduler");
        server.send(303);
        return;
      }
    }
  }
  server.send(404, "text/plain", "Event not found");
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
        
        Serial.printf("[API] Event deleted: %s\n", eventId.c_str());
        server.send(200);
        return;
      }
    }
  }
  server.send(404, "text/plain", "Event not found");
}

void handleCopyEvent() {
  if (server.hasArg("id") && server.hasArg("days")) {
    String eventId = server.arg("id");
    String daysStr = server.arg("days");
    
    // Find source event
    int srcIdx = -1;
    for (int i = 0; i < scheduleEventCount; i++) {
      if (scheduleEvents[i].id == eventId) {
        srcIdx = i;
        break;
      }
    }
    
    if (srcIdx < 0) {
      server.send(404, "text/plain", "Event not found");
      return;
    }
    
    // Parse days
    int days[7];
    int dayCount = 0;
    int idx = 0;
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
    
    // Copy to each day
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
    Serial.printf("[API] Event copied to %d days\n", dayCount);
    
    server.sendHeader("Location", "/scheduler");
    server.send(303);
    return;
  }
  server.send(400, "text/plain", "Missing parameters");
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n[BOOT] Jardio OS v5.6.7.9 Multi-Timeline starting...");
  
  // Initialize SD card
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, 20000000)) { 
    Serial.println("[ERROR] SD card init FAILED!");
    while(1) delay(1000); 
  }
  Serial.println("[OK] SD card initialized");

  // Create directories
  initPlaylists();
  initScheduler();
  const char* dirs[] = {"/config", "/music", "/playlist", "/scheduler", "/spot"};
  for(const char* d : dirs) if(!SD.exists(d)) SD.mkdir(d);

  // Load configuration and events
  indexTracks();
  loadConfig();
  loadScheduleEvents();
  Serial.printf("[CONFIG] Loaded - Music Vol: %d, Spot Vol: %d\n", defaultMusicVolume, defaultSpotVolume);

  // Connect to WiFi
  if (cfg.ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    if (!cfg.dhcp) {
      IPAddress ip, gw, nm, d1; 
      ip.fromString(cfg.ip); 
      gw.fromString(cfg.gw); 
      nm.fromString(cfg.mask); 
      d1.fromString(cfg.dns);
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
    Serial.println("[WIFI] Access Point started: Jardio-SP1 / admin123");
  } else { 
    Serial.printf("[WIFI] Connected to %s - IP: %s\n", cfg.ssid.c_str(), WiFi.localIP().toString().c_str());
    MDNS.begin(cfg.mdns.c_str()); 
    configTime(0, 0, cfg.ntpServer.c_str()); 
    setenv("TZ", cfg.tz.c_str(), 1); 
    tzset(); 
  }

  // Initialize audio
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(currentMusicVolume);
  Serial.println("[AUDIO] I2S configured - DAC: PCM5102A");

  // Create tasks
  xTaskCreatePinnedToCore(audioTask, "AudioTask", 20000, NULL, 6, NULL, 1);
  xTaskCreatePinnedToCore(schedulerTask, "SchedulerTask", 5000, NULL, 4, &schedulerTaskHandle, 0);
  Serial.println("[TASKS] Audio and Scheduler tasks created (100ms check)");

  // Setup web server routes
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

  server.on("/api/play", []() { 
    if (server.hasArg("f")) { 
      String fName = urlDecode(server.arg("f"));
      playPlaylist(1);  // Demo - would need to track which playlist
      Serial.printf("[API] Manual play: %s\n", fName.c_str());
    } 
    server.send(200); 
  });
  
  server.on("/api/play-spot", []() { 
    if (server.hasArg("f")) { 
      String fName = urlDecode(server.arg("f"));
      int vol = server.hasArg("vol") ? server.arg("vol").toInt() : defaultSpotVolume;
      playSpot(fName, vol);
    } 
    server.send(200); 
  });

  server.on("/api/stop", []() { 
    audio.stopSong(); 
    isSpotPlaying = false;
    isMusicPlaying = false;
    currentPlaylistNum = 0;
    Serial.println("[API] Playback stopped");
    server.send(200); 
  });
  
  server.on("/api/resume", []() { 
    audio.pauseResume(); 
    Serial.println("[API] Pause/Resume toggled");
    server.send(200); 
  });
  
  server.on("/api/vol", []() {
    if(server.hasArg("musicvol")) { 
      defaultMusicVolume = server.arg("musicvol").toInt(); 
      currentMusicVolume = defaultMusicVolume;
      cfg.musicVol = defaultMusicVolume; 
    }
    if(server.hasArg("spotvol")) { 
      defaultSpotVolume = server.arg("spotvol").toInt(); 
      currentSpotVolume = defaultSpotVolume;
      cfg.spotVol = defaultSpotVolume; 
    }
    saveConfig();
    if (!isSpotPlaying) {
      audio.setVolume(currentMusicVolume);
    }
    Serial.printf("[API] Volume set - Music: %d, Spot: %d\n", defaultMusicVolume, defaultSpotVolume);
    server.sendHeader("Location", "/system"); 
    server.send(303);
  });
  
  server.on("/api/time", []() {
    if(server.hasArg("ntp")) cfg.ntpServer = server.arg("ntp");
    if(server.hasArg("tz")) cfg.tz = server.arg("tz");
    saveConfig();
    configTime(0, 0, cfg.ntpServer.c_str());
    setenv("TZ", cfg.tz.c_str(), 1); 
    tzset();
    Serial.println("[API] Time settings updated");
    server.sendHeader("Location", "/system"); 
    server.send(303);
  });
  
  server.on("/api/net", []() {
    cfg.dhcp = (server.arg("dhcp") == "1"); 
    cfg.ip = server.arg("ip"); 
    cfg.gw = server.arg("gw");
    cfg.mask = server.arg("mask"); 
    cfg.dns = server.arg("dns");
    saveConfig();
    Serial.println("[API] Network settings updated");
    server.sendHeader("Location", "/system"); 
    server.send(303);
  });
  
  server.on("/api/restart", []() { 
    server.send(200); 
    delay(500); 
    ESP.restart(); 
  });
  
  server.on("/api/upload/music", HTTP_POST, [](){ indexTracks(); server.sendHeader("Location", "/media"); server.send(303); }, handleFileUpload);
  server.on("/api/upload/spot", HTTP_POST, [](){ indexTracks(); server.sendHeader("Location", "/media"); server.send(303); }, handleFileUpload);
  
  server.on("/api/delete", []() { 
    if (server.hasArg("p")) { 
      SD.remove(server.arg("p")); 
      indexTracks();
      Serial.printf("[API] File deleted: %s\n", server.arg("p").c_str());
    } 
    server.send(200); 
  });

  server.begin(); 
  Serial.println("[OK] Web server started on port 80");
  Serial.println("[BOOT] Jardio OS v5.6.7.9 ready! Multi-Timeline Active");
}

// ========== MAIN LOOP ==========
void loop() {
  server.handleClient();
  delay(1);
}
