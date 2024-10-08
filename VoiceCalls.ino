#include "utilities.h"
#include <pdulib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <UrlEncode.h>

const char* serverURL = "http://193.58.235.208:82";
const char* callLogEndpoint = "/calls_data";  
const char* smsLogEndpoint = "/sms_data";    
const char* activityEndpoint = "/node_activity"; 


// Placeholder values to keep the board online; replace these with actual functions or variables as needed
String versionName = "1.0";
String serviceStartedAt = "2024-09-03T16:28:41Z";
String serviceDestroyedAt = "";
String actWasDestroyedAt = "";
String dateOfOffline = "";
int failsSinceOffline = 0;

unsigned long callStartTime = 0;        


// Define the serial console for debug prints, if needed
#define TINY_GSM_DEBUG SerialMon

// Set serial for debug console (to the Serial Monitor, default speed 115200)
#define SerialMon Serial

// See all AT commands, if wanted
#define DUMP_AT_COMMANDS

#include <TinyGsmClient.h>

#ifdef DUMP_AT_COMMANDS  // if enabled it requires the StreamDebugger lib
#include <StreamDebugger.h>
StreamDebugger debugger(Serial2, SerialMon);
TinyGsm modem(debugger);
#else
TinyGsm modem(Serial2);
#endif

#define BUFFER_SIZE 100
PDU mypdu(BUFFER_SIZE); 


// Timer for sending activity info
unsigned long lastActivitySendTime = 0;
const unsigned long activityInterval = 30000;  // 30 seconds interval

void setup() {
    Serial.begin(115200);
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    // Initialize WiFi
    WiFi.begin("Guest", "");
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
    }
    Serial.println("Connected to WiFi");


    // Initialize Serial2 for GSM communication
    Serial2.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

    #ifdef BOARD_POWERON_PIN
    pinMode(BOARD_POWERON_PIN, OUTPUT);
    digitalWrite(BOARD_POWERON_PIN, HIGH);
    #endif

    // Reset the modem
    pinMode(MODEM_RESET_PIN, OUTPUT);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
    delay(1000);
    digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL);
    delay(2600);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);

    // Turn on modem
    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    delay(1000);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH);
    delay(1000);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);

    pinMode(MODEM_RING_PIN, INPUT_PULLUP);

    // Print board information
    Serial.println("Board Information:");
    
    Serial.print("Chip ID: ");
    Serial.println(ESP.getEfuseMac());
    

    Serial.println("Start modem...");
    delay(3000);

    while (!modem.testAT()) {
        delay(100);
    }

    // Disable power-saving mode
    modem.sendAT("+CSCLK=0");
    modem.waitResponse(1000);

    modem.sendAT("+CMEE=2");
    delay(1000);
    Serial.println(Serial2.readString());

    Serial2.println("AT+CSCS=\"GSM\"");  // Set the character set to GSM
    delay(1000);
    Serial.println(Serial2.readString());
    
    if (!modem.getSimStatus()) {
        Serial.println("SIM card not ready");
        return;
    }

    // Set APN
    //modem.sendAT("+CGDCONT=1,\"IP\",\"wap.orange.md\"");

    Serial2.println("AT+CSQ");
    delay(1000); 
    while (Serial2.available()) {
        SerialMon.println("Signal strength:");
        SerialMon.println(Serial2.readString());
    }

    // Manually set network to Orange GSM
    //modem.sendAT("+COPS=1,2,\"25901\",0");


    // Delete all SMS messages from memory at startup
    deleteAllSMS();

    Serial.println("Modem initialized, ready to receive calls and SMS.");
    modem.sendAT("+CMGF?");
    delay(1000);
    Serial.println(Serial2.readString());

    modem.sendAT("+CPMS?");
    delay(1000);
    Serial.println(Serial2.readString());

    modem.sendAT("+CSCS?");
    delay(1000);
    Serial.println(Serial2.readString());

    modem.sendAT("+CMGF=0"); // Set SMS to PDU mode
    delay(1000);
    Serial.println(Serial2.readString());

    modem.sendAT("+CNMI=2,1,0,0,0");
    delay(1000);
    Serial.println(Serial2.readString());

    modem.sendAT("+CMGF?");
    delay(1000);
    Serial.println(Serial2.readString());
}

void loop() {
    if (Serial2.available()) {
        String modemData = Serial2.readString();

        Serial.print("Modem Data: ");
        Serial.println(modemData);

        if (modemData.indexOf("+CMTI:") != -1) {
            Serial.println("New SMS received:");
            Serial.println(modemData);
            checkSMSStorage();  
        } 
        else if (modemData.indexOf("RING") != -1) {
            Serial.println("Incoming call detected via RING");
            startTimer();
            handleIncomingCall();
        } 
        else if (modemData.indexOf("NO CARRIER") != -1 || modemData.indexOf("+CME ERROR") != -1) {
            Serial.println("No carrier or error detected:");
            Serial.println(modemData);
            
            checkSMSStorage();
        }
    }

    // Use a flag to ensure call handling is not double-triggered
    static bool callInProgress = false;

    if (!callInProgress && digitalRead(MODEM_RING_PIN) == LOW) {
        Serial.println("Incoming call detected!");
        callInProgress = true;
        startTimer();
        handleIncomingCall();
        callInProgress = false;
    }

    if (millis() - lastActivitySendTime >= activityInterval) {
        lastActivitySendTime = millis();
        sendActivityInfo();
    }

    delay(100);
}


void startTimer() {
    callStartTime = millis();  // Record the start time
}

unsigned long stopTimer() {
    return (millis() - callStartTime) / 1000;  
}


void sendActivityInfo() {
    StaticJsonDocument<512> jsonData;
    jsonData["device_id"] = String(ESP.getEfuseMac());
    jsonData["battery_level"] = "100"; 
    jsonData["app_version"] = versionName;
    jsonData["utc_datetime"] = getUtcTimeMillis(); 
    jsonData["act_was_destroyed_at"] = actWasDestroyedAt;
    jsonData["is_on_top"] = true; 
    jsonData["service_started_at"] = serviceStartedAt;
    jsonData["service_destroyed_at"] = serviceDestroyedAt;
    jsonData["date_of_offline"] = dateOfOffline;
    jsonData["fails_since_offline"] = failsSinceOffline;

    String jsonString;
    serializeJson(jsonData, jsonString);

    sendPostRequest(String(serverURL) + activityEndpoint, jsonString);
}




void handleIncomingCall() {
    // Answer the call
    Serial2.println("ATA");
    delay(100);  
    String response = Serial2.readString();
    Serial.println(response);

    String callStartTime = "";  
    String callEndTime = "";    

    if (response.indexOf("OK") != -1) {
        callStartTime = getCurrentTime();  
        Serial.println("Call answered.");

        delay(3000);

        Serial2.println("AT+CHUP");
        callEndTime = getCurrentTime();
        Serial.println("Call ended.");
        
        String callerNumber = extractCallerNumber(response);

        stopTimer();
        
        logCallToDatabase(callerNumber, callStartTime, callEndTime);
    } else {
        Serial.println("Failed to answer call.");
        
        checkSMSStorage();
    }
}

String getCurrentTime() {
    time_t now;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
        return "";
    }
    char buffer[20];
    strftime(buffer, 20, "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buffer);
}


String extractCallerNumber(String response) {
    String callerNumber = "";
    int startIdx = response.indexOf("\"");

    if (startIdx != -1) {
        startIdx++;
        int endIdx = response.indexOf("\"", startIdx);
        
        if (endIdx != -1) {
            callerNumber = response.substring(startIdx, endIdx);
        }
    }
    
    return callerNumber;
}



void checkSMSStorage() {
    Serial2.println("AT+CMGL=0");  
    delay(100);  
    String smsResponse = Serial2.readString();
    Serial.println("SMS Storage Check Response:");
    Serial.println(smsResponse);

    if (smsResponse.indexOf("+CMGL:") != -1) {
        Serial.println("Unread SMS found:");

        int indexStart = 0;
        while ((indexStart = smsResponse.indexOf("+CMGL:", indexStart)) != -1) {
            indexStart += 7;  
            int indexEnd = smsResponse.indexOf(",", indexStart);
            String smsIndexStr = smsResponse.substring(indexStart, indexEnd);
            int smsIndex = smsIndexStr.toInt();  
            Serial.println("Reading SMS at index: " + String(smsIndex));
            readSMS(smsIndex);  
        }
        
        deleteAllSMS();
    } else {
        Serial.println("No unread SMS found.");
    }
}

void readSMS(int index) {
    Serial2.print("AT+CMGR=");
    Serial2.println(index);  

    delay(1000);

    // Read the response from the GSM module
    String response = "";
    while (Serial2.available()) {
        response += (char)Serial2.read();
    }

    Serial.println("CMGR Response:");
    Serial.println(response);

    // Check if the response contains the PDU message
    int pduStart = response.indexOf("+CMGR:");
    if (pduStart != -1) {
        // The PDU line usually follows the +CMGR: line
        int pduDataStart = response.indexOf("\n", pduStart) + 1;
        String pduData = response.substring(pduDataStart);
        pduData.trim();  

        Serial.println("PDU Data Found:");
        Serial.println(pduData);  

        // Decode the PDU data using the PDUlib library
        PDU mypdu = PDU(160);  // Adjust the number if needed based on the message length

        if (mypdu.decodePDU(pduData.c_str())) {
            Serial.println("Decoded SMS Content:");
            Serial.print("Sender Number: ");
            String sender = mypdu.getSender();
            Serial.println(sender);

            Serial.print("Message Content: ");
            String message = mypdu.getText();
            Serial.println(message);

            Serial.print("Timestamp: ");
            String timestamp = mypdu.getTimeStamp();
            Serial.println(timestamp);

            Serial.print("SMSC Number: ");
            String smsc = mypdu.getSCAnumber();
            Serial.println(smsc);

            logSMSToDatabase(sender, message, timestamp, smsc);

            Serial2.print("AT+CMGD=");
            Serial2.println(index);  
        } else {
            Serial.println("Failed to decode the PDU data.");
        }
    } else {
        Serial.println("No PDU data found in the response.");
    }
}



void deleteAllSMS() {
    // Command to delete all SMS messages from the modem's memory
    Serial2.println("AT+CMGD=1,4");  
    delay(1000);  
    String response = Serial2.readString();
    Serial.println("Delete All SMS Response:");
    Serial.println(response);
}

String sanitizeMessageContent(String message) {
    message.replace("\u0000", "");  // Replace all instances of null characters
    return message;
}

void logSMSToDatabase(const String& sender, const String& message, const String& timestamp, const String& smsc) {
    unsigned long long utcDatetimeMillis = getUtcTimeMillis();
    String sanitizedMessage = sanitizeMessageContent(message);

    randomSeed(micros());
    int randomID = random(100000, 999999);

    StaticJsonDocument<512> jsonData;
    jsonData["device_id"] = String(ESP.getEfuseMac());  
    jsonData["utc_datetime"] = utcDatetimeMillis;

    JsonArray smsArray = jsonData.createNestedArray("sms_array");
    JsonObject smsData = smsArray.createNestedObject();
    
    smsData["id"] = String(randomID);  
    smsData["date_mls"] = utcDatetimeMillis;  
    smsData["sms_message"] = sanitizedMessage;
    smsData["sender_id_received"] = sender;  
    smsData["smsc_received"] = smsc;  

    String jsonString;
    serializeJson(jsonData, jsonString);

    sendPostRequest(String(serverURL) + smsLogEndpoint, jsonString);
}







void logCallToDatabase(const String& number, const String& startTime, const String& endTime) {
    unsigned long long utcDatetimeMillis = getUtcTimeMillis();
     unsigned long callDuration = stopTimer();

    // Remove the '+' sign from the number if it exists
    String sanitizedNumber = number;
    if (sanitizedNumber.startsWith("+")) {
        sanitizedNumber = sanitizedNumber.substring(1);
    }

    randomSeed(micros());
    int randomID = random(1, 1000);

    StaticJsonDocument<512> jsonData;
    jsonData["device_id"] = String(ESP.getEfuseMac());  
    jsonData["utc_datetime"] = utcDatetimeMillis;  

    JsonArray callsArray = jsonData.createNestedArray("calls_array");
    JsonObject callData = callsArray.createNestedObject();
    
    callData["id"] = String(randomID);  
    callData["date_mls"] = String(utcDatetimeMillis);  
    callData["number"] = sanitizedNumber;  
    callData["type"] = "INCOMING";  
    callData["duration"] = String(callDuration);;  

    String jsonString;
    serializeJson(jsonData, jsonString);
    
    sendPostRequest(String(serverURL) + callLogEndpoint, jsonString);
}


// Function to get the current time in milliseconds since the Unix epoch
unsigned long long getUtcTimeMillis() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to get time");
        return 0;
    }

    time_t now = time(nullptr);

    unsigned long long milliseconds = millis() % 1000;

    unsigned long long epochTimeMillis = static_cast<unsigned long long>(now) * 1000 + milliseconds ;

    return epochTimeMillis;
}


void sendPostRequest(const String& url, String postData) {
    HTTPClient http;
    Serial.println("Original Post Data: " + postData);



    // Wrap the updated JSON in a data parameter and URL-encode it
    String postDataWrapped = "data=" + urlEncode(postData);

    Serial.println("Final URL: " + url);
    Serial.println("Payload: " + postDataWrapped);
    delay(2000);
    http.begin(url);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");  // Set content type to application/x-www-form-urlencoded

    int httpResponseCode = http.POST(postDataWrapped);
    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.println("Response: " + response);
    } else {
        Serial.println("Error on sending POST: " + String(httpResponseCode));
    }
    http.end();
}









