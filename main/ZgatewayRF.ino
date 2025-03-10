/*  
  Theengs OpenMQTTGateway - We Unite Sensors in One Open-Source Interface

   Act as a gateway between your 433mhz, infrared IR, BLE, LoRa signal and one interface like an MQTT broker 
   Send and receiving command by MQTT
 
  This gateway enables to:
 - receive MQTT data from a topic and send RF 433Mhz signal corresponding to the received MQTT data
 - publish MQTT data to a different topic related to received 433Mhz signal

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

#ifdef ZgatewayRF

#  ifdef ZradioCC1101
#    include <ELECHOUSE_CC1101_SRC_DRV.h>
#  endif

#  include <RCSwitch.h> // library for controling Radio frequency switch

RCSwitch mySwitch = RCSwitch();

//SOME CONVERSION function from https://github.com/sui77/rc-switch/tree/master/examples/ReceiveDemo_Advanced
static const char* bin2tristate(const char* bin) {
  static char returnValue[50];
  int pos = 0;
  int pos2 = 0;
  while (bin[pos] != '\0' && bin[pos + 1] != '\0') {
    if (bin[pos] == '0' && bin[pos + 1] == '0') {
      returnValue[pos2] = '0';
    } else if (bin[pos] == '1' && bin[pos + 1] == '1') {
      returnValue[pos2] = '1';
    } else if (bin[pos] == '0' && bin[pos + 1] == '1') {
      returnValue[pos2] = 'F';
    } else {
      return "-";
    }
    pos = pos + 2;
    pos2++;
  }
  returnValue[pos2] = '\0';
  return returnValue;
}

static char* dec2binWzerofill(unsigned long Dec, unsigned int bitLength) {
  static char bin[64];
  unsigned int i = 0;

  while (Dec > 0) {
    bin[32 + i++] = ((Dec & 1) > 0) ? '1' : '0';
    Dec = Dec >> 1;
  }

  for (unsigned int j = 0; j < bitLength; j++) {
    if (j >= bitLength - i) {
      bin[j] = bin[31 + i - (j - (bitLength - i))];
    } else {
      bin[j] = '0';
    }
  }
  bin[bitLength] = '\0';

  return bin;
}

#  if defined(ZmqttDiscovery) && !defined(RF_DISABLE_TRANSMIT) && defined(RFmqttDiscovery)

void RFtoMQTTdiscovery(uint64_t MQTTvalue) {
  //on the fly switch creation from received RF values
  char val[11];
  sprintf(val, "%lu", MQTTvalue);
  Log.trace(F("RF Entity Discovered, create HA Discovery CFG" CR));
  char* switchRF[2] = {val, "RF"};
  Log.trace(F("CreateDiscoverySwitch: %s" CR), switchRF[1]);
#    if valueAsATopic
  String discovery_topic = String(subjectRFtoMQTT) + "/" + String(switchRF[0]);
#    else
  String discovery_topic = String(subjectRFtoMQTT);
#    endif

  String theUniqueId = getUniqueId("-" + String(switchRF[0]), "-" + String(switchRF[1]));

  announceDeviceTrigger(
      false,
      (char*)discovery_topic.c_str(),
      "", "",
      (char*)theUniqueId.c_str(),
      "", "", "", "");
}
#  endif

void RFtoMQTT() {
  if (mySwitch.available()) {
    StaticJsonDocument<JSON_MSG_BUFFER> RFdataBuffer;
    JsonObject RFdata = RFdataBuffer.to<JsonObject>();
    Log.trace(F("Rcv. RF" CR));
#  ifdef ESP32
    Log.trace(F("RF Task running on core :%d" CR), xPortGetCoreID());
#  endif
    uint64_t MQTTvalue = mySwitch.getReceivedValue();
    int length = mySwitch.getReceivedBitlength();
    const char* binary = dec2binWzerofill(MQTTvalue, length);

    RFdata["value"] = (uint64_t)MQTTvalue;
    RFdata["protocol"] = (int)mySwitch.getReceivedProtocol();
    RFdata["length"] = (int)mySwitch.getReceivedBitlength();
    RFdata["delay"] = (int)mySwitch.getReceivedDelay();
    RFdata["tre_state"] = bin2tristate(binary);
    RFdata["binary"] = binary;

#  if defined(ESP32) || defined(ESP8266)
    unsigned int* raw = mySwitch.getReceivedRawdata();
    std::string rawDump;
    for (unsigned int i = 0; i < length * 2; i++) {
      if (i != 0)
        rawDump += ",";
      rawDump += std::to_string(raw[i]);
    }
    RFdata["raw"] = rawDump;
#  endif
#  ifdef ZradioCC1101 // set Receive off and Transmitt on
    RFdata["frequency"] = RFConfig.frequency;
#  endif
    mySwitch.resetAvailable();

    if (!isAduplicateSignal(MQTTvalue) && MQTTvalue != 0) { // conditions to avoid duplications of RF -->MQTT
#  if defined(ZmqttDiscovery) && !defined(RF_DISABLE_TRANSMIT) && defined(RFmqttDiscovery) //component creation for HA
      if (SYSConfig.discovery)
        RFtoMQTTdiscovery(MQTTvalue);
#  endif
      RFdata["origin"] = subjectRFtoMQTT;
      enqueueJsonObject(RFdata);
      // Casting "receivedSignal[o].value" to (unsigned long) because ArduinoLog doesn't support uint64_t for ESP's
      Log.trace(F("Store val: %u" CR), (unsigned long)MQTTvalue);
      storeSignalValue(MQTTvalue);
      if (repeatRFwMQTT) {
        Log.trace(F("Pub RF for rpt" CR));
        RFdata["origin"] = subjectMQTTtoRF;
        enqueueJsonObject(RFdata);
      }
    }
  }
}

#  if simpleReceiving
void MQTTtoRF(char* topicOri, char* datacallback) {
#    ifdef ZradioCC1101 // set Receive off and Transmitt on
  disableCurrentReceiver();
  ELECHOUSE_cc1101.SetTx(RFConfig.frequency);
  Log.notice(F("Transmit frequency: %F" CR), RFConfig.frequency);
#    endif
  mySwitch.disableReceive();
  mySwitch.enableTransmit(RF_EMITTER_GPIO);
  uint64_t data = strtoull(datacallback, NULL, 10); // we will not be able to pass values > 4294967295 on Arduino boards

  // RF DATA ANALYSIS
  //We look into the subject to see if a special RF protocol is defined
  String topic = topicOri;
  int valuePRT = 0;
  int valuePLSL = 0;
  int valueBITS = 0;
  int pos = topic.lastIndexOf(RFprotocolKey);
  if (pos != -1) {
    pos = pos + +strlen(RFprotocolKey);
    valuePRT = (topic.substring(pos, pos + 1)).toInt();
  }
  //We look into the subject to see if a special RF pulselength is defined
  int pos2 = topic.lastIndexOf(RFpulselengthKey);
  if (pos2 != -1) {
    pos2 = pos2 + strlen(RFpulselengthKey);
    valuePLSL = (topic.substring(pos2, pos2 + 3)).toInt();
  }
  int pos3 = topic.lastIndexOf(RFbitsKey);
  if (pos3 != -1) {
    pos3 = pos3 + strlen(RFbitsKey);
    valueBITS = (topic.substring(pos3, pos3 + 2)).toInt();
  }

  if ((cmpToMainTopic(topicOri, subjectMQTTtoRF)) && (valuePRT == 0) && (valuePLSL == 0) && (valueBITS == 0)) {
    Log.trace(F("MQTTtoRF dflt" CR));
    mySwitch.setProtocol(1, 350);
    mySwitch.send(data, 24);
    // Acknowledgement to the GTWRF topic
    pub(subjectGTWRFtoMQTT, datacallback);
  } else if ((valuePRT != 0) || (valuePLSL != 0) || (valueBITS != 0)) {
    Log.trace(F("MQTTtoRF usr par." CR));
    if (valuePRT == 0)
      valuePRT = 1;
    if (valuePLSL == 0)
      valuePLSL = 350;
    if (valueBITS == 0)
      valueBITS = 24;
    Log.notice(F("RF Protocol:%d" CR), valuePRT);
    Log.notice(F("RF Pulse Lgth: %d" CR), valuePLSL);
    Log.notice(F("Bits nb: %d" CR), valueBITS);
    mySwitch.setProtocol(valuePRT, valuePLSL);
    mySwitch.send(data, valueBITS);
    // Acknowledgement to the GTWRF topic
    pub(subjectGTWRFtoMQTT, datacallback); // we acknowledge the sending by publishing the value to an acknowledgement topic, for the moment even if it is a signal repetition we acknowledge also
  }
#    ifdef ZradioCC1101 // set Receive on and Transmitt off
  ELECHOUSE_cc1101.SetRx(RFConfig.frequency);
  mySwitch.disableTransmit();
  mySwitch.enableReceive(RF_RECEIVER_GPIO);
#    endif
}
#  endif

#  if jsonReceiving
void MQTTtoRF(char* topicOri, JsonObject& RFdata) { // json object decoding
  if (cmpToMainTopic(topicOri, subjectMQTTtoRF)) {
    Log.trace(F("MQTTtoRF json" CR));
    uint64_t data = RFdata["value"];
    if (data != 0) {
      int valuePRT = RFdata["protocol"] | 1;
      int valuePLSL = RFdata["delay"] | 350;
      int valueBITS = RFdata["length"] | 24;
      int valueRPT = RFdata["repeat"] | RF_EMITTER_REPEAT;
      Log.notice(F("RF Protocol:%d" CR), valuePRT);
      Log.notice(F("RF Pulse Lgth: %d" CR), valuePLSL);
      Log.notice(F("Bits nb: %d" CR), valueBITS);
#    ifdef ZradioCC1101
      disableCurrentReceiver();
      initCC1101();
      int txPower = RFdata["txpower"] | RF_CC1101_TXPOWER;
      ELECHOUSE_cc1101.setPA((int)txPower);
      Log.notice(F("CC1101 TX Power: %d" CR), txPower);
      float txFrequency = RFdata["frequency"] | RFConfig.frequency;
      ELECHOUSE_cc1101.SetTx(txFrequency);
      Log.notice(F("Transmit frequency: %F" CR), txFrequency);
#    endif
      mySwitch.enableTransmit(RF_EMITTER_GPIO);
      mySwitch.setRepeatTransmit(valueRPT);
      mySwitch.setProtocol(valuePRT, valuePLSL);
      mySwitch.send(data, valueBITS);
      Log.notice(F("MQTTtoRF OK" CR));
      // we acknowledge the sending by publishing the value to an acknowledgement topic, for the moment even if it is a signal repetition we acknowledge also
      pub(subjectGTWRFtoMQTT, RFdata);
      mySwitch.setRepeatTransmit(RF_EMITTER_REPEAT); // Restore the default value
    }

    enableActiveReceiver();
  }
}
#  endif

int receiveInterupt = -1;

void disableRFReceive() {
  Log.trace(F("disableRFReceive %d" CR), receiveInterupt);
  mySwitch.disableReceive();
}

void enableRFReceive() {
  Log.notice(F("Enable RF Receiver: %FMhz" CR), RFConfig.frequency);
  //RF init parameters
  Log.notice(F("RF_EMITTER_GPIO: %d " CR), RF_EMITTER_GPIO);
  Log.notice(F("RF_RECEIVER_GPIO: %d " CR), RF_RECEIVER_GPIO);

#  ifdef RF_DISABLE_TRANSMIT
  mySwitch.disableTransmit();
#  else
  mySwitch.enableTransmit(RF_EMITTER_GPIO);
#  endif
  receiveInterupt = RF_RECEIVER_GPIO;
  mySwitch.setRepeatTransmit(RF_EMITTER_REPEAT);
  mySwitch.enableReceive(receiveInterupt);
  Log.trace(F("ZgatewayRF command topic: %s%s%s" CR), mqtt_topic, gateway_name, subjectMQTTtoRF);
  Log.trace(F("ZgatewayRF setup done" CR));
}
#endif
