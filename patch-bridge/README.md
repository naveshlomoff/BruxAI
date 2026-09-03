# BroxMon Bridge

פותר את בעיית חיבור המדבקה לאייפון (ולכל בעיית BLE-מהדפדפן בכלל) על ידי הוצאת חלק ה-Bluetooth
לרכיב חומרה נפרד וזול (ESP32), שמדבר Bluetooth מול המדבקה ומעביר את הנתונים דרך WiFi רגיל -
שעובד זהה בכל דפדפן, כולל Safari באייפון.

```
מדבקה (BroxMon01)  --Bluetooth-->  ESP32  --WiFi-->  הנייד/מחשב שלך (http://broxmon.local/)
```

## מה צריך לקנות

לוח **ESP32** רגיל (למשל "ESP32 DevKit V1" / "ESP32-WROOM-32") - זמין כמעט בכל חנות אלקטרוניקה
או באמזון/עלי אקספרס, בסביבות 20-40 ש"ח. גם כבל USB (מיקרו-USB או USB-C, תלוי בלוח) לחיבור
למחשב לצריבה.

## שלב 1: התקנת Arduino IDE ותמיכת ESP32

1. הורד והתקן [Arduino IDE](https://www.arduino.cc/en/software) (גרסה 2.x).
2. **File → Preferences** → בשדה "Additional boards manager URLs" הוסף:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. **Tools → Board → Boards Manager** → חפש "esp32" → התקן את החבילה של Espressif Systems.
4. **Tools → Board** → בחר את הלוח שלך (למשל "ESP32 Dev Module").

## שלב 2: התקנת ספריות

**Tools → Manage Libraries**, חפש והתקן כל אחת מהבאות:

- `NimBLE-Arduino` (מאת h2zero)
- `ESPAsyncWebServer` (fork עדכני, למשל של ESP32Async)
- `AsyncTCP` (תלות של הספרייה הקודמת, אותו מפתח/fork)
- `ArduinoJson` (מאת Benoit Blanchon)

## שלב 3: התקנת כלי ההעלאה של LittleFS

זה מה שמעלה את קובץ `data/page.html` (העמוד עצמו) לזיכרון הפלאש של ה-ESP32, בנפרד מקוד ה-C++.

1. הורד את התוסף **"Arduino LittleFS Upload"** - חיפוש מהיר בגוגל "arduino littlefs upload plugin
   github" ייתן אותך לריפו הרשמי (earlephilhower/arduino-littlefs-upload). עקוב אחר הוראות
   ההתקנה שם (זה קובץ .vsix להתקנה תוך Arduino IDE 2.x).
2. אחרי ההתקנה, יופיע פקודה חדשה בתפריט **Command Palette** (Ctrl+Shift+P) בשם
   **"Upload LittleFS to Pico/ESP8266/ESP32"**.

## שלב 4: הגדרת WiFi

פתח את `BroxMonBridge.ino` (בתיקייה `esp32/BroxMonBridge/`), ובראש הקובץ ערוך:

```cpp
const char* WIFI_SSID     = "שם הרשת שלך";
const char* WIFI_PASSWORD = "הסיסמה שלך";
```

גם אם תשאיר את זה ריק/שגוי - המכשיר עדיין יעבוד, פשוט יפתח נקודת WiFi משלו בשם
**"BroxMon-Bridge"** (סיסמה: `brux12345`) שאפשר להתחבר אליה ישירות מהטלפון/מחשב.

## שלב 5: חיבור וצריבה

1. חבר את ה-ESP32 למחשב עם כבל USB.
2. **Tools → Port** → בחר את הפורט שהופיע (COMx).
3. לחץ **Upload** (החץ הימני למעלה) - זה צורב את קוד ה-C++.
4. אחרי שזה מסתיים בהצלחה: פתח את **Command Palette** (Ctrl+Shift+P) → הרץ
   **"Upload LittleFS to Pico/ESP8266/ESP32"** - זה צורב את קובץ ה-HTML.
5. פתח **Tools → Serial Monitor** (קצב 115200) כדי לראות מה קורה - אמור להופיע שם אם ה-WiFi
   התחבר, ואיזו כתובת IP קיבל, ואם המדבקה נמצאה.

## שלב 6: שימוש

1. חבר את הטלפון/מחשב לאותה רשת WiFi (או ל-"BroxMon-Bridge" אם השתמשת בנקודת הגישה העצמאית).
2. פתח דפדפן וגלוש ל-`http://broxmon.local/` (אם mDNS לא עובד אצלך - תשתמש בכתובת ה-IP
   שהודפסה ב-Serial Monitor, למשל `http://192.168.1.50/`).
3. אמור להיפתח אותו מסך Patch בדיוק כמו באפליקציה הראשית - גרפים חיים, הקלטת session, ייצוא CSV.

## פתרון בעיות

- **"bridge unreachable" באדום** - הטלפון/מחשב לא על אותה רשת כמו ה-ESP32, או שה-ESP32 לא דלוק.
- **"bridge up · patch searching" בענבר, לא זז ל-"connected"** - המדבקה כבויה, לא בטווח, או שהסוללה
  שלה חלשה. תלחץ "Rescan for patch" אחרי שוידאת שהיא דולקת וקרובה.
- לוגים מפורטים תמיד ב-Serial Monitor (115200 baud) - שם רואים בדיוק מה ה-ESP32 עושה בזמן אמת.
