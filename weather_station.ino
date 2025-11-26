#include <WiFi.h>
#include <WebServer.h>
#include <GyverBME280.h>
#include <vector>
#include <cmath>
#include <time.h>

GyverBME280 bme;

// Настройки Wi-Fi
const char* ssid = "xr5";
const char* password = "20052005";

WebServer server(80);

// Настройки NTP (Network Time Protocol)
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600; // GMT+3 (Москва)
const int daylightOffset_sec = 0;

// Структура для хранения данных с временной меткой
struct WeatherData {
  time_t timestamp;        // Временная метка в Unix time
  float temperature;       // Температура в °C
  float pressure;          // Давление в мм рт.ст.
  float rainProbability;   // Вероятность осадков в %
  String forecast;         // Текстовый прогноз
};

// Структура для анализа тенденций
struct WeatherTrend {
  float pressureChange;    // Изменение давления за час (мм рт.ст./час)
  float tempChange;        // Изменение температуры за час (°C/час)
  float pressureStability; // Стабильность давления (стандартное отклонение)
  int prediction;          // Предсказание: 0-улучшение, 1-стабильно, 2-ухудшение
  String predictionText;   // Текстовое предсказание
};

// Вектор для хранения исторических данных
std::vector<WeatherData> weatherHistory;

// Текущие данные
float temperature;
float pressure;
float rainProbability;
WeatherTrend currentTrend;

// Настройки сбора данных
const unsigned long COLLECTION_INTERVAL = 5 * 60 * 1000; // 5 минут в миллисекундах
const unsigned long HISTORY_DURATION = 6 * 60 * 60; // 6 часов в секундах (Unix time)
const unsigned long TREND_INTERVAL = 60 * 60; // 1 час в секундах

// Прототипы функций
bool syncTime();
time_t getCurrentTime();
String formatTime(time_t timestamp);
String formatTimeShort(time_t timestamp);
String getTimeAgo(time_t timestamp);
void updateSensorData();
void addToHistory();
void cleanOldData();
void analyzeTrends();
void predictWeather();
float calculateRainProbability(float temp, float press);
String getForecastText();
void handleRoot();
void handleData();
void handleHistory();
void handleCharts();
void handleChartData();
void handlePrediction();
void handleCSS();

void setup() {
  Serial.begin(115200);
  
  // Запуск BME280
  if (!bme.begin(0x76)) {
    Serial.println("BME280 Error!");
    while(1);
  }
  
  // Подключение к Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  // Ожидание подключения
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
    // Синхронизация времени через NTP
    if (syncTime()) {
      Serial.println("Time synchronized successfully");
    } else {
      Serial.println("Failed to synchronize time");
    }
  } else {
    Serial.println("\nFailed to connect to WiFi!");
    Serial.println("Starting AP mode as fallback...");
    WiFi.softAP("ESP32-Weather-Fallback", "12345678");
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
  }
  
  // Настройка маршрутов сервера
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/history", handleHistory);
  server.on("/charts", handleCharts);
  server.on("/chart-data", handleChartData);
  server.on("/prediction", handlePrediction);
  server.on("/style.css", handleCSS);
  
  server.begin();
  Serial.println("HTTP server started");
  
  // Первоначальный сбор данных
  updateSensorData();
  addToHistory();
}

void loop() {
  server.handleClient();
  
  // Проверка подключения Wi-Fi и синхронизации времени
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Потеряно соединение Wi-Fi, переподключение...");
    WiFi.reconnect();
    delay(5000);
  }
  
  // Обновление данных каждые 5 минут
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate >= COLLECTION_INTERVAL) {
    updateSensorData();
    addToHistory();
    cleanOldData();
    analyzeTrends();
    lastUpdate = millis();
    
    Serial.println("Данные обновлены. Записей в истории: " + String(weatherHistory.size()));
    if (weatherHistory.size() > 0) {
      Serial.println("Последняя запись: " + formatTime(weatherHistory.back().timestamp));
    }
    Serial.println("Предсказание: " + currentTrend.predictionText);
  }
}

// Синхронизация времени через NTP
bool syncTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Ждем синхронизации времени
  int attempts = 0;
  while (time(nullptr) < 1000000000 && attempts < 10) {
    Serial.print(".");
    delay(1000);
    attempts++;
  }
  
  return time(nullptr) > 1000000000;
}

// Получение текущего времени
time_t getCurrentTime() {
  return time(nullptr);
}

// Форматирование времени для отображения
String formatTime(time_t timestamp) {
  struct tm timeinfo;
  localtime_r(&timestamp, &timeinfo);
  
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%H:%M %d.%m.%Y", &timeinfo);
  return String(buffer);
}

// Короткое форматирование времени (только часы:минуты)
String formatTimeShort(time_t timestamp) {
  struct tm timeinfo;
  localtime_r(&timestamp, &timeinfo);
  
  char buffer[6];
  strftime(buffer, sizeof(buffer), "%H:%M", &timeinfo);
  return String(buffer);
}

// Время назад в читаемом формате
String getTimeAgo(time_t timestamp) {
  time_t now = getCurrentTime();
  time_t diff = now - timestamp;
  
  if (diff < 60) return "только что";
  else if (diff < 3600) return String(diff / 60) + " мин назад";
  else if (diff < 86400) return String(diff / 3600) + " ч назад";
  else return String(diff / 86400) + " дн назад";
}

void updateSensorData() {
  float newTemp = bme.readTemperature();
  float newPress = bme.readPressure() / 133.3; // в мм рт.ст.
  
  // Проверка на корректность данных датчика
  if (isnan(newTemp) || isnan(newPress) || newTemp < -50 || newTemp > 100 || newPress < 500 || newPress > 1100) {
    Serial.println("Ошибка данных датчика, используем предыдущие значения");
    return; // Не обновляем данные при ошибке
  }
  
  temperature = newTemp;
  pressure = newPress;
  rainProbability = calculateRainProbability(temperature, pressure);
}

void addToHistory() {
  WeatherData newData;
  newData.timestamp = getCurrentTime(); // Используем реальное время
  newData.temperature = temperature;
  newData.pressure = pressure;
  newData.rainProbability = rainProbability;
  newData.forecast = getForecastText();
  
  weatherHistory.push_back(newData);
  
  Serial.println("Добавлена запись в историю. Время: " + formatTime(newData.timestamp));
}

// Очистка старых данных
void cleanOldData() {
  time_t currentTime = getCurrentTime();
  time_t cutoffTime = currentTime - HISTORY_DURATION;
  
  size_t removedCount = 0;
  for (auto it = weatherHistory.begin(); it != weatherHistory.end(); ) {
    if (it->timestamp < cutoffTime) {
      it = weatherHistory.erase(it);
      removedCount++;
    } else {
      ++it;
    }
  }
  if (removedCount > 0) {
    Serial.println("Удалено устаревших записей: " + String(removedCount));
  }
}

void analyzeTrends() {
  if (weatherHistory.size() < 3) {
    // Недостаточно данных для анализа
    currentTrend.pressureChange = 0;
    currentTrend.tempChange = 0;
    currentTrend.pressureStability = 0;
    currentTrend.prediction = 1; // Стабильно
    currentTrend.predictionText = "Недостаточно данных для анализа (" + String(weatherHistory.size()) + " записей)";
    return;
  }
  
  // Анализ барической тенденции за последний час
  time_t currentTime = getCurrentTime();
  time_t oneHourAgo = currentTime - TREND_INTERVAL;
  
  // Находим данные за последний час
  std::vector<WeatherData> recentData;
  for (const auto& data : weatherHistory) {
    if (data.timestamp >= oneHourAgo) {
      recentData.push_back(data);
    }
  }
  
  if (recentData.size() < 2) {
    currentTrend.pressureChange = 0;
    currentTrend.tempChange = 0;
    currentTrend.pressureStability = 0;
    currentTrend.prediction = 1;
    currentTrend.predictionText = "Недостаточно данных за последний час (" + String(recentData.size()) + " записей)";
    return;
  }
  
  // Рассчитываем изменение давления и температуры
  float oldestPressure = recentData.front().pressure;
  float newestPressure = recentData.back().pressure;
  float oldestTemp = recentData.front().temperature;
  float newestTemp = recentData.back().temperature;
  
  // Изменение за час (мм рт.ст./час)
  float timeDiffHours = (recentData.back().timestamp - recentData.front().timestamp) / 3600.0;
  if (timeDiffHours > 0) {
    currentTrend.pressureChange = (newestPressure - oldestPressure) / timeDiffHours;
    currentTrend.tempChange = (newestTemp - oldestTemp) / timeDiffHours;
  } else {
    currentTrend.pressureChange = 0;
    currentTrend.tempChange = 0;
  }
  
  // Рассчитываем стабильность давления (стандартное отклонение)
  float pressureSum = 0;
  for (const auto& data : recentData) {
    pressureSum += data.pressure;
  }
  float pressureMean = pressureSum / recentData.size();
  
  float variance = 0;
  for (const auto& data : recentData) {
    variance += pow(data.pressure - pressureMean, 2);
  }
  currentTrend.pressureStability = sqrt(variance / recentData.size());
  
  // Прогнозирование погоды на основе правил
  predictWeather();
}

void predictWeather() {
  // Правило 1: Барические тенденции
  if (currentTrend.pressureChange < -1.5) {
    // Быстрое падение давления (>1.5 мм рт.ст./час)
    currentTrend.prediction = 2; // Ухудшение
    currentTrend.predictionText = "⚠️ Быстрое падение давления - ожидаются осадки";
  }
  else if (currentTrend.pressureChange < -0.5) {
    // Медленное падение давления
    if (currentTrend.tempChange < -1.0) {
      // Падение температуры + падение давления
      currentTrend.prediction = 2;
      currentTrend.predictionText = "🌧️ Холодный фронт - осадки и похолодание";
    }
    else if (currentTrend.tempChange > 1.0) {
      // Рост температуры + падение давления
      currentTrend.prediction = 2;
      currentTrend.predictionText = "🌧️ Теплый фронт - осадки и потепление";
    }
    else {
      currentTrend.prediction = 2;
      currentTrend.predictionText = "🌧️ Падение давления - возможны осадки";
    }
  }
  else if (currentTrend.pressureChange > 0.5) {
    // Рост давления
    currentTrend.prediction = 0; // Улучшение
    currentTrend.predictionText = "☀️ Рост давления - улучшение погоды";
  }
  else if (currentTrend.pressureStability > 0.8) {
    // Резкие колебания давления
    currentTrend.prediction = 2;
    currentTrend.predictionText = "🌤️ Неустойчивое давление - переменная облачность";
  }
  else {
    // Стабильное давление
    if (abs(currentTrend.tempChange) < 0.5) {
      // Стабильная температура
      currentTrend.prediction = 0;
      currentTrend.predictionText = "☀️ Стабильные условия - ясная погода";
    }
    else if (currentTrend.tempChange < -0.5) {
      currentTrend.prediction = 1;
      currentTrend.predictionText = "⛅ Похолодание - переменная облачность";
    }
    else {
      currentTrend.prediction = 1;
      currentTrend.predictionText = "⛅ Потепление - переменная облачность";
    }
  }
  
  // Дополнительная логика для экстремальных случаев
  if (currentTrend.pressureChange < -2.5) {
    currentTrend.predictionText = "⛈️ Резкое падение давления - сильные осадки!";
  }
  else if (currentTrend.pressureChange > 2.0) {
    currentTrend.predictionText = "☀️ Быстрый рост давления - отличная погода!";
  }
}

float calculateRainProbability(float temp, float press) {
  float basePressure = 760.0;
  float pressureDiff = basePressure - press;
  float tempFactor = 0.0;
  
  if (temp < 5) tempFactor = 0.3;
  else if (temp < 15) tempFactor = 0.1;
  else tempFactor = 0.0;
  
  float pressureFactor = 0.0;
  if (pressureDiff > 20) pressureFactor = 0.7;
  else if (pressureDiff > 10) pressureFactor = 0.4;
  else if (pressureDiff > 5) pressureFactor = 0.2;
  else if (pressureDiff < -10) pressureFactor = 0.1;
  
  float probability = (pressureFactor + tempFactor) * 100;
  probability = constrain(probability, 0, 95);
  
  return probability;
}

String getForecastText() {
  if (rainProbability < 20) {
    return "☀️ Ясно, осадков не ожидается";
  } else if (rainProbability < 40) {
    return "⛅ Малооблачно, небольшой шанс осадков";
  } else if (rainProbability < 60) {
    return "🌤️ Переменная облачность, возможны осадки";
  } else if (rainProbability < 80) {
    return "🌧️ Облачно, высокая вероятность дождя";
  } else {
    return "⛈️ Сильные осадки очень вероятны!";
  }
}

void handleRoot() {
  String ipAddress = WiFi.localIP().toString();
  if (ipAddress == "0.0.0.0") {
    ipAddress = WiFi.softAPIP().toString();
  }
  
  String currentTime = formatTime(getCurrentTime());
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Погодная станция ESP32</title>
    <link rel="stylesheet" href="/style.css">
    <meta charset="UTF-8">
</head>
<body>
    <div class="container">
        <h1>🌤️ Погодная станция</h1>
        
        <div class="connection-info">
            <p><strong>Режим:</strong> )rawliteral" + 
            String(WiFi.status() == WL_CONNECTED ? "Подключено к Wi-Fi" : "Точка доступа") + 
            R"rawliteral(</p>
            <p><strong>IP:</strong> )rawliteral" + ipAddress + R"rawliteral(</p>
            <p><strong>Текущее время:</strong> )rawliteral" + currentTime + R"rawliteral(</p>
            <p><strong>История:</strong> )rawliteral" + String(weatherHistory.size()) + R"rawliteral( записей (6 часов)</p>
        </div>
        
        <div class="sensor-data">
            <div class="data-item">
                <span class="label">Температура:</span>
                <span class="value" id="temp">)rawliteral" + String(temperature, 1) + R"rawliteral(°C</span>
            </div>
            
            <div class="data-item">
                <span class="label">Давление:</span>
                <span class="value" id="press">)rawliteral" + String(pressure, 1) + R"rawliteral( мм рт.ст.</span>
            </div>
            
            <div class="data-item">
                <span class="label">Вероятность осадков:</span>
                <span class="value" id="rain">)rawliteral" + String(rainProbability, 0) + R"rawliteral(%</span>
            </div>
        </div>
        
        <div class="prediction-card">
            <h2>🎯 Прогноз на ближайшие часы</h2>
            <div class="prediction-text" id="prediction-text">)rawliteral" + currentTrend.predictionText + R"rawliteral(</div>
            <div class="trend-info">
                <p><strong>Изменение давления:</strong> <span id="pressure-change">)rawliteral" + String(currentTrend.pressureChange, 1) + R"rawliteral( мм рт.ст./час</span></p>
                <p><strong>Изменение температуры:</strong> <span id="temp-change">)rawliteral" + String(currentTrend.tempChange, 1) + R"rawliteral( °C/час</span></p>
                <p><strong>Стабильность давления:</strong> <span id="pressure-stability">)rawliteral" + String(currentTrend.pressureStability, 2) + R"rawliteral(</span></p>
            </div>
        </div>
        
        <div class="forecast">
            <h2>Текущий прогноз:</h2>
            <div id="forecast-text">)rawliteral" + getForecastText() + R"rawliteral(</div>
        </div>
        
        <div class="navigation-links">
            <a href="/history" class="nav-link">📊 Просмотреть историю данных</a>
            <a href="/charts" class="nav-link">📈 Графики температуры и давления</a>
            <a href="/prediction" class="nav-link">🔍 Детальный анализ тенденций</a>
        </div>
        
        <div class="update-time">
            Последнее обновление: <span id="time">)rawliteral" + getTimeAgo(weatherHistory.size() > 0 ? weatherHistory.back().timestamp : getCurrentTime()) + R"rawliteral(</span><br>
            Следующее обновление через: <span id="next-update">5 минут</span>
        </div>
    </div>
    
    <script>
        // Автообновление текущих данных каждые 30 секунд
        setInterval(updateData, 30000);
        
        function updateData() {
            fetch('/data')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('temp').textContent = data.temperature + '°C';
                    document.getElementById('press').textContent = data.pressure + ' мм рт.ст.';
                    document.getElementById('rain').textContent = data.rainProbability + '%';
                    document.getElementById('forecast-text').textContent = data.forecast;
                    document.getElementById('time').textContent = data.time;
                    document.getElementById('prediction-text').textContent = data.predictionText;
                    document.getElementById('pressure-change').textContent = data.pressureChange + ' мм рт.ст./час';
                    document.getElementById('temp-change').textContent = data.tempChange + ' °C/час';
                    document.getElementById('pressure-stability').textContent = data.pressureStability;
                    
                    // Изменение цвета предсказания в зависимости от типа
                    const predictionElement = document.getElementById('prediction-text');
                    predictionElement.className = 'prediction-text prediction-' + data.predictionType;
                })
                .catch(error => console.log('Error updating data:', error));
        }
    </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleData() {
  String json = "{";
  json += "\"temperature\":" + String(temperature, 1) + ",";
  json += "\"pressure\":" + String(pressure, 1) + ",";
  json += "\"rainProbability\":" + String(rainProbability, 0) + ",";
  json += "\"forecast\":\"" + getForecastText() + "\",";
  json += "\"predictionText\":\"" + currentTrend.predictionText + "\",";
  json += "\"pressureChange\":" + String(currentTrend.pressureChange, 1) + ",";
  json += "\"tempChange\":" + String(currentTrend.tempChange, 1) + ",";
  json += "\"pressureStability\":" + String(currentTrend.pressureStability, 2) + ",";
  json += "\"predictionType\":" + String(currentTrend.prediction) + ",";
  json += "\"time\":\"" + getTimeAgo(weatherHistory.size() > 0 ? weatherHistory.back().timestamp : getCurrentTime()) + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

void handlePrediction() {
  // Создаем HTML для страницы предсказания
  String trendClass = "trend-stable";
  if (currentTrend.pressureChange > 0) trendClass = "trend-up";
  else if (currentTrend.pressureChange < 0) trendClass = "trend-down";
  
  String tempTrendClass = "trend-stable";
  if (currentTrend.tempChange > 0) tempTrendClass = "trend-up";
  else if (currentTrend.tempChange < 0) tempTrendClass = "trend-down";
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<title>Анализ погодных тенденций</title>";
  html += "<link rel=\"stylesheet\" href=\"/style.css\">";
  html += "<meta charset=\"UTF-8\">";
  html += "<style>";
  html += ".analysis-card { background: white; border-radius: 10px; padding: 20px; margin: 15px 0; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += ".rule-item { margin: 10px 0; padding: 10px; border-left: 4px solid #74b9ff; background: #f8f9fa; }";
  html += ".trend-indicator { display: inline-block; padding: 5px 10px; border-radius: 15px; color: white; font-weight: bold; margin: 0 5px; }";
  html += ".trend-up { background: #00b894; }";
  html += ".trend-down { background: #e17055; }";
  html += ".trend-stable { background: #636e72; }";
  html += "</style></head><body>";
  html += "<div class=\"container\">";
  html += "<h1>🔍 Анализ погодных тенденций</h1>";
  html += "<div class=\"navigation-links\">";
  html += "<a href=\"/\" class=\"nav-link\">← Назад к текущим данным</a>";
  html += "<a href=\"/history\" class=\"nav-link\">📊 История данных</a>";
  html += "<a href=\"/charts\" class=\"nav-link\">📈 Графики</a>";
  html += "</div>";
  html += "<div class=\"analysis-card\">";
  html += "<h2>📊 Текущие тенденции</h2>";
  html += "<div class=\"trend-info\">";
  html += "<p><strong>Изменение давления:</strong> ";
  html += "<span class=\"trend-indicator " + trendClass + "\">" + String(currentTrend.pressureChange, 1) + " мм рт.ст./час</span></p>";
  html += "<p><strong>Изменение температуры:</strong> ";
  html += "<span class=\"trend-indicator " + tempTrendClass + "\">" + String(currentTrend.tempChange, 1) + " °C/час</span></p>";
  html += "<p><strong>Стабильность давления:</strong> " + String(currentTrend.pressureStability, 2) + "</p>";
  html += "</div></div>";
  html += "<div class=\"analysis-card\">";
  html += "<h2>🎯 Прогноз погоды</h2>";
  html += "<div class=\"prediction-text prediction-" + String(currentTrend.prediction) + "\">";
  html += currentTrend.predictionText;
  html += "</div></div>";
  html += "<div class=\"analysis-card\">";
  html += "<h2>📖 Правила прогнозирования</h2>";
  html += "<div class=\"rule-item\"><strong>Быстрое падение давления (>1.5 мм/час):</strong> Ухудшение погоды, осадки</div>";
  html += "<div class=\"rule-item\"><strong>Медленное падение давления (0.5-1.5 мм/час):</strong> Возможны осадки</div>";
  html += "<div class=\"rule-item\"><strong>Рост давления (>0.5 мм/час):</strong> Улучшение погоды</div>";
  html += "<div class=\"rule-item\"><strong>Резкие колебания давления:</strong> Неустойчивая погода</div>";
  html += "<div class=\"rule-item\"><strong>Похолодание + падение давления:</strong> Холодный фронт, осадки</div>";
  html += "<div class=\"rule-item\"><strong>Потепление + падение давления:</strong> Теплый фронт, осадки</div>";
  html += "<div class=\"rule-item\"><strong>Стабильные условия:</strong> Ясная погода</div>";
  html += "</div>";
  html += "<div class=\"analysis-card\">";
  html += "<h2>📈 Интерпретация показателей</h2>";
  html += "<p><strong>Стабильность давления:</strong> &lt; 0.5 - очень стабильно, 0.5-1.0 - стабильно, &gt; 1.0 - нестабильно</p>";
  html += "<p><strong>Изменение температуры:</strong> &gt; 1.0 °C/час - быстрое потепление, &lt; -1.0 °C/час - быстрое похолодание</p>";
  html += "</div></div></body></html>";

  server.send(200, "text/html", html);
}

void handleHistory() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>История погоды</title>
    <link rel="stylesheet" href="/style.css">
    <meta charset="UTF-8">
    <style>
        .history-table {
            width: 100%;
            border-collapse: collapse;
            margin: 20px 0;
        }
        .history-table th, .history-table td {
            border: 1px solid #ddd;
            padding: 8px;
            text-align: center;
        }
        .history-table th {
            background-color: #74b9ff;
            color: white;
        }
        .history-table tr:nth-child(even) {
            background-color: #f2f2f2;
        }
        .back-link {
            display: inline-block;
            margin: 10px 0;
            color: #0984e3;
            text-decoration: none;
        }
        .navigation-links {
            display: flex;
            gap: 10px;
            margin: 20px 0;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>📊 История погодных данных</h1>
        <div class="navigation-links">
            <a href="/" class="nav-link">← Назад к текущим данным</a>
            <a href="/charts" class="nav-link">📈 Графики температуры и давления</a>
        </div>
        
        <div class="info">
            <p>Период: последние 6 часов</p>
            <p>Интервал записи: 5 минут</p>
            <p>Всего записей: )rawliteral" + String(weatherHistory.size()) + R"rawliteral(</p>
        </div>
        
        <table class="history-table">
            <thead>
                <tr>
                    <th>Время</th>
                    <th>Температура (°C)</th>
                    <th>Давление (мм рт.ст.)</th>
                    <th>Вероятность осадков (%)</th>
                    <th>Прогноз</th>
                </tr>
            </thead>
            <tbody>
)rawliteral";

  // Добавляем строки таблицы с историческими данными
  for (int i = weatherHistory.size() - 1; i >= 0; i--) {
    WeatherData data = weatherHistory[i];
    html += "<tr>";
    html += "<td>" + formatTime(data.timestamp) + "</td>";
    html += "<td>" + String(data.temperature, 1) + "</td>";
    html += "<td>" + String(data.pressure, 1) + "</td>";
    html += "<td>" + String(data.rainProbability, 0) + "</td>";
    html += "<td>" + data.forecast + "</td>";
    html += "</tr>";
  }

  html += R"rawliteral(
            </tbody>
        </table>
    </div>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleCharts() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Графики погоды</title>
    <link rel="stylesheet" href="/style.css">
    <meta charset="UTF-8">
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        .chart-container {
            background: white;
            border-radius: 10px;
            padding: 20px;
            margin: 20px 0;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
        }
        .navigation-links {
            display: flex;
            gap: 10px;
            margin: 20px 0;
            flex-wrap: wrap;
        }
        .nav-link {
            padding: 10px 15px;
            background: #74b9ff;
            color: white;
            text-decoration: none;
            border-radius: 5px;
            transition: background 0.3s;
        }
        .nav-link:hover {
            background: #0984e3;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>📈 Графики температуры и давления</h1>
        
        <div class="navigation-links">
            <a href="/" class="nav-link">← Назад к текущим данным</a>
            <a href="/history" class="nav-link">📊 Просмотреть историю данных</a>
        </div>
        
        <div class="info">
            <p>Период: последние 6 часов | Интервал записи: 5 минут | Всего точек: )rawliteral" + String(weatherHistory.size()) + R"rawliteral(</p>
        </div>

        <div class="chart-container">
            <h2>🌡️ Температура</h2>
            <canvas id="temperatureChart"></canvas>
        </div>

        <div class="chart-container">
            <h2>📊 Давление</h2>
            <canvas id="pressureChart"></canvas>
        </div>

        <div class="chart-container">
            <h2>📈 Сравнительный график</h2>
            <canvas id="combinedChart"></canvas>
        </div>
    </div>

    <script>
        let temperatureChart, pressureChart, combinedChart;

        // Загрузка данных для графиков
        async function loadChartData() {
            try {
                const response = await fetch('/chart-data');
                const data = await response.json();
                
                updateCharts(data);
            } catch (error) {
                console.error('Error loading chart data:', error);
            }
        }

        // Обновление графиков
        function updateCharts(data) {
            const labels = data.labels;
            const temperatures = data.temperatures;
            const pressures = data.pressures;

            // График температуры
            if (temperatureChart) {
                temperatureChart.destroy();
            }
            temperatureChart = new Chart(document.getElementById('temperatureChart'), {
                type: 'line',
                data: {
                    labels: labels,
                    datasets: [{
                        label: 'Температура (°C)',
                        data: temperatures,
                        borderColor: '#e17055',
                        backgroundColor: 'rgba(225, 112, 85, 0.1)',
                        borderWidth: 2,
                        fill: true,
                        tension: 0.4
                    }]
                },
                options: {
                    responsive: true,
                    plugins: {
                        title: {
                            display: true,
                            text: 'Изменение температуры за последние 6 часов'
                        }
                    },
                    scales: {
                        x: {
                            title: {
                                display: true,
                                text: 'Время'
                            }
                        },
                        y: {
                            title: {
                                display: true,
                                text: 'Температура (°C)'
                            }
                        }
                    }
                }
            });

            // График давления
            if (pressureChart) {
                pressureChart.destroy();
            }
            pressureChart = new Chart(document.getElementById('pressureChart'), {
                type: 'line',
                data: {
                    labels: labels,
                    datasets: [{
                        label: 'Давление (мм рт.ст.)',
                        data: pressures,
                        borderColor: '#0984e3',
                        backgroundColor: 'rgba(9, 132, 227, 0.1)',
                        borderWidth: 2,
                        fill: true,
                        tension: 0.4
                    }]
                },
                options: {
                    responsive: true,
                    plugins: {
                        title: {
                            display: true,
                            text: 'Изменение давления за последние 6 часов'
                        }
                    },
                    scales: {
                        x: {
                            title: {
                                display: true,
                                text: 'Время'
                            }
                        },
                        y: {
                            title: {
                                display: true,
                                text: 'Давление (мм рт.ст.)'
                            }
                        }
                    }
                }
            });

            // Сравнительный график
            if (combinedChart) {
                combinedChart.destroy();
            }
            combinedChart = new Chart(document.getElementById('combinedChart'), {
                type: 'line',
                data: {
                    labels: labels,
                    datasets: [
                        {
                            label: 'Температура (°C)',
                            data: temperatures,
                            borderColor: '#e17055',
                            backgroundColor: 'rgba(225, 112, 85, 0.1)',
                            borderWidth: 2,
                            fill: false,
                            yAxisID: 'y',
                            tension: 0.4
                        },
                        {
                            label: 'Давление (мм рт.ст.)',
                            data: pressures,
                            borderColor: '#0984e3',
                            backgroundColor: 'rgba(9, 132, 227, 0.1)',
                            borderWidth: 2,
                            fill: false,
                            yAxisID: 'y1',
                            tension: 0.4
                        }
                    ]
                },
                options: {
                    responsive: true,
                    interaction: {
                        mode: 'index',
                        intersect: false,
                    },
                    plugins: {
                        title: {
                            display: true,
                            text: 'Сравнение температуры и давления'
                        }
                    },
                    scales: {
                        x: {
                            title: {
                                display: true,
                                text: 'Время'
                            }
                        },
                        y: {
                            type: 'linear',
                            display: true,
                            position: 'left',
                            title: {
                                display: true,
                                text: 'Температура (°C)'
                            }
                        },
                        y1: {
                            type: 'linear',
                            display: true,
                            position: 'right',
                            title: {
                                display: true,
                                text: 'Давление (мм рт.ст.)'
                            },
                            grid: {
                                drawOnChartArea: false,
                            },
                        }
                    }
                }
            });
        }

        // Автообновление графиков каждую минуту
        setInterval(loadChartData, 60000);

        // Первоначальная загрузка данных
        document.addEventListener('DOMContentLoaded', loadChartData);
    </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleChartData() {
  String json = "{";
  json += "\"labels\":[";
  
  // Добавляем метки времени
  for (size_t i = 0; i < weatherHistory.size(); i++) {
    if (i > 0) json += ",";
    json += "\"" + formatTimeShort(weatherHistory[i].timestamp) + "\"";
  }
  
  json += "],\"temperatures\":[";
  
  // Добавляем данные температуры
  for (size_t i = 0; i < weatherHistory.size(); i++) {
    if (i > 0) json += ",";
    json += String(weatherHistory[i].temperature, 1);
  }
  
  json += "],\"pressures\":[";
  
  // Добавляем данные давления
  for (size_t i = 0; i < weatherHistory.size(); i++) {
    if (i > 0) json += ",";
    json += String(weatherHistory[i].pressure, 1);
  }
  
  json += "]}";
  
  server.send(200, "application/json", json);
}

void handleCSS() {
  String css = R"rawliteral(
body {
    font-family: Arial, sans-serif;
    background: linear-gradient(135deg, #74b9ff, #0984e3);
    margin: 0;
    padding: 20px;
    min-height: 100vh;
}

.container {
    max-width: 1000px;
    margin: 0 auto;
    background: white;
    border-radius: 15px;
    padding: 30px;
    box-shadow: 0 10px 30px rgba(0,0,0,0.2);
}

h1 {
    color: #2d3436;
    text-align: center;
    margin-bottom: 30px;
}

.connection-info {
    background: #e8f5e9;
    padding: 15px;
    border-radius: 10px;
    margin-bottom: 20px;
    border-left: 4px solid #4caf50;
}

.connection-info p {
    margin: 5px 0;
    color: #2e7d32;
}

.sensor-data {
    margin-bottom: 30px;
}

.data-item {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 15px;
    margin: 10px 0;
    background: #f8f9fa;
    border-radius: 10px;
    border-left: 4px solid #74b9ff;
}

.label {
    font-weight: bold;
    color: #2d3436;
}

.value {
    font-size: 1.2em;
    font-weight: bold;
    color: #0984e3;
}

.prediction-card {
    background: #fff;
    border-radius: 10px;
    padding: 20px;
    margin: 20px 0;
    border-left: 4px solid #fd79a8;
    box-shadow: 0 2px 10px rgba(0,0,0,0.1);
}

.prediction-text {
    font-size: 1.1em;
    font-weight: bold;
    padding: 15px;
    border-radius: 8px;
    margin: 10px 0;
    text-align: center;
}

.prediction-0 {
    background: #e8f5e9;
    color: #2e7d32;
    border: 2px solid #4caf50;
}

.prediction-1 {
    background: #fff9c4;
    color: #e17055;
    border: 2px solid #fdcb6e;
}

.prediction-2 {
    background: #ffebee;
    color: #c62828;
    border: 2px solid #ef5350;
}

.trend-info {
    background: #f8f9fa;
    padding: 15px;
    border-radius: 8px;
    margin-top: 10px;
}

.trend-info p {
    margin: 5px 0;
}

.forecast {
    background: #fff9c4;
    padding: 20px;
    border-radius: 10px;
    border-left: 4px solid #fdcb6e;
    margin-bottom: 20px;
}

.forecast h2 {
    margin-top: 0;
    color: #e17055;
}

.navigation-links {
    display: flex;
    gap: 10px;
    margin: 20px 0;
    flex-wrap: wrap;
}

.nav-link {
    padding: 10px 15px;
    background: #74b9ff;
    color: white;
    text-decoration: none;
    border-radius: 5px;
    transition: background 0.3s;
}

.nav-link:hover {
    background: #0984e3;
}

.update-time {
    text-align: center;
    color: #636e72;
    font-size: 0.9em;
}

#rain {
    color: #0984e3;
    font-size: 1.3em;
}

.info {
    background: #dfe6e9;
    padding: 15px;
    border-radius: 10px;
    margin-bottom: 20px;
}

.chart-container {
    background: white;
    border-radius: 10px;
    padding: 20px;
    margin: 20px 0;
    box-shadow: 0 2px 10px rgba(0,0,0,0.1);
}
)rawliteral";

  server.send(200, "text/css", css);
}