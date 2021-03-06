#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Hash.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <logger.h>

extern Garage garage;
extern Settings settings;
extern AsyncWebServer server;
String lastWsMessage;
extern int relay;
extern bool saveSettings(Settings sd);
extern bool restartRequired;
extern const char *version;
bool uploadOK;
String path = "";
AsyncWebSocketClient *globalClient = NULL;
File file;

AsyncWebSocket ws("/ws");

const char *serverIndex = "<form method='POST' action='/firmware' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form>";

// ---------------------- functions -------------------------------

String getContentType(String filename)
{ // convert the file extension to the MIME type
    if (filename.endsWith(".htm"))
        return "text/html";
    else if (filename.endsWith(".css"))
        return "text/css";
    else if (filename.endsWith(".js"))
        return "application/javascript";
    else if (filename.endsWith(".svg"))
        return "image/svg+xml";
    else if (filename.endsWith(".ico"))
        return "image/x-icon";
    return "text/plain";
}

String GetNewUploadHtml()
{
    if (path == "")
        path = "/";
    return "<form method='POST' id='pathForm' action='/setpath' ><input type='text' id='path' name='path' value='" + path + "'><input type='submit' value='Set Path'><button type='button' id='setJavascriptPath' onclick='document.getElementById(\"path\").value=\"/assets/js/\";document.getElementById(\"pathForm\").submit();'>Set Javascript Path</button><button type='button' id='setImagesPath' onclick='document.getElementById(\"path\").value=\"/assets/images/\";document.getElementById(\"pathForm\").submit();'>Set Images Path</button><button type='button' id='setCSSPath' onclick='document.getElementById(\"path\").value=\"/assets/css/\";document.getElementById(\"pathForm\").submit();'>Set CSS Path</button><button type='button' id='setHTMLPath' onclick='document.getElementById(\"path\").value=\"/\";document.getElementById(\"pathForm\").submit();'>Set HTML Path</button></form><br><form method='POST' action='/files' enctype='multipart/form-data'><input type='file' name='file'><input type='submit' value='Upload'></form>";
}

String processor(const String &var)
{
    if (var == "TITLE")
    {
        String title = String(settings.title);
        return (title == "" ? "GaragePack" : title);
    }
    else if(var == "VERSION")
    {
        return  "v" + String(version);
    }

    return String();
}

void handleNotFound(AsyncWebServerRequest *request)
{
    String path = request->url();
    if (path.endsWith("/"))
        path += "index.htm";                   // If a folder is requested, send the index file
    String contentType = getContentType(path); // Get the MIME type
    if (SPIFFS.exists(path))
    {                                       // If the file exists
        File file = SPIFFS.open(path, "r"); // Open it
        AsyncWebServerResponse *response;
        if (contentType == "text/html")
        {
            response = request->beginResponse(SPIFFS, path, contentType, false, processor);
        }
        else
        {
            response = request->beginResponse(SPIFFS, path, contentType);
        }

        // response->addHeader("Server","ESP Async Web Server");
        request->send(response);
        Logger.println("200\tGET\t" + path);
        file.close(); // Then close the file again
    }
    else
    {
        Logger.println("404\tGET\t" + path);
        request->send(404, "text/plain", "Not found");
    }
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
    if (type == WS_EVT_CONNECT)
    {
        Logger.println("Websocket client connection received");
        globalClient = client;
        client->text(lastWsMessage);
    }
    else if (type == WS_EVT_DISCONNECT)
    {
        globalClient = NULL;
        Logger.println("Client disconnected");
    }
}

void WifiSendtatus(String message)
{
    lastWsMessage = message;
    if (globalClient != NULL && globalClient->status() == WS_CONNECTED)
    {
        globalClient->text(lastWsMessage);
        // globalClient->text();
    }
}

// ----------------------------------------------------------------

void startWifi()
{
    // SPIFFS.begin();
    //######## config WIFI #########

    if (String(settings.wifiSSID) == "" || settings.runSetup == true)
    {
        Logger.println("Running SetupMode");
        WiFi.hostname("GaragePackSetup");

        WiFi.persistent(false);
        // disconnect sta, start ap
        WiFi.disconnect(); //  this alone is not enough to stop the autoconnecter
        WiFi.mode(WIFI_AP);
        WiFi.persistent(true);

        WiFi.softAP("GaragePackSetup_" + String(random(0xffff), HEX));

        Logger.println("IP address: " + WiFi.softAPIP().toString());
        // if (WiFi.wai() != WL_CONNECTED) {
        //     Logger.printlnf("WiFi AP Failed!\n");
        //     return;
        // }
    }
    else
    {
        //######## config WIFI #########
        String hostname = "GaragePack";
        if (String(settings.wifiHostname) != "")
            hostname = settings.wifiHostname;
        WiFi.hostname(hostname);
        WiFi.mode(WIFI_STA);

        WiFi.begin(settings.wifiSSID, settings.wifiPassword);
        if (WiFi.waitForConnectResult() != WL_CONNECTED)
        {
            Logger.println("WiFi Failed!\n");
            return;
        }

        Logger.println("IP Address: " + WiFi.localIP().toString());
    }

    Logger.println("Hostname: " + WiFi.hostname());

    //######## WIFI routes #########

    server.on("/attributes", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", AttributesToJson(garage.getAttributes()));
    });
    server.on("/door", HTTP_GET, [](AsyncWebServerRequest *request) {
        String action;
        if (request->hasParam("action"))
        {
            action = request->getParam("action")->value();
        }

        if (action == "on" || action == "open")
        {
            request->send(200, "application/json", "true");
            garage.open();
        }
        else if (action == "stop")
        {
            request->send(200, "application/json", "true");
            garage.stop();
        }
        else if (action == "off" || action == "close")
        {
            request->send(200, "application/json", "false");
            garage.close();
        }
        else
        {
            request->send(400, "application/json", "invalid command. use on, off, open,close or stop");
        }
    });

    server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
        Logger.println("restarting");
        request->send(200, "application/json", "true");
        delay(1000);
        restartRequired = true;
    });

    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", SettingsToJson(settings));
    });

    server.on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/log.log", "text/plain");
    });

    // Send a POST request to <IP>/post with a form field message set to <message>
    server.on("/settings", HTTP_POST, [](AsyncWebServerRequest *request) {
        String message;
        if (request->hasParam("body", true))
        {
            message = request->getParam("body", true)->value();
        }
        else
        {
            request->send(400, "application/json", "no body key sent");
            return;
        }
        Settings sp = JsonToSettings(message);
        sp.runSetup = false; //overwrite privously set runsetup
        if (saveSettings(sp))
        {
            request->send(200, "application/json", "true");
        }
        else
        {
            request->send(500, "application/json", "writing settings failed");
        }
    });

    server.on("/files", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", GetNewUploadHtml());
        response->addHeader("Connection", "close");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
    });

    server.on("/setpath", HTTP_POST, [](AsyncWebServerRequest *request) {
        path = "/";
        if (request->hasParam("path", true))
        {
            AsyncWebParameter *p = request->getParam("path", true);
            path = p->value();
        }
        if (path == "")
            path = "/";
        request->redirect("/files");
    });

    server.on("/files", HTTP_POST, [](AsyncWebServerRequest *request) {
        // the request handler is triggered after the upload has finished...
        // create the response, add header, and send response
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", (uploadOK) ? "OK" : "FAIL");
        response->addHeader("Connection", "close");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response); },
              [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        //Upload handler chunks in data
        String filepath = "";

        if(!index)
        {
            uploadOK = true;
            String filepath = path + filename;
            Logger.println("Upload filepath: " + filepath);
            SPIFFS.remove(filepath);
            file = SPIFFS.open(filepath, "a+");
            if (!file) {
                Logger.println("Error opening file for writing");
                uploadOK = false;
            }
        }      

        if(uploadOK)
        {
            int bytesWritten = file.write(data, len);
    
            if (bytesWritten > 0) {
                Logger.println("File was written: " + String(bytesWritten));
            } else {
                Logger.println("File write failed");
                uploadOK = false;
            }
        }
        if(final) file.close(); });

    // --------------- UPDATE FIRMWARE ----------------------
    server.on("/firmware", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", serverIndex);
        response->addHeader("Connection", "close");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
    });

    server.on("/firmware", HTTP_POST, [](AsyncWebServerRequest *request) {
        // the request handler is triggered after the upload has finished... 
        // create the response, add header, and send response
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", (Update.hasError())?"FAIL":"OK");
        response->addHeader("Connection", "close");
        response->addHeader("Access-Control-Allow-Origin", "*");
        restartRequired = true;  // Tell the main loop to restart the ESP
        request->send(response); }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        //Upload handler chunks in data
        
        if(!index){ // if index == 0 then this is the first frame of data
        Logger.println("UploadStart: " + filename);
        Serial.setDebugOutput(true);
        
        // calculate sketch space required for the update
        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if(!Update.begin(maxSketchSpace)){//start with max available size
            Update.printError(Serial);
        }
        Update.runAsync(true); // tell the updaterClass to run in async mode
        }

        //Write chunked data to the free sketch space
        if(Update.write(data, len) != len){
            Update.printError(Serial);
        }
        
        if(final){ // if the final flag is set then this is the last frame of data
        if(Update.end(true)){ //true to set the size to the current progress
            Logger.println("Update Success: " + String(index+len) + " B. Rebooting...");
            } else {
            Update.printError(Serial);
            }
            Serial.setDebugOutput(false);
        } });

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.onNotFound(handleNotFound);

    // #######################

    server.begin();
}