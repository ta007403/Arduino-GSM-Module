// This code is use for arduino manage GSM module and then forward command to Raspberry Pi via USB port
// There are command here
//READ <index>
//DEL <index>
//LIST_ALL
//COUNT
//COUNT_STRING
//DEL_ALL
//GSM_RESET
//ARDUINO_RESET
//STATUS
//SEND <Tel Number> <Message>

#include <SoftwareSerial.h>
#include <TimeLib.h> // For time handling

// ======== VERSION / STATUS =========
#define FW_VERSION   "3.0"
#define BUILD_DATE   __DATE__ " " __TIME__
#define DELAY_DURATION 100
#define GSM_POWER_RELAY_PIN 7  // <-- Make sure to set the correct relay pin here!

const byte  LED_PIN = 8;
const unsigned long LED_ON_MS  = 100;
const unsigned long LED_OFF_MS = 2900;

bool ledState = false;          // false = OFF, true = ON
unsigned long lastToggle = 0;   // millis() when state last changed

#include <avr/wdt.h>      // <- also needed for the soft-reset later

SoftwareSerial sim800(3, 2); // RX, TX (Note: connect SIM800L TX to pin 3, RX to pin 2)

String commandBuffer = "";

struct KBankSMSInfo {
  bool isKBank;
  float amount;
  String senderAccount;
  time_t smsTimestamp; // Unix time format
};

extern "C" char *__brkval; // for freeMemory()
extern "C" char __bss_end;

int freeMemory() 
{
  char top;
  return &top - (__brkval ? __brkval : &__bss_end);
}

void softReset() 
{
  wdt_enable(WDTO_15MS);   // shortest period; MCU reboots after ~15 ms
  while (true) { }         // wait for watchdog to bite
}

void reportStatus() {
  Serial.print(F("FW: "));     Serial.print(FW_VERSION);
  Serial.print(F("  ("));      Serial.print(BUILD_DATE); Serial.println(F(")  "));
}

KBankSMSInfo parseKBankSMS(String sms) 
{
  KBankSMSInfo result = { false, 0.0, "", 0 };
  bool hasReceived = false;
  bool hasDeposit  = false;

  // 1. Check if it matches known KBank SMS pattern
  if ( (sms.indexOf("received") == -1 && sms.indexOf("Deposit") == -1) ||
       sms.indexOf("Baht")     == -1 ||
       sms.indexOf("A/C")      == -1 ) {
      return result;
  }

  if(sms.indexOf("received") == -1) // Don't have this word
  {
    hasReceived = false;
    hasDeposit = true;
  }
  else if(sms.indexOf("Deposit") == -1) // Don't have this word
  {
    hasReceived = true;
    hasDeposit = false;
  }

  result.isKBank = true;

  // 2. Find timestamp in format "DD/MM/YY HH:MM"
  // Let's search for pattern "DD/MM/YY HH:MM" using the first "A/C" as anchor
  int patternStart = sms.indexOf("A/C");
  if (patternStart == -1) return result;

  // Go backward to find the timestamp
  // We assume fixed length from the anchor: 17 characters before "A/C"
  String tsBlock = sms.substring(patternStart - 17, patternStart - 1); // should contain "23/05/25 17:31"
  
  //Serial.print("Timestamp raw = "); Serial.println(tsBlock);

  int spaceIdx = tsBlock.indexOf(" ");
  if (spaceIdx == -1) return result;

  String datePart = tsBlock.substring(0, spaceIdx);
  String timePart = tsBlock.substring(spaceIdx + 1);
  //Serial.print("datePart = "); Serial.println(datePart);
  //Serial.print("timePart = "); Serial.println(timePart);

  String ddStr = datePart.substring(2, 4); // day
  String mmStr = datePart.substring(5, 7); // month
  String yyStr = datePart.substring(8, 10); // year
  //Serial.print("ddStr = "); Serial.println(ddStr);
  //Serial.print("mmStr = "); Serial.println(mmStr);
  //Serial.print("yyStr = "); Serial.println(yyStr);
  
  int d = ddStr.toInt();
  int m = mmStr.toInt();
  int y = yyStr.toInt() + 2000;

  int h = timePart.substring(0, 2).toInt();
  int min = timePart.substring(3, 5).toInt();

  //Serial.print("Date = "); Serial.println(d);
  //Serial.print("Month = "); Serial.println(m);
  //Serial.print("Year = "); Serial.println(y);
  //Serial.print("Hour = "); Serial.println(h);
  //Serial.print("Min = "); Serial.println(min);

  tmElements_t tm;
  tm.Day = d;
  tm.Month = m;
  tm.Year = y - 1970;
  tm.Hour = h;
  tm.Minute = min;
  tm.Second = 0;

  result.smsTimestamp = makeTime(tm);

  // 3. Extract amount
  if (hasReceived) {
      int receivedIndex = sms.indexOf("received");
      int bahtIndex = sms.indexOf("Baht", receivedIndex);
      if (receivedIndex != -1 && bahtIndex != -1) {
          String amountStr = sms.substring(receivedIndex + 8, bahtIndex);
          amountStr.trim();
          amountStr = removeCommas(amountStr);
          result.amount = amountStr.toFloat();
      }
  } else if (hasDeposit) {
      int depositIndex = sms.indexOf("Deposit");
      int bahtIndex = sms.indexOf("Baht", depositIndex);
      if (depositIndex != -1 && bahtIndex != -1) {
          String amountStr = sms.substring(depositIndex + 7, bahtIndex);
          amountStr.trim();
          amountStr = removeCommas(amountStr);
          result.amount = amountStr.toFloat();
      }
  }

  // 4. Extract sender account
  int acFrom = sms.indexOf("from A/C ");
  if (acFrom != -1) {
    result.senderAccount = sms.substring(acFrom + 9, sms.indexOf(" ", acFrom + 9));
  }

  return result;
}


void setup()
{
  // initialize digital pin LED_BUILTIN as an output.
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  
  // Initialize serial communication with Arduino and the Arduino IDE (Serial Monitor)
  Serial.begin(9600);
  // Initialize serial communication with Arduino and the SIM800L module
  sim800.begin(9600);
  Serial.println("Initializing...");
  delay(DELAY_DURATION);
  sim800.println("AT"); // Handshake test, should return "OK" on success
  updateSerial();
  sim800.println("AT+CSQ"); // Signal quality test, value range is 0-31, 31 is the best
  updateSerial();
  sim800.println("AT+CCID"); // Read SIM information to confirm whether the SIM is inserted
  updateSerial();
  sim800.println("AT+CREG?"); // Check if it's registered on the network
  updateSerial();
  sim800.println("AT+CMGF=1"); // Always set text mode ONCE before working with SMS
  updateSerial();
  sim800.println("AT+CNMI=2,1,0,0,0"); // Get new SMS to storage
  delay(DELAY_DURATION);

  pinMode(GSM_POWER_RELAY_PIN, OUTPUT);
  digitalWrite(GSM_POWER_RELAY_PIN, LOW); // HIGH = normal power ON (relay off)

  digitalWrite(LED_BUILTIN, LOW);
  digitalWrite(LED_PIN, LOW);

} //End Setup

void resetGSMModule() {
  Serial.println(F("Resetting GSM module (via relay)..."));
  digitalWrite(GSM_POWER_RELAY_PIN, LOW);   // Turn OFF relay = cut power
  digitalWrite(LED_PIN, HIGH);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(10000);                              // Keep off for 10 sec
  digitalWrite(GSM_POWER_RELAY_PIN, HIGH);  // Relay back ON (power GSM)
  digitalWrite(LED_PIN, LOW);
  digitalWrite(LED_BUILTIN, LOW);
  delay(5000);                              // Wait for GSM boot up
}

int getSMSCount() {
  sim800.println("AT+CPMS?"); // Command to query SMS storage status
  delay(DELAY_DURATION);

  String response = "";
  while (sim800.available()) {
    char c = sim800.read();
    response += c;
  }

  // Example response:
  // +CPMS: "SM",3,30,"SM",3,30,"SM",3,30
  int firstComma = response.indexOf(',');
  int secondComma = response.indexOf(',', firstComma + 1);
  if (firstComma != -1 && secondComma != -1) {
    String usedSMS = response.substring(firstComma + 1, secondComma);
    return usedSMS.toInt();
  }

  return -1; // Error
} //End function

String getSMSCountString() {
  sim800.println("AT+CPMS?");
  delay(DELAY_DURATION);

  String response = "";
  while (sim800.available()) {
    char c = sim800.read();
    response += c;
  }

  return response;
} //End function


String readSMS(int index) {
  sim800.print("AT+CMGR=");
  sim800.print(index);
  sim800.print("\r");

  String response = "";
  unsigned long timeout = millis() + 5000;
  while (millis() < timeout) {
    while (sim800.available()) {
      char c = sim800.read();
      response += c;
      timeout = millis() + 500;  // Reset timeout when data is still coming
    }
  }

  return response;
} //End function

void printSMS(int index) {
  String sms = readSMS(index);
  Serial.print("SMS at index ");
  Serial.print(index);
  Serial.println(":");
  Serial.println(sms);
}

bool deleteSMS(int index) {
  sim800.print("AT+CMGD=");
  sim800.print(index);
  sim800.print("\r");
  delay(DELAY_DURATION);

  String response = "";
  while (sim800.available()) {
    char c = sim800.read();
    response += c;
  }

  response.trim();
  return response.indexOf("OK") != -1;
}

bool sendSMS(String number, String message) {
  sim800.println("AT+CMGF=1"); // Set SMS text mode
  delay(DELAY_DURATION);

  sim800.print("AT+CMGS=\"");
  sim800.print(number);
  sim800.println("\"");
  delay(DELAY_DURATION);

  sim800.print(message);
  delay(DELAY_DURATION);

  sim800.write(26); // ASCII code of Ctrl+Z to send SMS
  delay(DELAY_DURATION);

  // Read response to check success
  String response = "";
  unsigned long startTime = millis();
  while (millis() - startTime < 5000) {
    while (sim800.available()) {
      char c = sim800.read();
      response += c;
    }
  }

  response.toUpperCase();
  return response.indexOf("OK") != -1;
}


void updateSerial()
{
  delay(DELAY_DURATION);
  while (Serial.available()) 
  {
    sim800.write(Serial.read()); // Forward data from Serial to Software Serial Port
  }
  while (sim800.available()) 
  {
    Serial.write(sim800.read()); // Forward data from Software Serial to Serial Port
  }
} //End function

void loop() {
  /* ---------- LED heartbeat ----------- */
  unsigned long now = millis();

  if (ledState) {                          // LED currently ON
    if (now - lastToggle >= LED_ON_MS) {   // time to switch OFF
      digitalWrite(LED_PIN, LOW);
      digitalWrite(LED_BUILTIN, LOW);
      ledState   = false;
      lastToggle = now;                    // reset timer
    }
  } else {                                 // LED currently OFF
    if (now - lastToggle >= LED_OFF_MS) {  // time to switch ON
      digitalWrite(LED_PIN, HIGH);
      digitalWrite(LED_BUILTIN, HIGH);
      ledState   = true;
      lastToggle = now;
    }
  }
  /* ------------------------------------ */

  // ---------- your existing logic ----------
  // Forward new SMS from SIM800 to Raspberry Pi
	if (sim800.available()) {
	  String sms = "";
	  while (sim800.available()) {
		sms += (char)sim800.read();
		delay(10);
	  }
	  sms.trim();  // Remove leading/trailing whitespace

	  if (sms.length() > 0) {
		Serial.print("New SMS: ");
		Serial.println(sms);
	  } // else: ignore blank/empty lines!
	}

  // Check for incoming commands from Raspberry Pi
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      processCommand(commandBuffer);
      commandBuffer = "";
    } else {
      commandBuffer += c;
    }
  }
}

void processCommand(String cmd) 
{
  cmd.trim();
  cmd.toUpperCase();

  if (cmd.startsWith("READ ")) 
  {
    String index = cmd.substring(5);
    int index_temp_int = index.toInt();
    //printSMS(index_temp_int); //This line is for debug
    delay(DELAY_DURATION);
    String sms = readSMS(index_temp_int);
    KBankSMSInfo info = parseKBankSMS(sms);
    if (info.isKBank) 
    {
      Serial.println("KBank SMS detected.");
      Serial.print("Amount: "); Serial.println(info.amount);
      Serial.print("From: "); Serial.println(info.senderAccount);
      Serial.print("SMS Timestamp (Unix): "); Serial.println(info.smsTimestamp);
      Serial.print("As DateTime: ");
      Serial.print(day(info.smsTimestamp)); Serial.print("/");
      Serial.print(month(info.smsTimestamp)); Serial.print("/");
      Serial.print(year(info.smsTimestamp)); Serial.print(" ");
      Serial.print(hour(info.smsTimestamp)); Serial.print(":");
      Serial.println(minute(info.smsTimestamp));
    } 
    else 
    {
      Serial.println("Not a KBank message.");
    }
    
    delay(DELAY_DURATION);
    forwardSIM800Response();
  }
   
  else if (cmd.startsWith("DEL ")) 
  {
    String index = cmd.substring(4);
    sim800.print("AT+CMGD=");
    sim800.println(index);
    delay(DELAY_DURATION);
    forwardSIM800Response();
  }
   
  else if (cmd == "LIST_ALL") 
  {
    int SMS_Count = getSMSCount();
    Serial.print("SMS Count = ");
    Serial.println(SMS_Count);
    delay(DELAY_DURATION);
    for (int i = 1; i <= SMS_Count; i++) 
    {
      printSMS(i);
      delay(DELAY_DURATION);
    }
    forwardSIM800Response();
  }
   
  else if (cmd.startsWith("SEND ")) 
  {
    int firstQuote = cmd.indexOf('"');
    int lastQuote = cmd.lastIndexOf('"');
  
    if (firstQuote != -1 && lastQuote != -1 && lastQuote > firstQuote) 
    {
      String number = cmd.substring(5, firstQuote);
      number.trim();  // Apply trim separately
      String message = cmd.substring(firstQuote + 1, lastQuote); // message inside quotes
  
      if (sendSMS(number, message)) 
      {
        Serial.println("SMS SENT: OK");
      } 
      else 
      {
        Serial.println("SMS SENT: FAILED");
      }
    } 
    else 
    {
      Serial.println("FORMAT ERROR: Use SEND <number> \"<message>\"");
    }
  }
   
  else if (cmd == "COUNT") 
  {
    int SMS_Count = getSMSCount();
    Serial.print("SMS Count = ");
    Serial.println(SMS_Count);
    delay(DELAY_DURATION);
    forwardSIM800Response();
  } 
  
  else if (cmd == "COUNT_STRING") 
  {
    String SMS_Count = getSMSCountString();
    Serial.print("SMS Count string = ");
    Serial.println(SMS_Count);
    delay(DELAY_DURATION);
    forwardSIM800Response();
  } 
  
  else if (cmd == "DEL_ALL") 
  { 
      sim800.println("AT+CMGD=1,4");
      //sim800.println("AT+CMGDA=\"DEL ALL\"");
      delay(DELAY_DURATION);
      forwardSIM800Response();
  } 
  
  else if (cmd == "GSM_RESET") 
  {
      //sim800.println("AT+CFUN=1,1");
      //delay(DELAY_DURATION);
      //forwardSIM800Response();
      resetGSMModule();
      Serial.println("Reset GSM Module Done");
  }

  else if (cmd == "STATUS")
  {
    reportStatus();
  }
  
  else if (cmd == "ARDUINO_RESET")
  {
    Serial.println(F("MCU is rebooting..."));
    Serial.flush();     // let the message out first
    delay(DELAY_DURATION);
    softReset();        // never returns
  }
  
  else 
  {
    // Do nothing
//    Serial.println("Unknown command. Use:");
//    Serial.println("  READ <index>");
//    Serial.println("  DEL <index>");
//    Serial.println("  LIST_ALL");
//    Serial.println("  COUNT");
//    Serial.println("  COUNT_STRING");
//    Serial.println("  DEL_ALL");
//    Serial.println("  GSM_RESET");
//    Serial.println("  ARDUINO_RESET");
//    Serial.println("  STATUS");
//    Serial.println("  SEND <Tel Number> <Message>");
  }
} // End function

void forwardSIM800Response() {
  unsigned long startTime = millis();
  while (millis() - startTime < 3000) {
    while (sim800.available()) {
      char c = sim800.read();
      Serial.write(c);
    }
  }
}

String removeCommas(String str) {
  String result = "";
  for (size_t i = 0; i < str.length(); i++) {
    if (str[i] != ',') result += str[i];
  }
  return result;
}