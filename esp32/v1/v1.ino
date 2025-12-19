#include <WiFi.h>
#include <HTTPClient.h>

// --- تنظیمات وای‌فای و سرور ---
const char* ssid = "AndroidAP";     // اسم وای‌فای
const char* password = "1234tt5678"; // رمز وای‌فای
const char* serverName = "http://webhook.site/3470ad63-8b37-4b98-b34d-1dd1e2cab0fd"; // آدرس وب‌هوک

// پین‌های ارتباطی
#define RXD2 16
#define TXD2 17

void setup() {
  Serial.begin(115200);
  
  // سرعت ارتباط با MC60 (حتما 115200 باشه چون کد MC60 روی دیباگ پورته)
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);

  // اتصال به وای‌فای
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi...");
  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.println("IP: " + WiFi.localIP().toString());
  Serial.println("Waiting for SMS from MC60...");
}

void sendToWebhook(String msg) {
  if(WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverName);
    http.addHeader("Content-Type", "text/plain");
    
    Serial.println("Posting to Server: " + msg);
    int httpResponseCode = http.POST(msg);
    
    if(httpResponseCode > 0) {
      Serial.print("Success! Response code: ");
      Serial.println(httpResponseCode);
    } else {
      Serial.print("Error on sending POST: ");
      Serial.println(httpResponseCode);
    }
    http.end();
  } else {
    Serial.println("WiFi Disconnected!");
  }
}

void loop() {
  if (Serial2.available()) {
    String line = Serial2.readStringUntil('\n');
    line.trim(); // حذف فاصله‌های اضافه
    
    // چاپ همه چیز در سریال مانیتور (برای دیباگ)
    Serial.println("MC60 Log: " + line);

    // فیلتر کردن: فقط اگر با SMS_DATA شروع شده بود بفرست
    if (line.startsWith("SMS_DATA:")) {
      String actualMessage = line.substring(9); // حذف 9 کاراکتر اول (SMS_DATA:)
      Serial.println("Valid SMS found! Sending...");
      sendToWebhook(actualMessage);
    }
  }
}