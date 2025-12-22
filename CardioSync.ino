/**************** HEART RATE + SpO2 + GPS + SMS + GRAPH + AUDIO ****************/

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <MAX30100_PulseOximeter.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

/**************** CONFIGURATION (EDIT THIS) ****************/
const char* ssid = "Wi-Fi Name";
const char* password = "Wi-Fi PASS";

// TWILIO CREDENTIALS
const char* accountSID = "Twilio SSID";
const char* authToken  = "Twilio Token";
const char* twilioFrom = "+Purchased Virtual Number"; 

// EMERGENCY CONTACTS
const int NUM_CONTACTS = 3;
const char* contacts[NUM_CONTACTS] = {
  "Emergency Contact 1", 
  "Emergency Contact 2", 
  "Emergency Contact 3",  
};

/**************** TFT DISPLAY ****************/
#define TFT_CS  5
#define TFT_RST 4 
#define TFT_DC  2
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

/**************** GLOBAL OBJECTS ****************/
WebServer server(80);
PulseOximeter pox;
TinyGPSPlus gps;
HardwareSerial GPS_Serial(2);   

/**************** SENSOR VARIABLES ****************/
float heartRate = 0;
float SpO2 = 0;
double currentLat = 0.0;
double currentLng = 0.0;

/**************** ALARM VARIABLES ****************/
const int HR_HIGH_THRESHOLD_1 = 100;
const int HR_HIGH_THRESHOLD_2 = 130; 
const int LHR_THRESHOLD = 50;        

bool alarm1Active = false;
bool alarm2Active = false; 
bool lhrAlarmActive = false; 

bool alarm1Canceled = false; 
bool alarm2Canceled = false;
bool lhrAlarmCanceled = false;

const unsigned long LHR_ACTIVATION_DELAY_MS = 15000; 
unsigned long setupTime = 0; 
bool sensorStabilized = false; 

/**************** SMS ABORT LOGIC ****************/
bool smsPending = false;
unsigned long smsTimerStart = 0;
const unsigned long SMS_ABORT_WINDOW = 7000; 
int pendingSmsType = 0; 
bool smsSentForThisEvent = false; 

/**************** TIMERS ****************/
unsigned long lastScreenUpdate = 0;
const int SCREEN_INTERVAL = 500;

/**************** FUNCTION DECLARATIONS ****************/
void handleRoot();
void handleData();
void handleCancel();
void updateTFTDisplay();
void checkAlarms();
void checkSMSLogic();
bool sendTwilioSMS(String toPhone, String message);
String urlEncode(const String &str);

/**************** SETUP ****************/
void setup() {
  Serial.begin(115200);

  // TFT Init
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); 
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.print("System Starting...");

  // I2C Init
  Wire.begin(21, 22);

  // WiFi Init
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    if (millis() - wifiStart > 20000) break;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(WiFi.localIP());
  }

  // MAX30100 Init
  if (!pox.begin()) {
    tft.fillScreen(ST77XX_RED);
    tft.setCursor(0,0);
    tft.print("SENSOR FAIL");
    while(1);
  }
  pox.setIRLedCurrent(MAX30100_LED_CURR_50MA);

  // GPS Init
  GPS_Serial.begin(9600, SERIAL_8N1, 16, 17); 

  // Webserver Routes
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/cancel", handleCancel);
  server.begin();

  setupTime = millis();
  tft.fillScreen(ST77XX_BLACK);
}

/**************** MAIN LOOP ****************/
void loop() {
  pox.update(); 
  server.handleClient();

  heartRate = pox.getHeartRate();
  SpO2 = pox.getSpO2();

  while (GPS_Serial.available() > 0) {
    gps.encode(GPS_Serial.read());
    if (gps.location.isUpdated()) {
      currentLat = gps.location.lat();
      currentLng = gps.location.lng();
    }
  }

  checkAlarms();
  checkSMSLogic(); 

  if (millis() - lastScreenUpdate > SCREEN_INTERVAL) {
    updateTFTDisplay();
    lastScreenUpdate = millis();
  }
}

/**************** ALARM LOGIC ****************/
void checkAlarms() {
  alarm1Active = (heartRate > HR_HIGH_THRESHOLD_1 && !alarm1Canceled);
  alarm2Active = (heartRate > HR_HIGH_THRESHOLD_2 && !alarm2Canceled);

  if (heartRate < HR_HIGH_THRESHOLD_1) alarm1Canceled = false;
  if (heartRate < HR_HIGH_THRESHOLD_2) {
    alarm2Canceled = false;
    if (!lhrAlarmActive) smsSentForThisEvent = false; 
  }

  const unsigned long LHR_DEBOUNCE_MS = 10000; 
  static unsigned long lhrTimerStart = 0;
  static float prevHeartRate = 0;

  if (heartRate > LHR_THRESHOLD) sensorStabilized = true;

  bool lhrThresholdActive = (sensorStabilized && heartRate > 0 && heartRate < LHR_THRESHOLD);

  if (lhrThresholdActive && prevHeartRate >= LHR_THRESHOLD) lhrTimerStart = millis(); 

  if (lhrThresholdActive) {
    bool isIncreasing = (heartRate > prevHeartRate + 2.0);
    bool debouncePassed = (millis() - lhrTimerStart >= LHR_DEBOUNCE_MS);
    lhrAlarmActive = (debouncePassed && !isIncreasing && !lhrAlarmCanceled);
  } else {
    lhrAlarmActive = false;
    if (!alarm2Active) smsSentForThisEvent = false;
  }
  
  if (heartRate >= LHR_THRESHOLD) lhrAlarmCanceled = false;
  prevHeartRate = heartRate;
}

/**************** SMS LOGIC ****************/
void checkSMSLogic() {
  bool criticalCondition = false;
  int type = 0;

  if (alarm2Active) {
    criticalCondition = true;
    type = 1; 
  } else if (lhrAlarmActive) {
    criticalCondition = true;
    type = 2; 
  }

  if (criticalCondition && !smsPending && !smsSentForThisEvent) {
    smsPending = true;
    smsTimerStart = millis();
    pendingSmsType = type;
  }

  if (!criticalCondition && smsPending) smsPending = false;

  if (smsPending) {
    unsigned long elapsed = millis() - smsTimerStart;
    if (elapsed > SMS_ABORT_WINDOW) {
      String conditionName = (pendingSmsType == 1) ? "TACHYCARDIA (High HR)" : "BRADYCARDIA (Low HR)";
      String mapLink = "https://maps.google.com/?q=" + String(currentLat, 6) + "," + String(currentLng, 6);
      
      String msgBody = "EMERGENCY ALERT!\nCondition: " + conditionName + "\n";
      msgBody += "HR: " + String(heartRate, 1) + " BPM\n";
      msgBody += "Location: " + mapLink;

      for(int i=0; i<NUM_CONTACTS; i++) {
        if(String(contacts[i]) != "+910000000000") sendTwilioSMS(contacts[i], msgBody);
      }
      smsPending = false;
      smsSentForThisEvent = true; 
    }
  }
}

/**************** UPDATE TFT ****************/
void updateTFTDisplay() {
  tft.fillScreen(ST77XX_BLACK); 
  
  tft.setCursor(5, 5);
  if (smsPending) {
    tft.setTextColor(ST77XX_MAGENTA);
    tft.setTextSize(2);
    tft.print("SMS IN ");
    tft.print((SMS_ABORT_WINDOW - (millis() - smsTimerStart))/1000);
    tft.print("s !!");
  } else if (lhrAlarmActive) {
    tft.setTextColor(ST77XX_MAGENTA);
    tft.setTextSize(1);
    tft.print("BRADYCARDIA ALERT!");
  } else if (alarm2Active) {
    tft.setTextColor(ST77XX_RED);
    tft.setTextSize(1);
    tft.print("TACHYCARDIA ALERT!");
  } else {
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    tft.print("Status: OK");
  }

  tft.setCursor(5, 30);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);
  tft.print("HR: ");
  if (heartRate > 0) tft.print(heartRate, 0); else tft.print("--");
  tft.print("bpm");
  
  tft.setCursor(5, 60);
  tft.setTextColor(ST77XX_GREEN);
  tft.print("SpO2: ");
  if (SpO2 > 0) tft.print(SpO2, 0); else tft.print("--");
  tft.print("%");
}

/**************** WEBPAGE + GRAPH ****************/
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>Health Monitor</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
body { font-family: Arial; padding: 10px; text-align:center; background:#f4f4f4; }
.container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; }
.card { border: 2px solid #ccc; padding: 15px; border-radius: 8px; margin: 10px 5px; display:inline-block; width: 40%; font-size:20px; font-weight:bold; }
.btn-sound { background: #28a745; color: white; border: none; padding: 10px 20px; font-size: 16px; border-radius: 5px; margin-bottom: 20px; cursor: pointer; }
.btn-cancel { background: #d9534f; color: white; border: none; padding: 15px 30px; font-size: 18px; border-radius: 5px; width: 100%; margin-top:10px; cursor: pointer; }
.alert-box { background-color: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; padding: 20px; border-radius: 5px; margin-top: 20px; display: none; }
canvas { background: #fff; margin-top: 20px; border: 1px solid #ddd; border-radius: 5px; }
</style>
</head>
<body>

<div class="container">
  <h2>Live Health Monitor</h2>
  <button onclick="enableAudio()" class="btn-sound">Enable Sound</button>

  <div>
    <div class="card" style="border-color:red; color:red;">
      HR: <span id="hr">--</span> <span style="font-size:14px">BPM</span>
    </div>
    <div class="card" style="border-color:green; color:green;">
      SpO2: <span id="spo2">--</span> <span style="font-size:14px">%</span>
    </div>
  </div>

  <div>
    <canvas id="hrChart"></canvas>
  </div>

  <div id="alarmBox" class="alert-box">
    <h3 id="alarmTitle">CRITICAL ALERT!</h3>
    <p>SMS Sending in: <span id="smsTimer" style="font-weight:bold; color:red; font-size:20px;">--</span> s</p>
    <button class="btn-cancel" onclick="cancelAll()">ABORT SMS & ALARM</button>
  </div>

  <div style="margin-top:20px;">
    Location: <a id="gpsLink" href="#" target="_blank">Waiting for GPS...</a>
  </div>
</div>

<audio id="alarmSound" loop preload="auto">
  <source src="https://actions.google.com/sounds/v1/alarms/alarm_clock.ogg" type="audio/ogg">
</audio>

<script>
let audioEnabled = false;
let soundPlaying = false;

const ctx = document.getElementById('hrChart').getContext('2d');
const hrChart = new Chart(ctx, {
    type: 'line',
    data: {
        labels: [], 
        datasets: [{
            label: 'Heart Rate Readings',
            data: [], 
            borderColor: 'red',
            backgroundColor: 'rgba(255, 0, 0, 0.1)',
            borderWidth: 2,
            tension: 0.3, 
            pointRadius: 3,
            fill: true
        }]
    },
    options: {
        responsive: true,
        scales: {
            x: { 
                title: { display: true, text: 'Time' },
                ticks: { maxTicksLimit: 5 } 
            },
            y: { 
                title: { display: true, text: 'BPM' }, 
                min: 0, 
                max: 250,
                ticks:{
                  stepSize:25
                },
            }
        }
    }
});

function updateGraph(time, hrValue) {
    hrChart.data.labels.push(time);
    hrChart.data.datasets[0].data.push(hrValue);
    if (hrChart.data.labels.length > 20) {
        hrChart.data.labels.shift();
        hrChart.data.datasets[0].data.shift();
    }
    hrChart.update();
}

function enableAudio() {
  document.getElementById('alarmSound').play().catch(()=>{});
  document.getElementById('alarmSound').pause();
  audioEnabled = true;
  alert("Sound Enabled!");
}

function cancelAll() {
  fetch('/cancel?alarm=2');
  fetch('/cancel?alarm=3');
  if(soundPlaying) {
      document.getElementById('alarmSound').pause();
      document.getElementById('alarmSound').currentTime = 0;
      soundPlaying = false;
  }
  document.getElementById('alarmBox').style.display = 'none';
  alert("Alarms Canceled!");
}

function updateData() {
  fetch('/data').then(r => r.json()).then(d => {
    document.getElementById('hr').innerText = d.hr.toFixed(1);
    document.getElementById('spo2').innerText = d.spo2.toFixed(1);

    if(d.hr > 40) { 
        let now = new Date();
        let timeStr = now.getHours() + ":" + now.getMinutes() + ":" + now.getSeconds();
        updateGraph(timeStr, d.hr);
    }

    if(d.lat != 0) {
      document.getElementById('gpsLink').href = "https://maps.google.com/?q=" + d.lat + "," + d.lng;
      document.getElementById('gpsLink').innerText = "Open Maps";
    }

    let box = document.getElementById('alarmBox');
    
    if (d.smsPending) {
       box.style.display = 'block';
       document.getElementById('smsTimer').innerText = "7";
       document.getElementById('alarmTitle').innerText = (d.type == 1) ? "TACHYCARDIA ALERT" : "BRADYCARDIA ALERT";

       if(audioEnabled && !soundPlaying) {
         document.getElementById('alarmSound').play();
         soundPlaying = true;
       }
    } else {
       box.style.display = 'none';
       if(soundPlaying) {
         document.getElementById('alarmSound').pause();
         document.getElementById('alarmSound').currentTime = 0;
         soundPlaying = false;
       }
    }
  });
}
setInterval(updateData, 1000);
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleData() {
  String json = "{";
  json += "\"hr\":" + String(heartRate) + ",";
  json += "\"spo2\":" + String(SpO2) + ",";
  json += "\"lat\":" + String(currentLat, 6) + ",";
  json += "\"lng\":" + String(currentLng, 6) + ",";
  json += "\"smsPending\":" + String(smsPending ? "true" : "false") + ",";
  json += "\"type\":" + String(pendingSmsType); 
  json += "}";
  server.send(200, "application/json", json);
}

void handleCancel() {
  if (server.hasArg("alarm")) {
    int a = server.arg("alarm").toInt();
    if (a == 1) alarm1Canceled = true;
    if (a == 2) alarm2Canceled = true; 
    if (a == 3) lhrAlarmCanceled = true;
    
    if (a == 2 || a == 3) {
      smsPending = false;
      Serial.println("User Canceled -> SMS ABORTED");
    }
  }
  server.send(200, "text/plain", "OK");
}

/**************** TWILIO SMS ****************/
bool sendTwilioSMS(String toPhone, String message) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure(); 
  HTTPClient https;
  String url = "https://api.twilio.com/2010-04-01/Accounts/" + String(accountSID) + "/Messages.json";
  
  if (https.begin(client, url)) {
    https.setAuthorization(accountSID, authToken);
    https.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String payload = "To=" + urlEncode(toPhone) + "&From=" + urlEncode(twilioFrom) + "&Body=" + urlEncode(message);
    int httpCode = https.POST(payload);
    https.end();
    return (httpCode == 201);
  }
  return false;
}

String urlEncode(const String &str) {
  String encoded = "";
  char c;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < str.length(); i++) {
    c = str[i];
    if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9')) encoded += c;
    else if (c == ' ') encoded += '+';
    else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0xF];
      encoded += hex[c & 0xF];
    }
  }
  return encoded;
}