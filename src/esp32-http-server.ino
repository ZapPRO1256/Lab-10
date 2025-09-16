#include <WiFi.h>
#include <WebServer.h>
#include "DHTesp.h"
#include "RTClib.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>

// ---------------- Wi-Fi ----------------
const char* SSID = "Wokwi-GUEST";
const char* password = "";
const char* openaiApiKey = "b453666eb3d04c1fa7c608375d88f7fc";

// ---------------- WebServer ----------------
WebServer server(80);
String dataValue = "None";
String aiForecast = "No forecast yet";

// ---------------- RTC ----------------
RTC_DS1307 rtc;

// ---------------- DHT22 ----------------
#define DHT_PIN 15
DHTesp dhtSensor;

// ---------------- Soil & pH ----------------
#define SOIL_PIN 34
#define PH_PIN   35

// ---------------- LCD ----------------
LiquidCrystal_I2C LCD(0x27, 16, 2);

// ---------------- Data Buffers ----------------
#define MAX_POINTS 50
float tempHistory[MAX_POINTS];
float humHistory[MAX_POINTS];
int soilHistory[MAX_POINTS];
float phHistory[MAX_POINTS];
int historyIndex = 0;

// ---------------- Handlers ----------------
void handleRoot() {
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <meta charset="utf-8">
    <title>Agro Monitor</title>
    <script type="text/javascript" src="https://www.gstatic.com/charts/loader.js"></script>
    <script>
      google.charts.load('current', {'packages':['corechart']});
      google.charts.setOnLoadCallback(drawCharts);

      function drawCharts() {
        fetch('/data').then(r=>r.json()).then(d=>{
          let tempData = new google.visualization.DataTable();
          tempData.addColumn('string','Time');
          tempData.addColumn('number','Temp');
          tempData.addRows(d.history.map(x=>[x.time, x.temp]));

          let humData = new google.visualization.DataTable();
          humData.addColumn('string','Time');
          humData.addColumn('number','Humidity');
          humData.addRows(d.history.map(x=>[x.time, x.hum]));

          let soilData = new google.visualization.DataTable();
          soilData.addColumn('string','Time');
          soilData.addColumn('number','Soil %');
          soilData.addRows(d.history.map(x=>[x.time, x.soil]));

          let phData = new google.visualization.DataTable();
          phData.addColumn('string','Time');
          phData.addColumn('number','pH');
          phData.addRows(d.history.map(x=>[x.time, x.ph]));

          let opts={width:400,height:200};

          new google.visualization.LineChart(document.getElementById('temp')).draw(tempData,opts);
          new google.visualization.LineChart(document.getElementById('hum')).draw(humData,opts);
          new google.visualization.LineChart(document.getElementById('soil')).draw(soilData,opts);
          new google.visualization.LineChart(document.getElementById('ph')).draw(phData,opts);

          document.getElementById("forecast").innerText = d.forecast;
        });
      }

      function irrigate(){
        fetch('/irrigate').then(r=>r.text()).then(t=>alert(t));
      }

      setInterval(drawCharts,5000);
    </script>
  </head>
  <body>
    <h2>🌱 Agro Monitor</h2>
    <div id="temp"></div>
    <div id="hum"></div>
    <div id="soil"></div>
    <div id="ph"></div>
    <h3>AI Forecast:</h3>
    <p id="forecast">Loading...</p>
    <button onclick="irrigate()">💧 Імітація поливу</button>
  </body>
  </html>
  )rawliteral";

  server.send(200, "text/html", html);
}

void handleData() {
  DynamicJsonDocument doc(2048);
  JsonArray hist = doc.createNestedArray("history");

  for(int i=0;i<MAX_POINTS;i++){
    int idx=(historyIndex+i)%MAX_POINTS;
    if(tempHistory[idx]==0 && humHistory[idx]==0) continue;

    JsonObject obj=hist.createNestedObject();
    obj["time"]=String(idx);
    obj["temp"]=tempHistory[idx];
    obj["hum"]=humHistory[idx];
    obj["soil"]=soilHistory[idx];
    obj["ph"]=phHistory[idx];
  }
  doc["forecast"]=aiForecast;

  String out;
  serializeJson(doc,out);
  server.send(200,"application/json",out);
}

void handleIrrigate(){
  soilHistory[(historyIndex-1+MAX_POINTS)%MAX_POINTS]+=10;
  if(soilHistory[(historyIndex-1+MAX_POINTS)%MAX_POINTS]>100)
    soilHistory[(historyIndex-1+MAX_POINTS)%MAX_POINTS]=100;
  server.send(200,"text/plain","Полив виконано (імітація)");
}

// ---------------- GPT Request ----------------
void request_gpt(String prompt) {
  DynamicJsonDocument jsonDocument(1024);
  jsonDocument["model"] = "gpt-3.5-turbo";
  JsonArray messages = jsonDocument.createNestedArray("messages");

  JsonObject sys = messages.createNestedObject();
  sys["role"] = "system";
  sys["content"] = "You are an AI agro assistant. Answer shortly.";

  JsonObject usr = messages.createNestedObject();
  usr["role"] = "user";
  usr["content"] = prompt;

  HTTPClient http;
  String apiUrl = "https://artificialintelligence.openai.azure.com/openai/deployments/test/chat/completions?api-version=2023-05-15";
  http.begin(apiUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("api-key", openaiApiKey);

  String body;
  serializeJson(jsonDocument, body);

  int httpCode = http.POST(body);
  if (httpCode == 200) {
    String response = http.getString();
    deserializeJson(jsonDocument, response);
    String content = jsonDocument["choices"][0]["message"]["content"].as<String>();
    aiForecast = content;
    Serial.println("AI Response: " + content);
  } else {
    Serial.println("HTTP error: " + String(httpCode));
  }
  http.end();
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  pinMode(SOIL_PIN, INPUT);
  pinMode(PH_PIN, INPUT);

  WiFi.begin(SSID, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/irrigate", handleIrrigate);
  server.begin();

  dhtSensor.setup(DHT_PIN, DHTesp::DHT22);
  rtc.begin();
  LCD.init();
  LCD.backlight();
}

// ---------------- Loop ----------------
int counter = 0;
void loop() {
  server.handleClient();

  TempAndHumidity data = dhtSensor.getTempAndHumidity();
  float temp = data.temperature;
  float hum  = data.humidity;

  int soilRaw = analogRead(SOIL_PIN);
  int soilPercent = map(soilRaw, 0, 4095, 100, 0);

  int phRaw = analogRead(PH_PIN);
  float voltage = phRaw * (3.3 / 4095.0);
  float phValue = 7 + ((2.5 - voltage) / 0.18);

  DateTime now = rtc.now();
  String timeStr = String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second());

  dataValue = "Time: " + timeStr + "\nTemp: " + String(temp,1) + " C\nHumidity: " + String(hum,1) + " %\nSoil: " + String(soilPercent) + " %\npH: " + String(phValue,2);

  Serial.println(dataValue);

  LCD.clear();
  LCD.setCursor(0, 0);
  LCD.print("T:" + String(temp,1) + " H:" + String(hum,1));
  LCD.setCursor(0, 1);
  LCD.print("Soil:" + String(soilPercent) + "% pH:" + String(phValue,1));

  // запис у історію
  tempHistory[historyIndex]=temp;
  humHistory[historyIndex]=hum;
  soilHistory[historyIndex]=soilPercent;
  phHistory[historyIndex]=phValue;
  historyIndex=(historyIndex+1)%MAX_POINTS;

  delay(1000);

  if (counter == 0) {
    request_gpt("Based on this data, when should irrigation be done?\n" + dataValue);
    counter++;
  }
}
