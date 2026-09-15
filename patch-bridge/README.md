# BroxMon Bridge

פותר את בעיית חיבור המדבקה לאייפון (ולכל בעיית BLE-מהדפדפן בכלל) על ידי הוצאת חלק ה-Bluetooth
לרכיב חומרה נפרד וזול (ESP32), שמדבר Bluetooth מול המדבקה ומעביר את הנתונים ל-**Supabase** -
שהאפליקציה שלנו (naveshlomoff.github.io/BruxAI/) כבר מחוברת אליו. זה עובד **באותו URL בדיוק**
כמו האפליקציה הראשית - בלי דף נפרד, בלי צורך להיות על אותה רשת WiFi.

```
מדבקה (BroxMon01) --Bluetooth--> ESP32 --wss--> Supabase Realtime --wss--> naveshlomoff.github.io/BruxAI/
```

**החיבור למדבקה הוא לפי דרישה:** ה-ESP32 לא מתחבר למדבקה לבד. בלשונית Patch לוחצים **Connect to
patch**, וה-ESP32 מתחבר. **Disconnect patch** מנתק - ולפני הניתוק הוא מכבה את המיקרופון במדבקה
(הקושחה שלה לא עוצרת אותו בניתוק רגיל, וזה מרוקן סוללה). אם הדף נסגר בלי ניתוק, ה-ESP32 מנתק
לבד אחרי 90 שניות.

## מה צריך לקנות

לוח **ESP32** רגיל עם Bluetooth מלא (למשל SparkFun Thing Plus ESP32 WROOM USB-C, או כל לוח
מבוסס ESP32-WROOM-32/32E - **לא** ESP32-S2, לו אין Bluetooth בכלל). כבל USB-C לצריבה.

## שלב 1: התקנת Arduino IDE ותמיכת ESP32

1. הורד והתקן [Arduino IDE](https://www.arduino.cc/en/software) (גרסה 2.x).
2. **File → Preferences** → בשדה "Additional boards manager URLs" הוסף:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. **Tools → Board → Boards Manager** → חפש "esp32" → התקן את **"esp32" by Espressif Systems**.
4. **Tools → Board → esp32 →** בחר **"ESP32 Dev Module"**.
5. **Tools → Partition Scheme →** בחר **"Huge APP (3MB No OTA/1MB SPIFFS)"** (בברירת המחדל הקוד
   לא נכנס).

## שלב 2: התקנת ספריות

**Tools → Manage Libraries…** → חפש והתקן:
- **`NimBLE-Arduino`** (מאת h2zero)
- **`WiFiManager`** (מאת tzapu)
- **`WebSockets`** (מאת Markus Sattler)
- **`ArduinoJson`** (מאת Benoit Blanchon)

## שלב 3: WiFi - אין מה להגדיר בקוד

**אין שם/סיסמת WiFi בקובץ בכלל, בכוונה** - הלוח עובר בין מקומות (בית, משרד, הדגמות), וסיסמה
קבועה בקוד הייתה נשארת לתמיד בהיסטוריית ה-git הציבורי.

- **בהפעלה ראשונה (או בכל מקום חדש)**: הלוח פותח רשת WiFi זמנית בשם **"BroxMon-Setup"**. התחבר
  אליה מהטלפון - אמור להיפתח עמוד הגדרה (אם לא, גלוש ל-192.168.4.1). בחר את ה-WiFi, הכנס סיסמה,
  שמור. הלוח זוכר את זה גם אחרי כיבוי. אם אף אחד לא מגדיר תוך 3 דקות, הלוח מאתחל ומנסה שוב את
  הרשת השמורה.
- **כדי לעבור למקום אחר**: בזמן הפעלת הלוח **החזק את כפתור ה-BOOT** כשניה-שתיים - זה שוכח את
  הרשת השמורה ופותח שוב את "BroxMon-Setup".

## שלב 4: חיבור וצריבה

1. חבר את ה-ESP32 למחשב עם כבל USB-C.
2. **Tools → Port** → בחר את הפורט שהופיע (COMx).
3. לחץ **Upload** (החץ הימני למעלה).
4. פתח **Tools → Serial Monitor** (קצב **115200**) - אמורות להופיע שורות `[WiFi] connected`,
   `[WS] channel join ok` ו-`Ready -- waiting for Connect in the app`.

## שלב 5: שימוש

פתח את naveshlomoff.github.io/BruxAI/ בכל דפדפן (כולל Safari באייפון) → לשונית **Patch**:
- **bridge offline** - ה-ESP32 כבוי או בלי אינטרנט.
- **disconnected** - ה-ESP32 מחובר ומחכה. לחץ **Connect to patch**.
- **connecting** - מחפש ומתחבר למדבקה (כמה שניות).
- **connected** - הנתונים זורמים לגרפים. **Disconnect patch** מנתק.

## שמירה בענן (Supabase)

הנתונים החיים לא נשמרים בשום מקום. **הקלטה** (Start recording ← Stop) נשמרת במכשיר, ואם הוגדר
הענן - גם ב-Supabase:
- שורה בטבלה `patch_sessions`: זמנים, מכשיר, אירועים שזוהו, סדרת עוצמת המיקרופון, מספר דגימות.
- הדגימות הגולמיות (mic/acc/fsm) כקובץ JSON דחוס (gzip) בדלי הפרטי `patch-raw`,
  בנתיב `{user_id}/{session_id}.json.gz`.

**הגדרה חד-פעמית:** Supabase Dashboard → SQL Editor → הדבק את התוכן של
`patch-bridge/supabase_patch_sessions.sql` → Run. כל משתמש רואה רק את ההקלטות שלו (כמו לילות
ואירועים). הקלטה שלא הצליחה לעלות (אין אינטרנט, הענן לא הוגדר) נשמרת במכשיר ועולה אוטומטית
בפעם הבאה שהאפליקציה נפתחת.

## פתרון בעיות

- **נשאר "bridge offline"** - בדוק ב-Serial Monitor שה-ESP32 מחובר ל-WiFi ושמופיע
  `[WS] channel join ok`.
- **נשאר "connecting"** - המדבקה כבויה/רחוקה/סוללה חלשה. ה-ESP32 מזהה אותה גם לפי השם וגם לפי
  הכתובת שלה (00:80:e1:...), כי היא לא תמיד משדרת את השם.
- **WiFi לא מתחבר / נשאר על "BroxMon-Setup"** - החזק BOOT בהפעלה כדי לאפס, וודא שהסיסמה נכונה.
- ב-Serial Monitor מודפס כל 5 שניות כמה הודעות הגיעו מכל חיישן (`acc= mic= fsm=`) - כך רואים
  מיד אם חיישן לא שולח בכלל מהמדבקה.
