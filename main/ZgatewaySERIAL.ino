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

#  ifdef Serial_cipher
#    include <CRC32.h>

#    include "Cipher.h" // Подключаем библиотеку шифрования
Cipher* cipher = new Cipher(); // Создаем объект Cipher
unsigned long pingSentTime = 0; // Время отправки команды ping
int messageID = 0;
std::map<int, unsigned long> activeRequests; // Сопоставление messageID и времени отправки

const int maxDevices = 10; // Максимальное количество устройств
String deviceIdList[maxDevices]; // Статический массив для хранения device_id
int deviceCount = 0; // Текущее количество устройств в массиве

// Проверяем наличие ключа шифрования
#    ifdef Cipher_key
char* key = Cipher_key; // Используем определённый ключ
#    else
char* key = "testkey"; // Используем ключ по умолчанию, если не задан
#    endif

// Функция для преобразования строки в нижний регистр
String toLowerCase(String str) {
  String lowerStr = str;
  lowerStr.toLowerCase(); // Преобразуем строку
  return lowerStr;
}

#  endif

// use pointer to stream class for serial communication to make code
// compatible with both softwareSerial as hardwareSerial.
Stream* SERIALStream = NULL;
unsigned long msgCount = 0;

void setupSERIAL() {
#  ifdef Serial_cipher
  cipher->setKey(key);
  TheengsUtils::syncNTP();
#  endif

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

// Создаем набор для хранения подписанных топиков
// Карта для хранения времени последнего обновления топиков
// std::map<String, unsigned long> lastUpdateTimes; // Хранение времени последнего обновления

// Функция для проверки, не прошло ли 10 минут с последнего обновления топика
// void checkDeviceStatus() {
//     unsigned long currentMillis = millis();
//     for (auto it = lastUpdateTimes.begin(); it != lastUpdateTimes.end(); ) {
//         String deviceName = it->first;
//         unsigned long lastUpdateTime = it->second;

//         // Если с последнего обновления прошло более 10 минут
//         if (currentMillis - lastUpdateTime > 5 * 60 * 1000) {
//             // Отправляем сообщение "offline" в соответствующий топик
//             String LWTTopic = String(Base_Topic) + Gateway_Name + "/" + deviceName + "/LWT";
//             pubCustom(LWTTopic.c_str(), "offline");
//             Log.notice(F("Set topic %s to offline" CR), LWTTopic.c_str());

//             // Удаляем устройство из карты
//             it = lastUpdateTimes.erase(it);
//         } else {
//             ++it; // Переход к следующему элементу
//         }
//     }
// }

#  if SERIALtoMQTTmode == 0 // Convert received data to single MQTT topic
void SERIALtoMQTT() {
  // Проверка каждые 60 секунд
  static unsigned long lastPingTime = 0;
  if (millis() - lastPingTime > 60000) {
    sendPingCommand();
    lastPingTime = millis();
  }
#    ifdef Serial_cipher
  std::map<String, String> sensorTypeMap = {
      {"Temp", "temperature"},
      {"Hum", "humidity"},
      {"Pres", "pressure"},
      {"DP", "temperature"},
      {"Up", "duration"},
      {"Ping", "duration"},

      // Добавьте другие сокращения и полные имена по необходимости
  };
  // Если DSerial_cipher определён, вызываем processSerialData
  if (SERIALStream->available()) {
    Log.trace(F("SERIALtoMQTT" CR));

    String encryptedData = SERIALStream->readString();
    String escapedBinaryString = unescapeControlChars(encryptedData);
    String decryptedData = cipher->decryptString(escapedBinaryString);

    Log.notice(F("Decrypted SERIAL data: %s" CR), decryptedData.c_str());

    DynamicJsonDocument receivedDoc(768);
    DeserializationError error = deserializeJson(receivedDoc, decryptedData);

    if (error) {
      // Serial.print("deserializeJson() failed: ");
      // Serial.println(error.c_str());
      return;
    }

    // CRC check
    String receivedCRC = receivedDoc["crc"].as<String>();
    receivedDoc.remove("crc");
    String jsonForCRCCheck;
    serializeJson(receivedDoc, jsonForCRCCheck);

    CRC32 crcCheck;
    crcCheck.update(jsonForCRCCheck.c_str(), jsonForCRCCheck.length());
    uint32_t calculatedChecksum = crcCheck.finalize();
    uint32_t originalChecksum = strtoul(receivedCRC.c_str(), nullptr, 16);

    if (calculatedChecksum != originalChecksum) {
      // Serial.println("CRC check failed!");
      return;
    }

    pub(subjectSERIALtoMQTT, decryptedData.c_str());

    const char* deviceName = receivedDoc["name"];
    const char* deviceDescription = receivedDoc["desc"];
    String deviceIdString = receivedDoc["id"].as<String>();
    const char* deviceId = deviceIdString.c_str();
    const char* deviceLocation = receivedDoc["loc"];

    addDeviceId(deviceIdString);

    // Создаем общую информацию об устройстве
    DynamicJsonDocument deviceInfoDoc(256);
    JsonObject deviceInfo = deviceInfoDoc.createNestedObject("device");
    JsonArray ids = deviceInfo.createNestedArray("ids");
    ids.add(deviceId);
    deviceInfo["name"] = deviceName;
    deviceInfo["mdl"] = deviceDescription;
    deviceInfo["serial_number"] = deviceId;
    deviceInfo["mf"] = "Naben";
    deviceInfo["cu"] = "http://192.168.0.172/";
    deviceInfo["sw"] = "0.9";
    deviceInfo["via_device"] = Gateway_Name;

    String uniqueId = deviceId;

    // Функция для создания и публикации MQTT discovery сообщения
    auto publishDiscoveryMessage = [&](const String& discoveryTopic, const String& stateTopic,
                                       const String& originalSensorType, const String& fullSensorType,
                                       const char* sensorUnit, const char* sensorDescription) {
      DynamicJsonDocument discoveryDoc(512);
      discoveryDoc["stat_t"] = stateTopic;
      discoveryDoc["avty_t"] = String(Base_Topic) + String(Gateway_Name) + "/devices/" + String(uniqueId) + "/LWT";
      discoveryDoc["dev_cla"] = fullSensorType;
      discoveryDoc["unit_of_meas"] = sensorUnit;
      discoveryDoc["name"] = sensorDescription;
      discoveryDoc["uniq_id"] = uniqueId + "-" + originalSensorType;
      discoveryDoc["val_tpl"] = "{{ value_json." + toLowerCase(originalSensorType) + "}}";
      discoveryDoc["pl_avail"] = "online";
      discoveryDoc["pl_not_avail"] = "offline";
      discoveryDoc["stat_cla"] = "measurement";
      discoveryDoc["expire_after"] = 5 * 60;
      discoveryDoc["device"] = deviceInfo;

      String discoveryPayload;
      serializeJson(discoveryDoc, discoveryPayload);
      pubCustom(discoveryTopic.c_str(), discoveryPayload.c_str());
    };

    if (receivedDoc.containsKey("result") && receivedDoc["result"].as<String>() == "pong") {
      int receivedMessageID = receivedDoc["message_id"].as<int>();
      if (activeRequests.find(receivedMessageID) != activeRequests.end()) {
        unsigned long pongReceivedTime = millis();
        unsigned long elapsedTime = pongReceivedTime - activeRequests[receivedMessageID];
        float elapsedSeconds = round((elapsedTime / 1000.0) * 1000) / 1000;

        String pongTopic = String("homeassistant/sensor/") + uniqueId + "-Ping/config";
        String timeTopic = String("homeassistant/sensor/") + uniqueId + "-Ping/state";

        publishDiscoveryMessage(pongTopic, timeTopic, "Ping", "duration", "s", "Ping");

        DynamicJsonDocument pingDoc(64);
        pingDoc["ping"] = elapsedSeconds;
        String pingPayload;
        serializeJson(pingDoc, pingPayload);

        pubCustom(timeTopic.c_str(), pingPayload.c_str());

        activeRequests.erase(receivedMessageID);
      } else {
        Serial.println("Received message ID not found in active requests.");
      }
    } else if (receivedDoc["result"][0].containsKey("sensor")) {
      JsonArray sensors = receivedDoc["result"][0]["sensor"];
      for (JsonVariant sensor : sensors) {
        String originalSensorType = sensor[0].as<String>();
        int sensorValue = sensor[1];
        const char* sensorUnit = sensor[2];
        const char* sensorDescription = sensor[3];

        String fullSensorType = sensorTypeMap[originalSensorType];
        if (fullSensorType.isEmpty()) {
          fullSensorType = originalSensorType;
        }

        String discoveryTopic = String("homeassistant/sensor/") + uniqueId + "-" + originalSensorType + "/config";
        String stateTopic = String("homeassistant/sensor/") + uniqueId + "-" + originalSensorType + "/state";

        publishDiscoveryMessage(discoveryTopic, stateTopic, originalSensorType, fullSensorType, sensorUnit, sensorDescription);

        DynamicJsonDocument stateDoc(64);
        stateDoc[toLowerCase(originalSensorType)] = sensorValue;
        String statePayload;
        serializeJson(stateDoc, statePayload);
        pubCustom(stateTopic.c_str(), statePayload.c_str());
      }

    } else {
      // Serial.println("Unexpected result or missing 'result' key.");
    }

    // Publish LWT message
    // String LWTTopic = String(Base_Topic) + String(Gateway_Name) + "/devices/" + String(deviceName) + "/LWT";
    String LWTTopic = String(Base_Topic) + String(Gateway_Name) + "/devices/" + String(uniqueId) + "/LWT";
    String nameTopic = String(Base_Topic) + String(Gateway_Name) + "/devices/" + String(uniqueId) + "/name";

    pubCustom(LWTTopic.c_str(), "online", true);
    pubCustom(nameTopic.c_str(), deviceName, true);

    // Publish binary sensor config

    // Создаем общую информацию об устройстве
    DynamicJsonDocument deviceConnectivityInfoDoc(256);
    JsonObject deviceConnectivityInfo = deviceConnectivityInfoDoc.createNestedObject("device");
    JsonArray idsConnectivity = deviceConnectivityInfo.createNestedArray("ids");
    idsConnectivity.add(String(getMacAddress()));

    String configBinarySensorTopic = String("homeassistant/binary_sensor/") + uniqueId + "-connectivity/config";
    DynamicJsonDocument binarySensorDoc(512);

    binarySensorDoc["stat_t"] = String(Base_Topic) + String(Gateway_Name) + "/devices/" + String(uniqueId) + "/LWT";
    binarySensorDoc["avty_t"] = String(Base_Topic) + String(Gateway_Name) + "/devices/" + String(uniqueId) + "/LWT";
    binarySensorDoc["name"] = String(deviceName);
    binarySensorDoc["uniq_id"] = uniqueId + "-connectivity";
    binarySensorDoc["pl_on"] = "online";
    binarySensorDoc["pl_off"] = "offline";
    binarySensorDoc["pl_avail"] = "online";
    binarySensorDoc["pl_not_avail"] = "offline";
    binarySensorDoc["expire_after"] = 5 * 60;
    binarySensorDoc["device"] = deviceConnectivityInfo;

    String binarySensorPayload;
    serializeJson(binarySensorDoc, binarySensorPayload);
    pubCustom(configBinarySensorTopic.c_str(), binarySensorPayload.c_str());
  }
#    else
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
#    endif
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

// void XtoSERIAL(const char* topicOri, JsonObject& SERIALdata) {
//   if (cmpToMainTopic(topicOri, subjectXtoSERIAL) || SYSConfig.serial) {
//     Log.trace(F(" MQTTtoSERIAL" CR));
//     const char* prefix = SERIALdata["prefix"] | SERIALPre;
//     const char* postfix = SERIALdata["postfix"] | SERIALPost;
//     std::string data;
//     SERIALStream->print(prefix);
//     if (SYSConfig.serial) {
//       SERIALdata["msgcount"] = msgCount++;
//       serializeJson(SERIALdata, data);
//     } else if (SERIALdata.containsKey("value")) {
//       data = SERIALdata["value"].as<std::string>();
//     }
//     Log.notice(F("[ OMG->SERIAL ] data: %s" CR), data.c_str());
//     SERIALStream->print(data.c_str());
//     SERIALStream->print(postfix);
//     delay(10);
//   }
// }
void XtoSERIAL(const char* topicOri, JsonObject& SERIALdata) {
  if (cmpToMainTopic(topicOri, subjectXtoSERIAL) || SYSConfig.serial) {
    Log.trace(F(" MQTTtoSERIAL" CR));

    // Сериализуем весь объект JSON в строку
    std::string serializedData;
    serializeJson(SERIALdata, serializedData);

    // Логируем отправляемые данные
    Log.notice(F("[ OMG->SERIAL ] data: %s" CR), serializedData.c_str());

    String cipherString = cipher->encryptString(serializedData.c_str());

    // Экранируем зашифрованные данные
    String escapedCipherString = escapeControlChars(cipherString);

    // Отправляем экранированные данные на серийный порт
    // SERIALStream->print(escapedCipherString);
    SERIALStream->write((const uint8_t*)escapedCipherString.c_str(), escapedCipherString.length());

    delay(10);
  }
}

String escapeControlChars(const String& input) {
  String escapedString = "";
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c < 32 || c == 127) { // ASCII управляющие символы
      escapedString += "~";
      escapedString += char(c + 64); // Сдвигаем символ в printable ASCII диапазон
    } else if (c == '~') { // Экранируем сам символ экранирования
      escapedString += "~~";
    } else {
      escapedString += c;
    }
  }
  return escapedString;
}

String unescapeControlChars(const String& input) {
  String output;
  bool escaped = false;

  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];

    if (escaped) {
      if (c == '~') {
        output += '~'; // Если это экранированный символ '~'
      } else {
        output += (char)(c - 64); // Преобразуем управляющий символ
      }
      escaped = false; // Сбрасываем флаг экранирования
    } else if (c == '~') {
      escaped = true; // Если находим символ экранирования '~', включаем флаг
    } else {
      output += c; // Копируем обычные символы
    }
  }

  return output; // Возвращаем результат
}

// Функция для отправки команды пинга всем устройствам из массива
void sendPingCommand() {
  // Проверяем, есть ли устройства в массиве
  if (deviceCount == 0) {
    // Serial.println("Нет устройств для пинга.");
    return; // Если нет устройств, завершаем выполнение функции
  }

  // Если есть устройства, пингуем их
  for (int i = 0; i < deviceCount; i++) {
    String deviceId = deviceIdList[i];

    DynamicJsonDocument pingCommand(256);

    // Формируем JSON с командой ping
    pingCommand["id"] = "1";
    pingCommand["message_id"] = messageID;
    pingCommand["time"] = TheengsUtils::unixtimestamp();

    JsonObject method = pingCommand.createNestedObject("method");
    method["device_id"] = deviceId; // Используем device_id из списка

    method["command"] = "ping";

    // Сериализуем команду в строку
    String commandPayload;
    serializeJson(pingCommand, commandPayload);

    // Шифруем команду
    String cipherString = cipher->encryptString(commandPayload.c_str());

    // Экранируем зашифрованные данные
    String escapedCipherString = escapeControlChars(cipherString);

    // Отправляем команду через SERIALStream
    SERIALStream->write((const uint8_t*)escapedCipherString.c_str(), escapedCipherString.length());

    // Запоминаем время отправки и добавляем в activeRequests
    pingSentTime = millis();
    activeRequests[messageID] = pingSentTime; // Сохраняем время отправки с messageID

    messageID++;

    delay(10);

    // Serial.println("Ping command sent to device: " + deviceId);
  }
}

// Функция для добавления нового device_id в массив
void addDeviceId(String newDeviceId) {
  // Проверяем, нет ли уже такого device_id в массиве
  bool exists = false;
  for (int i = 0; i < deviceCount; i++) {
    if (deviceIdList[i] == newDeviceId) {
      exists = true;
      break;
    }
  }

  if (!exists && deviceCount < maxDevices) {
    deviceIdList[deviceCount] = newDeviceId; // Добавляем новый device_id
    deviceCount++; // Увеличиваем счетчик устройств
    // Serial.println("Device ID added: " + newDeviceId);
  } else if (exists) {
    // Serial.println("Device ID already exists: " + newDeviceId);
  } else {
    // Serial.println("Device list is full, cannot add: " + newDeviceId);
  }
}

#endif