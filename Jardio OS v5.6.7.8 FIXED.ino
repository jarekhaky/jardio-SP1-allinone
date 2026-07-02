/* Jardio OS v5.6.7.8 - Calendar Scheduler FIXED */
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

// ========== SCHEDULE EVENT STRUCTURE ==========
struct ScheduleEvent {
  int day;           // 0=Mon, 1=Tue ... 6=Sun
  String startTime;  // "HH:MM"
  String endTime;    // "HH:MM"
  String type;       // "playlist" / "spot" / "volume"
  int target;        // 1-10 (playlist/spot number or volume level)
  int repeat;        // how many times to repeat
  int volume;        // volume if type=="volume"
  bool active;       // is event currently active
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
void playSpot(String filename);
void resumeMusic();

// ========== GLOBAL VARIABLES ==========
Audio audio;
WebServer server(80);
WiFiServer apiServer(11000);
File uploadFile;
SPIClass sdSPI(HSPI);

String musicListCache = "";
String spotListCache = "";
String spotTriggersCache = "";
String eventsCache = "";

int musicVolume = 15;
int spotVolume = 20;
int currentPlaylistVolume = 15;  // Remember volume for current playlist
bool isSpotPlaying = false;
bool isMusicPlaying = false;

String nowPlayingTrack = "";
String lastPlayedFile = "";
String pausedAt = "";  // Track pause point for resume

// Schedule events array
ScheduleEvent scheduleEvents[50];  // Max 50 events
int scheduleEventCount = 0;

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

// ========== SCHEDULE FUNCTIONS ==========
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
  
  StaticJsonDocument<8192> doc;
  if (deserializeJson(doc, f)) {
    Serial.println("[SCHEDULER] Failed to parse events.json");
    f.close();
    return;
  }
  
  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    if (scheduleEventCount >= 50) break;
    
    scheduleEvents[scheduleEventCount].day = obj["day"] | 0;
    scheduleEvents[scheduleEventCount].startTime = obj["start"].as<String>();
    scheduleEvents[scheduleEventCount].endTime = obj["end"].as<String>();
    scheduleEvents[scheduleEventCount].type = obj["type"].as<String>();
    scheduleEvents[scheduleEventCount].target = obj["target"] | 1;
    scheduleEvents[scheduleEventCount].repeat = obj["repeat"] | 1;
    scheduleEvents[scheduleEventCount].volume = obj["vol"] | 15;
    scheduleEvents[scheduleEventCount].active = false;
    
    scheduleEventCount++;
  }
  
  f.close();
  Serial.printf("[SCHEDULER] Loaded %d events\n", scheduleEventCount);
}

void saveScheduleEvents() {
  File f = SD.open("/scheduler/events.json", FILE_WRITE);
  if (!f) {
    Serial.println("[SCHEDULER] Failed to open events.json for writing");
    return;
  }
  
  StaticJsonDocument<8192> doc;
  JsonArray arr = doc.createNestedArray("events");
  
  for (int i = 0; i < scheduleEventCount; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["day"] = scheduleEvents[i].day;
    obj["start"] = scheduleEvents[i].startTime;
    obj["end"] = scheduleEvents[i].endTime;
    obj["type"] = scheduleEvents[i].type;
    obj["target"] = scheduleEvents[i].target;
    obj["repeat"] = scheduleEvents[i].repeat;
    obj["vol"] = scheduleEvents[i].volume;
  }
  
  serializeJson(doc, f);
  f.close();
  Serial.printf("[SCHEDULER] Saved %d events\n", scheduleEventCount);
}

bool timeInRange(String startTime, String endTime) {
  time_t now;
  struct tm ti;
  time(&now);
  localtime_r(&now, &ti);
  
  int currentHour = ti.tm_hour;
  int currentMin = ti.tm_min;
  int currentTimeInMin = currentHour * 60 + currentMin;
  
  int startH = startTime.substring(0, 2).toInt();
  int startM = startTime.substring(3, 5).toInt();
  int startTimeInMin = startH * 60 + startM;
  
  int endH = endTime.substring(0, 2).toInt();
  int endM = endTime.substring(3, 5).toInt();
  int endTimeInMin = endH * 60 + endM;
  
  return (currentTimeInMin >= startTimeInMin && currentTimeInMin < endTimeInMin);
}

int getCurrentDayOfWeek() {
  time_t now;
  struct tm ti;
  time(&now);
  localtime_r(&now, &ti);
  return (ti.tm_wday + 6) % 7;  // 0=Mon, 1=Tue ... 6=Sun
}

void processScheduleEvents() {
  int currentDay = getCurrentDayOfWeek();
  
  for (int i = 0; i < scheduleEventCount; i++) {
    ScheduleEvent& evt = scheduleEvents[i];
    
    // Check if event applies to current day
    if (evt.day != currentDay) continue;
    
    // Check if current time is within event range
    bool isNowInRange = timeInRange(evt.startTime, evt.endTime);
    
    // Handle state transitions
    if (isNowInRange && !evt.active) {
      // Event just started
      evt.active = true;
      Serial.printf("[SCHEDULER] Event activated: type=%s, target=%d\n", evt.type.c_str(), evt.target);
      
      if (evt.type == "volume") {
        setMusicVolume(evt.volume);
      } else if (evt.type == "playlist") {
        // Start playing playlist (implementation depends on your playlist structure)
        // For now, just remember the volume for when music plays
        currentPlaylistVolume = evt.volume;
      }
    } else if (!isNowInRange && evt.active) {
      // Event just ended
      evt.active = false;
      Serial.printf("[SCHEDULER] Event deactivated: type=%s\n", evt.type.c_str());
    }
  }
}

void setMusicVolume(int vol) {
  musicVolume = vol;
  currentPlaylistVolume = vol;
  if (!isSpotPlaying) {
    audio.setVolume(musicVolume);
  }
  Serial.printf("[VOLUME] Music volume set to %d\n", vol);
}

void playSpot(String filename) {
  Serial.printf("[AUDIO] Playing spot: %s\n", filename.c_str());
  isSpotPlaying = true;
  // Save current state before playing spot
  pausedAt = "restored";  // Flag to resume music after spot
  audio.setVolume(spotVolume);
  audio.connecttoFS(SD, ("/spot/" + filename).c_str());
}

void resumeMusic() {
  Serial.println("[AUDIO] Resuming music after spot");
  isSpotPlaying = false;
  // Restore music volume to scheduled volume (not generic spotVolume)
  audio.setVolume(currentPlaylistVolume);
  if (lastPlayedFile != "") {
    audio.connecttoFS(SD, lastPlayedFile.c_str());
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
        spotListCache += "<div class='media-item'><span>" + shortN + "</span><div><button class='btn-s' onclick=\"cmd('/api/play-spot?f=" + enc + "')\">PLAY</button><button class='btn-s del' onclick=\"del('/spot/" + enc + "')\">DEL</button></div></div>";
      }
      f = rootSpot.openNextFile();
    }
    rootSpot.close();
  }
  
  // Generate ALL 10 spot trigger buttons (with or without files)
  spotTriggersCache = "";
  for (int i = 1; i <= 10; i++) {
    // Check if file exists for this spot number
    String spotFile = "";
    File rootSpot2 = SD.open("/spot");
    if (rootSpot2) {
      File f = rootSpot2.openNextFile();
      while(f) {
        if (!f.isDirectory()) {
          String n = f.name();
          if (n.length() >= 3 && isdigit(n[0]) && n[1] == '_') {
            int num = n.substring(0, 1).toInt();
            if (num == i) {
              spotFile = n;
              break;
            }
          }
        }
        f = rootSpot2.openNextFile();
      }
      rootSpot2.close();
    }
    
    String buttonText = String(i) + (spotFile.length() > 0 ? "" : "_");
    String enc = spotFile.length() > 0 ? urlEncode(spotFile) : "";
    String onclick = spotFile.length() > 0 ? "onclick=\"cmd('/api/play-spot?f=" + enc + "')\"" : "";
    
    spotTriggersCache += "<button class='spot-trigger' " + onclick + ">" + buttonText + "</button>";
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
  musicVolume = cfg.musicVol;
  spotVolume = cfg.spotVol;
  currentPlaylistVolume = cfg.musicVol;
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
    
    // Detect when spot playback finishes
    if (isSpotPlaying && !audio.isRunning()) {
      Serial.println("[AUDIO] Spot finished, resuming music");
      resumeMusic();
    }
    
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void schedulerTask(void *pvParameters) {
  for(;;) {
    // Process schedule events every 10 seconds
    processScheduleEvents();
    vTaskDelay(pdMS_TO_TICKS(10000));
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
       "input, select, button{width:100%;padding:10px;border-radius:6px;margin:5px 0;border:none;box-sizing:border-box;background:#282c34;color:#fff;}"
       "button{background:#007bff;cursor:pointer;font-weight:bold;} .btn-s{width:auto;padding:5px 10px;font-size:11px;margin-left:5px;} .del{background:#d73a49;}"
       ".media-item{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #2d333b;align-items:center;}"
       ".spot-trigger{background:#f39c12;margin:4px;width:calc(20% - 8px);display:inline-block;font-size:11px;padding:8px 4px;}</style>";
  h += "<script>"
       "function cmd(u){fetch(u).then(()=>{location.reload();});}"
       "function del(p){fetch('/api/delete?p='+p).then(()=>location.reload());}"
       "function toggleAll(cb, col) {let boxes = document.querySelectorAll('input[name=pl' + col + ']');boxes.forEach(box => box.checked = cb.checked);}"
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
  h += "<div class='tile'><h2>Právě hraje</h2><div style='background:#282c34;padding:15px;border-radius:8px;'>" + (nowPlayingTrack.length() ? shortenName(nowPlayingTrack) : "Neaktivní") + "</div>";
  h += "<div style='display:flex;gap:10px;margin-top:15px;'><button onclick=\"cmd('/api/resume')\">PAUSE/RESUME</button><button onclick=\"cmd('/api/stop')\" style='background:#d73a49;'>STOP</button></div></div>";
  h += "<div class='tile'><h2>Hlasitost</h2><div style='color:#cfd3d7;'>Hudba: <strong>" + String(musicVolume) + "</strong><br>Spoty: <strong>" + String(spotVolume) + "</strong></div></div>";
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
    h += "<label><input type='checkbox' onclick=\"toggleAll(this, " + String(i) + ")\" id='all" + String(i) + "'> " + String(i) + "</label>";
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
  h += "<div class='main'><h1>Týdenní Kalendář (00:00 - 23:00)</h1>";
  h += "<div style='overflow-x:auto;'><table style='width:100%;border-collapse:collapse;table-layout:fixed;background:#1c212c;'>";
  h += "<tr><th style='width:70px;background:#161a23;'>Hodina</th><th>Po</th><th>Út</th><th>St</th><th>Čt</th><th>Pá</th><th>So</th><th>Ne</th></tr>";
  for(int hour = 0; hour <= 23; hour++) {
    h += "<tr style='height:48px;border-bottom:1px solid #333;'>";
    h += "<td style='text-align:right;padding-right:10px;font-weight:bold;background:#161a23;'>" + String(hour) + ":00</td>";
    for(int day = 0; day < 7; day++) {
      h += "<td style='border:1px solid #333;position:relative;vertical-align:top;'></td>";
    }
    h += "</tr>";
  }
  h += "</table></div>";
  
  // Display scheduled events
  if (scheduleEventCount > 0) {
    h += "<div style='margin-top:15px;background:#282c34;padding:10px;'>";
    h += "<h3>Naplánované události (" + String(scheduleEventCount) + "):</h3>";
    for (int i = 0; i < scheduleEventCount; i++) {
      String dayName[] = {"Pondělí", "Úterý", "Středa", "Čtvrtek", "Pátek", "Sobota", "Neděle"};
      h += "<div style='background:#3498db;color:white;padding:8px;margin:5px 0;border-radius:4px;'>";
      h += dayName[scheduleEvents[i].day] + " | " + scheduleEvents[i].startTime + "-" + scheduleEvents[i].endTime + " | " 
        + scheduleEvents[i].type + " #" + String(scheduleEvents[i].target);
      if (scheduleEvents[i].type == "volume") {
        h += " (" + String(scheduleEvents[i].volume) + "%)";
      }
      h += " <button style='background:#d73a49;padding:4px 8px;' onclick=\"if(confirm('Smazat?')) fetch('/api/delevent?idx=" + String(i) + "').then(()=>location.reload());\">X</button>";
      h += "</div>";
    }
    h += "</div>";
  }

  h += "<div class='tile' style='margin-top:20px;'><h2>Přidat událost</h2>";
  h += "<form action='/api/addschedule' method='POST'>";
  h += "Den: <select name='day'>";
  String dayNames[] = {"Pondělí", "Úterý", "Středa", "Čtvrtek", "Pátek", "Sobota", "Neděle"};
  for (int i = 0; i < 7; i++) {
    h += "<option value='" + String(i) + "'>" + dayNames[i] + "</option>";
  }
  h += "</select><br><br>";
  h += "Od: <input type='time' name='start' value='08:00'> Do: <input type='time' name='end' value='22:00'><br><br>";
  h += "Typ: <select name='type'><option value='playlist'>Playlist</option><option value='spot'>Spot</option><option value='volume'>Hlasitost</option></select><br><br>";
  h += "Cíl: <select name='target'>";
  for(int i=1; i<=10; i++) h += "<option value='" + String(i) + "'>PL/Spot " + String(i) + "</option>";
  h += "</select><br><br>";
  h += "Opakování: <select name='repeat'><option value='1'>1x</option><option value='2'>2x</option><option value='3'>3x</option><option value='4'>4x</option><option value='5'>5x</option></select><br><br>";
  h += "Hlasitost: <input type='number' name='vol' value='15' min='0' max='21'><br><br>";
  h += "<button type='submit' style='background:#28a745;padding:12px;'>PŘIDAT UDÁLOST</button>";
  h += "</form></div></div></body></html>";
  return h;
}

String getSystemUI() {
  String h = getHeader();
  h += "<div class='main'><h1>Nastavení systému</h1><div class='grid'>";
  h += "<div class='tile'><h2>Síťové nastavení</h2><form action='/api/net'>";
  h += "Režim <select name='dhcp'><option value='1' "+String(cfg.dhcp?"selected":"")+">DHCP</option><option value='0' "+String(!cfg.dhcp?"selected":"")+">Statika</option></select>";
  h += "IP <input name='ip' value='"+cfg.ip+"'> GW <input name='gw' value='"+cfg.gw+"'> Mask <input name='mask' value='"+cfg.mask+"'> DNS <input name='dns' value='"+cfg.dns+"'>";
  h += "<button type='submit'>ULOŽIT SÍŤ</button></form></div>";
  h += "<div class='tile'><h2>Hlasitost</h2><form action='/api/vol'>";
  h += "Hudba: <select name='musicvol'>";
  for(int i=0; i<=21; i++) h += "<option value='"+String(i)+"' "+String(musicVolume==i?"selected":"")+">"+String(i)+"</option>";
  h += "</select><br>Spoty: <select name='spotvol'>";
  for(int i=0; i<=21; i++) h += "<option value='"+String(i)+"' "+String(spotVolume==i?"selected":"")+">"+String(i)+"</option>";
  h += "</select><button type='submit'>ULOŽIT HLASITOST</button></form></div>";
  h += "<div class='tile'><h2>Časová synchronizace</h2><form action='/api/time'>";
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

void handleAddSchedule() {
  if (server.hasArg("day") && server.hasArg("start") && server.hasArg("type")) {
    if (scheduleEventCount < 50) {
      ScheduleEvent& evt = scheduleEvents[scheduleEventCount];
      evt.day = server.arg("day").toInt();
      evt.startTime = server.arg("start");
      evt.endTime = server.arg("end");
      evt.type = server.arg("type");
      evt.target = server.arg("target").toInt();
      evt.repeat = server.arg("repeat").toInt();
      evt.volume = server.arg("vol").toInt();
      evt.active = false;
      scheduleEventCount++;
      saveScheduleEvents();
      Serial.printf("[SCHEDULER] Added event: day=%d, type=%s\n", evt.day, evt.type.c_str());
    }
  }
  server.sendHeader("Location", "/scheduler");
  server.send(303);
}

void handleDeleteEvent() {
  if (server.hasArg("idx")) {
    int idx = server.arg("idx").toInt();
    if (idx >= 0 && idx < scheduleEventCount) {
      for (int i = idx; i < scheduleEventCount - 1; i++) {
        scheduleEvents[i] = scheduleEvents[i + 1];
      }
      scheduleEventCount--;
      saveScheduleEvents();
      Serial.printf("[SCHEDULER] Deleted event at index %d\n", idx);
    }
  }
  server.send(200);
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

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n[BOOT] Jardio OS v5.6.7.8 starting...");
  
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
  Serial.printf("[CONFIG] Loaded - Music Vol: %d, Spot Vol: %d\n", musicVolume, spotVolume);

  // Connect to WiFi
  if (loadConfig()) {
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
  audio.setVolume(musicVolume);
  Serial.println("[AUDIO] I2S configured - DAC: PCM5102A");

  // Create tasks
  xTaskCreatePinnedToCore(audioTask, "AudioTask", 20000, NULL, 6, NULL, 1);
  xTaskCreatePinnedToCore(schedulerTask, "SchedulerTask", 5000, NULL, 4, &schedulerTaskHandle, 0);
  Serial.println("[TASKS] Audio and Scheduler tasks created");

  // Setup web server routes
  server.on("/", []() { server.send(200, "text/html", apMode ? getSetupUI() : getDashboard()); });
  server.on("/media", []() { server.send(200, "text/html", getMediaUI()); });
  server.on("/playlists", []() { server.send(200, "text/html", getPlaylistUI()); });
  server.on("/scheduler", []() { server.send(200, "text/html", getSchedulerUI()); });
  server.on("/system", []() { server.send(200, "text/html", getSystemUI()); });

  server.on("/api/saveplaylists", HTTP_POST, handleSavePlaylists);
  server.on("/api/addschedule", HTTP_POST, handleAddSchedule);
  server.on("/api/delevent", handleDeleteEvent);

  server.on("/api/play", []() { 
    if (server.hasArg("f")) { 
      String fName = urlDecode(server.arg("f"));
      nowPlayingTrack = fName;
      lastPlayedFile = "/music/" + fName;
      audio.connecttoFS(SD, lastPlayedFile.c_str());
      audio.setVolume(currentPlaylistVolume);
      isSpotPlaying = false;
      isMusicPlaying = true;
      Serial.printf("[API] Playing music: %s (vol=%d)\n", fName.c_str(), currentPlaylistVolume);
    } 
    server.send(200); 
  });
  
  server.on("/api/play-spot", []() { 
    if (server.hasArg("f")) { 
      String fName = urlDecode(server.arg("f"));
      playSpot(fName);
    } 
    server.send(200); 
  });

  server.on("/api/stop", []() { 
    audio.stopSong(); 
    isSpotPlaying = false;
    isMusicPlaying = false;
    nowPlayingTrack = "";
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
      musicVolume = server.arg("musicvol").toInt(); 
      currentPlaylistVolume = musicVolume;
      cfg.musicVol = musicVolume; 
    }
    if(server.hasArg("spotvol")) { 
      spotVolume = server.arg("spotvol").toInt(); 
      cfg.spotVol = spotVolume; 
    }
    saveConfig();
    if (!isSpotPlaying) {
      audio.setVolume(musicVolume);
    }
    Serial.printf("[API] Volume set - Music: %d, Spot: %d\n", musicVolume, spotVolume);
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
  Serial.println("[BOOT] Jardio OS ready!");
}

// ========== MAIN LOOP ==========
void loop() {
  server.handleClient();
  delay(1);
}
