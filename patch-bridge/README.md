# BroxMon Bridge

פותר את בעיית חיבור המדבקה לאייפון (ולכל בעיית BLE-מהדפדפן בכלל) על ידי הוצאת חלק ה-Bluetooth
לרכיב חומרה נפרד וזול (ESP32), שמדבר Bluetooth מול המדבקה ומעביר את הנתונים ל-**Supabase** -
שהאפליקציה שלנו (naveshlomoff.github.io/BruxAI/) כבר מאזינה לו. זה עובד **באותו URL בדיוק**
כמו האפליקציה הראשית - בלי דף נפרד, בלי צורך להיות על אותה רשת WiFi.

```
מדבקה (BroxMon01) --Bluetooth--> ESP32 --HTTPS--> Supabase --wss--> naveshlomoff.github.io/BruxAI/
```

## מה צריך לקנות

לוח **ESP32** רגיל עם Bluetooth מלא (למשל SparkFun Thing Plus ESP32 WROOM USB-C, או כל לוח
מבוסס ESP32-WROOM-32/32E - **לא** ESP32-S2, לו אין Bluetooth בכלל). כבל USB-C-ל-USB-C לצריבה.

## שלב 1: התקנת Arduino IDE ותמיכת ESP32

1. הורד והתקן [Arduino IDE](https://www.arduino.cc/en/software) (גרסה 2.x).
2. **File → Preferences** → בשדה "Additional boards manager URLs" הוסף:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. **Tools → Board → Boards Manager** → חפש "esp32" → התקן את החבילה **"esp32" by Espressif
   Systems** (לא "Arduino ESP32 Boards" - זו חבילה אחרת, מצומצמת יותר).
4. **Tools → Board → esp32 →** בחר **"ESP32 Dev Module"**.

## שלב 2: התקנת ספרייה אחת

**Tools → Manage Libraries…** → חפש והתקן **`NimBLE-Arduino`** (מאת h2zero). זהו - אין צורך
בשום ספרייה נוספת (WiFi/HTTPClient כבר מובנים ב-Arduino IDE של ESP32).

## שלב 3: הגדרת WiFi

פתח את `esp32/BroxMonBridge/BroxMonBridge.ino`, ובראש הקובץ ערוך:

```cpp
const char* WIFI_SSID     = "שם הרשת שלך";
const char* WIFI_PASSWORD = "הסיסמה שלך";
```

**חשוב:** ה-repo הזה ציבורי ב-GitHub - **אל תשמור (commit) את הסיסמה האמיתית לקובץ הזה**. ערוך
אותה מקומית לפני הצריבה, אבל לפני שמעלים שינויים ל-GitHub תחזיר את זה לערכי ה-placeholder
(`YOUR_WIFI_NAME` / `YOUR_WIFI_PASSWORD`).

## שלב 4: חיבור וצריבה

1. חבר את ה-ESP32 למחשב עם כבל USB-C.
2. **Tools → Port** → בחר את הפורט שהופיע (COMx). אם מספר פורטים מופיעים ואתה לא בטוח איזה -
   **Tools → Get Board Info** אחרי שבחרת כל אחד יראה לך VID/PID (CH340 = VID 0x1A86, PID 0x7523,
   שכיח בלוחות ESP32).
3. לחץ **Upload** (החץ הימני למעלה) - זה מקמפל וצורב את הקוד. הצריבה הראשונה לוקחת כמה דקות
   (מוריד toolchain מלא), הבאות הרבה יותר מהירות.
4. פתח **Tools → Serial Monitor** (קצב **115200**, לא 9600!) כדי לראות מה קורה - אמור להופיע
   שם אם ה-WiFi התחבר, ואם המדבקה נמצאה ומחוברת.

## שלב 5: שימוש

זהו - **אין עוד שלבים**. פתח את naveshlomoff.github.io/BruxAI/ בכל דפדפן (כולל Safari באייפון),
לך ללשונית **Patch**. ברגע שה-ESP32 מוצא ומתחבר למדבקה, הנתונים יופיעו שם בלייב - בלי צורך
להיות על אותה רשת WiFi בכלל, כי הכל עובר דרך הענן (Supabase).

## פתרון בעיות

- **פס הסטטוס נשאר "disconnected" באפליקציה** - בדוק ב-Serial Monitor שה-ESP32 מצא את המדבקה
  (`[BLE] connected + subscribed`). אם לא - המדבקה כבויה/רחוקה מדי/הסוללה חלשה.
- **WiFi לא מתחבר** - בדוק שם/סיסמה בקובץ, ושה-ESP32 בטווח הראוטר.
- לוגים מפורטים תמיד ב-Serial Monitor (115200 baud) - שם רואים בדיוק מה ה-ESP32 עושה בזמן אמת,
  כולל אם שליחת הנתונים ל-Supabase נכשלת (`[Supabase] broadcast '...' failed, HTTP ...`).
