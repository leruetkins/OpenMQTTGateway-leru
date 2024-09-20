/*  
  Theengs OpenMQTTGateway - We Unite Sensors in One Open-Source Interface

   Act as a wifi or ethernet gateway between your SERIAL device and a MQTT broker 
   Send and receiving command by MQTT
 
  This gateway enables to:
 - receive MQTT data from a topic and send SERIAL signal corresponding to the received MQTT data
 - publish MQTT data to a different topic related to received SERIAL signal

    Copyright: (c)Florian ROBERT
  
    This file is part of OpenMQTTGateway.
    
    OpenMQTTGateway is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenMQTTGateway is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "User_config.h"


#ifdef ZgatewaySERIAL

#  ifndef SERIAL_UART // software serial mode
#    include <SoftwareSerial.h>
SoftwareSerial SERIALSoftSerial(SERIAL_RX_GPIO, SERIAL_TX_GPIO); // RX, TX
#  endif


#ifdef Serial_cipher
#include "Cipher.h"  // Подключаем библиотеку шифрования
Cipher *cipher = new Cipher();  // Создаем объект Cipher

// Проверяем наличие ключа шифрования
# ifdef Cipher_key
char *key = Cipher_key;  // Используем определённый ключ
# else
char *key = "testkey";   // Используем ключ по умолчанию, если не задан
# endif

// Функция для преобразования строки в нижний регистр
String toLowerCase(String str) {
    String lowerStr = str;
    lowerStr.toLowerCase();  // Преобразуем строку
    return lowerStr;
}

#endif


// use pointer to stream class for serial communication to make code
// compatible with both softwareSerial as hardwareSerial.
Stream* SERIALStream = NULL;
unsigned long msgCount = 0;



void setupSERIAL() {
#ifdef Serial_cipher
  cipher->setKey(key);
#endif

//Initalize serial port
#  ifdef SERIAL_UART // Hardware serial
#    if SERIAL_UART == 0 // init UART0
  Serial.end(); // stop if already initialized
#      ifdef ESP32
  Serial.begin(SERIALBaud, SERIAL_8N1, SERIAL_RX_GPIO, SERIAL_TX_GPIO);
#      else
  Serial.begin(SERIALBaud, SERIAL_8N1);
#      endif
#      if defined(ESP8266) && defined(SERIAL_UART0_SWAP)
  Serial.swap(); // swap UART0 ports from (GPIO1,GPIO3) to (GPIO15,GPIO13)
#      endif
  SERIALStream = &Serial;
  Log.notice(F("SERIAL HW UART0" CR));

#    elif SERIAL_UART == 1 // init UART1
  Serial1.end(); // stop if already initialized
#      ifdef ESP32
  Serial1.begin(SERIALBaud, SERIAL_8N1, SERIAL_RX_GPIO, SERIAL_TX_GPIO);
#      else
  Serial1.begin(SERIALBaud, SERIAL_8N1);
#      endif
  SERIALStream = &Serial1;
  Log.notice(F("SERIAL HW UART1" CR));

#    elif SERIAL_UART == 2 // init UART2
  Serial2.end(); // stop if already initialized
#      ifdef ESP32
  Serial2.begin(SERIALBaud, SERIAL_8N1, SERIAL_RX_GPIO, SERIAL_TX_GPIO);
#      else
  Serial2.begin(SERIALBaud, SERIAL_8N1);
#      endif
  SERIALStream = &Serial2;
  Log.notice(F("SERIAL HW UART2" CR));

#    elif SERIAL_UART == 3 // init UART3
  Serial3.end(); // stop if already initialized
  Serial3.begin(SERIALBaud, SERIAL_8N1);
  SERIALStream = &Serial3;
  Log.notice(F("SERIAL HW UART3" CR));
#    endif

#  else // Software serial
  // define pin modes for RX, TX:
  pinMode(SERIAL_RX_GPIO, INPUT);
  pinMode(SERIAL_TX_GPIO, OUTPUT);
  SERIALSoftSerial.begin(SERIALBaud);
  SERIALStream = &SERIALSoftSerial; // get stream of serial

  Log.notice(F("SERIAL_RX_GPIO: %d" CR), SERIAL_RX_GPIO);
  Log.notice(F("SERIAL_TX_GPIO: %d" CR), SERIAL_TX_GPIO);
#  endif

  // Flush all bytes in the "link" serial port buffer
  while (SERIALStream->available() > 0)
    SERIALStream->read();

  Log.notice(F("SERIALBaud: %d" CR), SERIALBaud);
  Log.trace(F("ZgatewaySERIAL setup done" CR));
}

#  if SERIALtoMQTTmode == 0 // Convert received data to single MQTT topic
void SERIALtoMQTT() {
  
#ifdef Serial_cipher
    std::map<String, String> sensorTypeMap = {
    {"Temp", "temperature"},
    {"Hum", "humidity"},
    {"Pres", "pressure"},
    {"DP", "temperature"},
    // Добавьте другие сокращения и полные имена по необходимости 
    }; 
    // Если DSerial_cipher определён, вызываем processSerialData
    if (SERIALStream->available()) {
        Log.trace(F("SERIALtoMQTT" CR));

        // Читаем всю доступную строку
        String encryptedData = SERIALStream->readString();

        // Теперь добавляем расшифровку
        String decryptedData = cipher->decryptString(encryptedData);  // Расшифровка строки

        Log.notice(F("Decrypted SERIAL data: %s" CR), decryptedData.c_str());  // Лог расшифрованных данных

        // Отправляем расшифрованные данные в MQTT
        pub(subjectSERIALtoMQTT, decryptedData.c_str());

        // Парсим JSON
        DynamicJsonDocument doc(1024);  // Увеличьте размер при необходимости
        DeserializationError error = deserializeJson(doc, decryptedData);

        if (error) {
            Log.error(F("Failed to parse JSON: %s" CR), error.c_str());
            return;
        }

        // Получаем информацию об устройстве
        const char* deviceName = doc["device_name"];
        const char* deviceDescription = doc["device_description"];
        String deviceIdString = String("00000000") + doc["device_id"].as<String>();
        const char* deviceId = deviceIdString.c_str();

        const char* deviceLocation = doc["device_location"];

        // Настраиваем информацию об устройстве для автодискавери
        DynamicJsonDocument deviceInfo(256);
        JsonObject device = deviceInfo.createNestedObject("device");  // Создаём объект "device" внутри deviceInfo
        JsonArray ids = device.createNestedArray("ids");  // Создаём массив "ids" внутри объекта "device"
        ids.add(deviceId);  // Добавляем идентификатор устройства в массив 

        // Добавляем остальные поля внутри объекта device
        device["name"] = deviceName;
        device["mdl"] = deviceDescription;
        device["mf"] = "Naben";
        device["cu"] = "http://192.168.0.172/";
        device["sw"] = "0.9";

        // Получаем уникальный идентификатор устройства для пути MQTT
        String uniqueId = deviceId; // Используйте ваш уникальный идентификатор

        // Обрабатываем сенсоры
        JsonArray sensors = doc["result"][0]["sensor"];
        for (JsonVariant sensor : sensors) {
            String sensorType = sensor[0].as<String>();  // Тип сенсора (Temp, Hum, и т.д.)
            int sensorValue = sensor[1];                 // Значение сенсора
            const char* sensorUnit = sensor[2];          // Единица измерения
            const char* sensorDescription = sensor[3];   // Описание сенсора

            // Получаем полное имя сенсора
            String fullSensorType = sensorTypeMap[sensorType];
            if (fullSensorType.isEmpty()) {
                fullSensorType = sensorType;  // Если полное имя не найдено, используем сокращение
            }

            // Формируем правильный путь для публикации MQTT Discovery
            String discoveryTopic = String("homeassistant/sensor/") + uniqueId + "-" + sensorType + "/config";
            String stateTopic = String("homeassistant/sensor/") + uniqueId + "-" + sensorType + "/state";
            String LWTTopic = String("homeassistant/") + Gateway_Name + "/" + deviceName + "/LWT";

            // Формируем JSON для MQTT Discovery
            DynamicJsonDocument discoveryDoc(512);
            discoveryDoc["stat_t"] = stateTopic;
            // discoveryDoc["avty_t"] = String(Base_Topic) + String(deviceName) + "/LWT";
            discoveryDoc["avty_t"] = String(Base_Topic) + String(Gateway_Name) + "/" + String(deviceName) + "/LWT";
            discoveryDoc["dev_cla"] = fullSensorType;  // Полное имя типа устройства (temperature, humidity и т.д.)
            discoveryDoc["unit_of_meas"] = sensorUnit;
            discoveryDoc["name"] = sensorDescription;
            discoveryDoc["uniq_id"] = uniqueId + "-" + sensorType;
            discoveryDoc["val_tpl"] = "{{ value_json." + toLowerCase(sensorType) + " | round(1) }}";  // Формируем шаблон значения

            discoveryDoc["pl_avail"] = "online";
            discoveryDoc["pl_not_avail"] = "offline";
            discoveryDoc["stat_cla"] = "measurement"; // Или другой статус-класс

            // Используем объект "device", созданный ранее
            discoveryDoc["device"] = device;  // Информация об устройстве

            String discoveryPayload;
            serializeJson(discoveryDoc, discoveryPayload);

            // Отправляем конфигурацию сенсора в MQTT Discovery
            pubCustom(discoveryTopic.c_str(), discoveryPayload.c_str());

            // Отправляем актуальное значение сенсора
            String statePayload = "{\"" + String(toLowerCase(sensorType)) + "\":" + String(sensorValue) + "}"; 
            pubCustom(stateTopic.c_str(), statePayload.c_str());

            // Отправляем LWT

            pubCustom(LWTTopic.c_str(), "online", true); 
        }
    }
#else
    // Стандартный код
    Log.trace(F("SERIALtoMQTT" CR));
    static char SERIALdata[MAX_INPUT];
    static unsigned int input_pos = 0;
    static char inChar;
    do {
        if (SERIALStream->available()) {
            inChar = SERIALStream->read();
            SERIALdata[input_pos] = inChar;
            input_pos++;
        }
    } while (inChar != SERIALInPost && input_pos < MAX_INPUT);
    SERIALdata[input_pos] = 0;
    input_pos = 0;

    char* output = SERIALdata + sizeof(SERIALPre) - 1;
    Log.notice(F("SERIAL data: %s" CR), output);
    pub(subjectSERIALtoMQTT, output);
#endif
}

#  elif SERIALtoMQTTmode == 1 // Convert received JSON data to one or multiple MQTT topics
void SERIALtoMQTT() {
  // Assumes valid JSON data at SERIAL interface. Use (nested) keys to split JSON data in separate
  // sub-MQTT-topics up to the defined nesting level.
  if (SERIALStream->available()) {
    // Allocate the JSON document
    StaticJsonDocument<JSON_MSG_BUFFER> SERIALBuffer;
    JsonObject SERIALdata = SERIALBuffer.to<JsonObject>();

    // Read the entire JSON string from the serial stream
    String jsonString = SERIALStream->readStringUntil('\n');
    Log.trace(F("SERIAL msg received: %s" CR), jsonString.c_str());

    int startIndex = 0;
    while (startIndex < jsonString.length()) {
      int endIndex = jsonString.indexOf(SERIALPost, startIndex) + strlen(SERIALPost);
      if (endIndex == -1) {
        endIndex = jsonString.length();
      }
      // Extract the individual JSON message
      String singleJsonString = jsonString.substring(startIndex, endIndex);
      // Check if the JSON string contains the prefix and postfix
      if (singleJsonString.startsWith(SERIALPre) && singleJsonString.endsWith(SERIALPost)) {
        // Remove the prefix and postfix
        singleJsonString = singleJsonString.substring(strlen(SERIALPre), singleJsonString.length() - strlen(SERIALPost));
      } else {
        Log.error(F("Invalid SERIAL message format: %s" CR), singleJsonString.c_str());
        return;
      }

      // Check if the JSON string is complete (basic check for curly braces)
      if (singleJsonString.indexOf('{') == -1 || singleJsonString.indexOf('}') == -1) {
        Log.error(F("Incomplete JSON string: %s" CR), singleJsonString.c_str());
        return;
      }

      // Read the JSON document from the JSON string
      DeserializationError err = deserializeJson(SERIALBuffer, singleJsonString);

      if (err == DeserializationError::Ok) {
        // Check if the JSON object is empty
        if (SERIALBuffer.isNull() || SERIALBuffer.size() == 0) {
          Log.error(F("SERIAL data is empty JSON object: %s" CR), jsonString.c_str());
          return;
        }
// JSON received
#    if jsonPublishing
        // send as json
        if (SERIALdata.containsKey("origin")) {
          if (SERIALdata["origin"] == "BT") {
//Decode the BT data
#      ifdef ZgatewayBT
            SERIALdata.remove("origin");
            PublishDeviceData(SERIALdata);
#      endif
          } else {
            enqueueJsonObject(SERIALdata);
          }
        } else {
          SERIALdata["origin"] = subjectSERIALtoMQTT;
          enqueueJsonObject(SERIALdata);
        }

#    endif
#    if simplePublishing
        // send as MQTT topics
        char topic[mqtt_topic_max_size + 1] = subjectSERIALtoMQTT;
        sendMQTTfromNestedJson(SERIALBuffer.as<JsonVariant>(), topic, 0, SERIALmaxJSONlevel);
#    endif
      } else {
        // Print error to serial log
        Log.error(F("Error in SERIALJSONtoMQTT, deserializeJson() returned %s" CR), err.c_str());
      }
      startIndex = endIndex;
    }
  }
}

void sendMQTTfromNestedJson(JsonVariant obj, char* topic, int level, int maxLevel) {
  // recursively step through JSON data and send MQTT messages
  if (level < maxLevel && obj.is<JsonObject>()) {
    int topicLength = strlen(topic);
    // loop over fields
    for (JsonPair pair : obj.as<JsonObject>()) {
      // check if new key still fits in topic cstring
      const char* key = pair.key().c_str();
      Log.trace(F("level=%d, key='%s'" CR), level, pair.key().c_str());
      if (topicLength + 2 + strlen(key) <= mqtt_topic_max_size) {
        // add new level to existing topic cstring
        topic[topicLength] = '/'; // add slash
        topic[topicLength + 1] = '\0'; // terminate
        strncat(topic + topicLength, key, mqtt_topic_max_size - topicLength - 2);

        // step recursively into next level
        sendMQTTfromNestedJson(pair.value(), topic, level + 1, maxLevel);

        // restore topic
        topic[topicLength] = '\0';
      } else {
        Log.error(F("Nested key '%s' at level %d does not fit within max topic length of %d, skipping"),
                  key, level, mqtt_topic_max_size);
      }
    }

  } else {
    // output value at current json level
    char output[MAX_INPUT + 1];
    serializeJson(obj, output, MAX_INPUT);
    Log.notice(F("level=%d, topic=%s, value: %s\n"), level, topic, output);

    // send MQTT message
    pub(topic, &output[0]);
  }
}
#  endif

void XtoSERIAL(const char* topicOri, JsonObject& SERIALdata) {
  if (cmpToMainTopic(topicOri, subjectXtoSERIAL) || SYSConfig.serial) {
    Log.trace(F(" MQTTtoSERIAL" CR));
    const char* prefix = SERIALdata["prefix"] | SERIALPre;
    const char* postfix = SERIALdata["postfix"] | SERIALPost;
    std::string data;
    SERIALStream->print(prefix);
    if (SYSConfig.serial) {
      SERIALdata["msgcount"] = msgCount++;
      serializeJson(SERIALdata, data);
    } else if (SERIALdata.containsKey("value")) {
      data = SERIALdata["value"].as<std::string>();
    }
    Log.notice(F("[ OMG->SERIAL ] data: %s" CR), data.c_str());
    SERIALStream->print(data.c_str());
    SERIALStream->print(postfix);
    delay(10);
  }
}
#endif