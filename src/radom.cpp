//Importation des librairies
#include <Arduino.h>
#include <SoftwareSerial.h>
#include <String.h>
#include <DS3231.h>
#include <Wire.h>
#include <PersonalData.h>
#include <EEPROM.h>//Lib AT24C32
#include <VirtualWire.h> //Lib for wireless

// Liste des fonctions
void readSMS(String message);
void sendMessage(String message);
void setConsigne(String message, int indexConsigne);
void heatingProg();
void turnOn() ;
void turnOnWithoutMessage() ;
void turnOff() ;
void turnOffWithoutMessage() ;
String getDate() ;
void sendStatus() ;
void i2c_eeprom_write_byte( int deviceaddress, unsigned int eeaddress, byte data );
byte i2c_eeprom_read_byte( int deviceaddress, unsigned int eeaddress );
void eepromWriteData(float value);
float eepromReadSavedConsigne();
int getBijunctionState();
void listen(int timeout);
int getLastUpdate();
void checkThermostat();
void checkBiJunction();
void heatingProcess();

//Définition des pinouts
#define BIJUNCTION_PIN 3
enum {
  ENABLED = 1,
  DISABLED = 0
};
#define RX_PIN 6 //Renseigne la pinouille connectée au module radio 433MHz
SoftwareSerial gsm(10, 11); // Pins TX,RX du Arduino
#define RELAYS_PERSO 2 // Pin connectée au relai
#define RELAYS_COMMON 3 // Pin de commandes des relais du commun
#define LED_PIN 13
//pin 4,5 -> I2C DS3231

//Variables de texte
String textMessage;
int index = 0;

//Variables pour la gestion du temps
DS3231 Clock;
bool Century=false;
bool h12;
bool PM;
//Pour le comptage du temps
unsigned long lastTempMeasureMillis = 0;
int lastRefresh = 0;
#define THERMOSTAT_LISTENING_TIME 8000 // En millisecondes

//Variables de mémorisation d'état
int currentBijunctionState = DISABLED; // Etat de présence précédent du secteur commun
int heating = DISABLED; // Variable d'état du chauffage par le courant perso
int forcedHeating = DISABLED;
int program = DISABLED; // Programmation active ou non
int alertNoSignalSent = false;
int alertBatteryLowSent = false;
int alertBatteryCriticalSent = false;

//Programmation de la consigne de Programmation
#define hysteresis 1.0
float consigne;
float newConsigne = 1.0;
//const float temperatureOffset = 0.0; // pour corriger un éventuel offset de temps
float temperature = 33.3; // température par défaut
int batteryLevel = 101;
struct ThermostatData{
  float temp;
  int batt;
};
ThermostatData receivedData;

//Variable pour le wireless
byte messageSize = sizeof(ThermostatData);

//MODE DEBUG
//Permet d'afficher le mode débug dans la console
//Beaucoup plus d'infos apparaissent
#define DEBUG 1 // 0 pour désactivé et 1 pour activé

//Récupération des données privées qui ne sont pas uploadées dans GITHUB
PersonalData personalData; // Objet contenant les données sensibles
String phoneNumber = personalData.getPhoneNumber();
String pinNumber = personalData.getPinNumber();

/*SETUP************************************************************************/
// cppcheck-suppress unusedFunction
void setup() {
  // Start the I2C interface
  Wire.begin();
  //Configuration des I/O
  pinMode(RELAYS_PERSO, OUTPUT);
  pinMode(RELAYS_COMMON, OUTPUT);
  pinMode(BIJUNCTION_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(RELAYS_PERSO, LOW); // The current state of the RELAYS_PERSO is off NO, donc ne laisse pas passser
  digitalWrite(RELAYS_COMMON, LOW); // NC donc laisse passer au repos
  heating = false;

  Serial.begin(9600);//Demarrage Serial
  Serial.print("Connecting...");
  delay(5000);

  gsm.begin(9600);//Demarrage GSM

  Serial.println("Connected");

  gsm.print("AT+CREG?\r\n");
  delay(1000);
  gsm.print("AT+CMGF=1\r\n");
  delay(1000);
  gsm.println("AT+CNMI=2,2,0,0,0\r\n"); //This command selects the procedure
  delay(1000);                          //for message reception from the network.
  // gsm.println("AT+CMGD=4\r\n"); //Suppression des SMS
  // delay(1000);

  // Initialisation de la bibliothèque VirtualWire
  // Vous pouvez changez les broches RX/TX/PTT avant vw_setup() si nécessaire
  vw_set_rx_pin(RX_PIN);
  vw_setup(2000);
  vw_rx_start(); // On peut maintenant recevoir des messages

  if(DEBUG) {// Test de la configuration du numéro de téléphone
    Serial.print("**DEBUG :: Phone number :");
    Serial.print(phoneNumber);
    Serial.println(".");
    Serial.print("**DEBUG :: Pin number :");
    Serial.print(pinNumber);
    Serial.println(".");
  }

  consigne = eepromReadSavedConsigne(); //Récupération de la consigne enregistrée
  sendStatus(); //Envoie un SMS avec le statut
}

/*LOOP************************************************************************/
void loop() {
  if (gsm.available() > 0) {
    textMessage = gsm.readString();
    if (DEBUG) {
      Serial.println(textMessage);
    }
    //Cas nominal avec le numéro de tel par défaut
    if ( (textMessage.indexOf(phoneNumber)) < 10 && textMessage.indexOf(phoneNumber) > 0) { // SMS arrived
      readSMS(textMessage);
    }
    //Permet de prendre la main pour le controle du système
    else if (textMessage.indexOf(pinNumber) < 51 && textMessage.indexOf(pinNumber) > 0) {
      int indexOfPhoneNumber = textMessage.indexOf("+",5);
      int finalIndexOfPhoneNumber = textMessage.indexOf("\"", indexOfPhoneNumber);
      String newPhoneNumber = textMessage.substring(indexOfPhoneNumber,finalIndexOfPhoneNumber);
      String information = "Nouveau numero enregistre : ";
      information.concat(newPhoneNumber);
      sendMessage(information);
      phoneNumber=newPhoneNumber;
      if (DEBUG) {
        Serial.print("First index : ");
        Serial.println(indexOfPhoneNumber);
        Serial.print("Last index : ");
        Serial.println(finalIndexOfPhoneNumber);
        Serial.print("New Phone number : ");
        Serial.println(phoneNumber);
      }
      readSMS(textMessage);
    }
  }

  //Vérifier le ping timeout, le niveau de batterie, envoie des alertes.
  checkThermostat();

  //Fonction de chauffage
  heatingProcess();

  //Attente paquet du thermostat, timout 8000 ms
  listen(THERMOSTAT_LISTENING_TIME);
}

/*FUNCTIONS*******************************************************************/

void activatePerso(){
  digitalWrite(RELAYS_COMMON, HIGH); // Déconnecter le commun
  delay(200);
  digitalWrite(RELAYS_PERSO, HIGH); // Connecter le perso
  heating = ENABLED;
  Serial.println("Perso activé");
}

void desactivatePerso() {
  digitalWrite(RELAYS_COMMON, LOW); // Déconnecter le perso
  delay(200);
  digitalWrite(RELAYS_PERSO, LOW); // Connecter le commun
  heating = DISABLED;
  Serial.println("Perso désactivé");
}

void heatingProcess() {
  int bijunction = getBijunctionState();

  if ((bijunction == ENABLED) && (currentBijunctionState == DISABLED) && (heating == ENABLED)) { // si commun present et état précedent éteint
    // Si le chauffage est en cours sur le perso, il faut le couper
    desactivatePerso();
    currentBijunctionState = ENABLED;
  }
  else if ((bijunction == DISABLED) && (currentBijunctionState == DISABLED) && (program == ENABLED) ) {
    heatingProg();
  }
  else if ((bijunction == DISABLED) && (forcedHeating == ENABLED) && (heating == DISABLED)) {
    activatePerso();
  }

  if ((bijunction == DISABLED) && (currentBijunctionState == ENABLED)) {
    //désactiver le chauffage par le commun, pas de changement d'état des relais (commun connecté, perso déconnecté)
    currentBijunctionState = DISABLED;
  }
  
  if ((forcedHeating == DISABLED) && (heating == ENABLED)) {
    //désactiver le chauffage manuel forcé
    desactivatePerso();
    heating = DISABLED;
  }
}

void checkThermostat() {
  // Mise à jour du chrono
  lastRefresh = (int)((millis() - lastTempMeasureMillis) / 60000);

  /*Permet d'envoyer un unique message (jusqu'au redémarrage) si le signal n'a pas
    été reçu depuis 30min*/
    if (!alertNoSignalSent) {
      if (lastRefresh > 30) {// si plus mis à jour depuis 30min désactivation de la prog
        program = DISABLED; // Coupe la programmation pour empécher de rester en chauffe indéfiniement
        alertNoSignalSent = true;
        sendMessage("Plus de signal du thermostat.");
      }
    }
  /*Permet d'envoyer un unique message jusqu'au redémarrage; si le niveau de batterie
  du termostat passs en dessous de 20 %*/
    if(!alertBatteryLowSent) {
      if (batteryLevel < 20) {
        alertBatteryLowSent = true;
        sendMessage("Niveau de batterie faible.");
      }
    }
  /*Permet d'envoyer un unique message jusqu'au redémarrage; si le niveau de batterie
    du termostat passs en dessous de 10 %*/
    if (!alertBatteryCriticalSent) {
      if (batteryLevel < 10) {
        alertBatteryCriticalSent = true;
      sendMessage("Niveau de batterie critique.");
      }
    }
}

void listen(int timeout) {
  vw_wait_rx_max(timeout);
   // On copie le message, qu'il soit corrompu ou non
   if (vw_have_message()) {//Si un message est pret a etre lu
     if (vw_get_message((byte *) &receivedData, &messageSize)) { // Si non corrpompu
       lastTempMeasureMillis = millis();
       temperature = receivedData.temp;
       batteryLevel = receivedData.batt;
       if (DEBUG) {
         Serial.print("Temp transmise : ");
         Serial.println(temperature); // Affiche le message
         Serial.print("Batt transmise : ");
         Serial.println(batteryLevel);
         Serial.println();
       }
     }
     else if (DEBUG) {
       Serial.println("Message du thermostat corrompu.");
     }
   }
 }


void readSMS(String textMessage) {
  const char* commandList[] = {"Ron", "Roff", "Status", "Progon", "Progoff", "Consigne"};
  int command = -1;

  for(int i = 0; i<6; i++) {
    if (textMessage.indexOf(commandList[i]) > 0) {
      command = i;
      index = textMessage.indexOf(commandList[i]);
      break; //Permet de sortir du for des que le cas est validé
    }
  }
  switch (command) {
    case 0: // Ron
    turnOn();
    break;
    case 1: // Roff
    turnOff();
    break;
    case 2: // Status
    sendStatus();
    break;
    case 3: // Progon
    program = ENABLED;
    sendMessage("Programme actif");
    digitalWrite(LED_PIN, HIGH);
    break;
    case 4: // Progoff
    program = DISABLED;
    sendMessage("Programme inactif");
    digitalWrite(LED_PIN, LOW);
    turnOff();
    break;
    case 5: // Consigne
    setConsigne(textMessage, index);
    break;
    default:
    break;
  }
}

void sendMessage(String message) {//Envoi du "Message" par sms
gsm.print("AT+CMGS=\"");
gsm.print(phoneNumber);
gsm.println("\"");
delay(500);
gsm.print(message);
gsm.write( 0x1a ); //Permet l'envoi du sms
}

void setConsigne(String message, int indexConsigne) {//Réglage de la consigne contenue dans le message à l'indexConsigne
newConsigne = message.substring(indexConsigne + 9, message.length()).toFloat(); // On extrait la valeur et on la cast en float // 9 = "Consigne ".length()
Serial.print("nouvelle consigne :");
Serial.println(newConsigne);
if (!newConsigne) {// Gestion de l'erreur de lecture et remontée du bug
if (DEBUG) {
  Serial.println("Impossible d'effectuer la conversion de la température String -> Float. Mauvais mot-clé? Mauvais index?");
  Serial.print("indexConsigne = ");
  Serial.println(indexConsigne);
  Serial.print("consigne lenght (>0)= ");
  Serial.println(message.length()- indexConsigne + 9);
  Serial.print("newConsigne = ");
  Serial.println(newConsigne);
} else {
  sendMessage("Erreur de lecture de la consigne envoyee");
}
} else if (consigne != newConsigne) { //Si tout se passe bien et la consigne est différente la consigne actuelle
  consigne = newConsigne;
  message = "La nouvelle consigne est de ";
  message.concat(consigne);
  sendMessage(message);
  eepromWriteData(consigne);//Enregistrement dans l'EEPROM
} else {
  sendMessage("Cette consigne est deja enregistree");
}
}

void heatingProg(){//Vérification de le temp, comparaison avec la consigne, et activation/désactivation en fonction
  if ((temperature < (consigne - 0.5*hysteresis)) && (heating == DISABLED)) {
    //Activer le chauffage par le perso
    activatePerso();    
  }
  if ((temperature > (consigne + 0.5*hysteresis)) && (heating == ENABLED)) {
    //Désactiver le chauffage par le perso
    desactivatePerso();
  }
}

void turnOn() {//allumage du radiateur si pas de consigne et envoi de SMS
  gsm.print("AT+CMGS=\"");
  gsm.print(phoneNumber);
  gsm.println("\"");
  delay(500);
  if (program) {
    gsm.println("Le programme est toujours actif !!");
  } else {
    // Turn on RELAYS_PERSO and save current state
    forcedHeating = ENABLED;
    gsm.println("Chauffage en marche.");
  }
  gsm.write( 0x1a ); //Permet l'envoi du sms
}

void turnOnWithoutMessage() {//allumage du radiateur si pas de consigne
  // Turn on RELAYS_PERSO and save current state
  forcedHeating = DISABLED;
}

void turnOff() {//Extinction du rad si pas de consigne et envoie de SMS
  gsm.print("AT+CMGS=\"");
  gsm.print(phoneNumber);
  gsm.println("\"");
  delay(500);
  if (program) {
    gsm.println("Le programme est toujours actif !!");
  } else {
    // Turn off RELAYS_PERSO and save current state
    gsm.println("Le chauffage est eteint.");
    forcedHeating = DISABLED;
  } //Emet une alerte si le programme est toujours actif
  gsm.write( 0x1a ); //Permet l'envoi du sms
}

void turnOffWithoutMessage() {//Extinction du rad si pas de consigne
  // Turn on RELAYS_PERSO and save current state
  forcedHeating = DISABLED;
}

//Renvoie la date
String getDate() {
  //Ce code concatène dans "date" la date et l'heure courante
  //dans le format 20YY MM DD HH:MM:SS
  String date ="";
  date +="2";
  date +="0";
  date += String(Clock.getYear());
  date += " ";
  date += String(Clock.getMonth(Century));
  date += " ";
  date += String(Clock.getDate());
  date += " ";
  date += String(Clock.getHour(h12, PM));
  date += ":";
  date += String(Clock.getMinute());
  date += ":";
  date += String(Clock.getSecond());

  //Ce code est exécuté si la variable DEBUG est a TRUE
  //Permet d'afficher dans la console la date et l'heure au format
  // YYYY MM DD w HH MM SS 24h
  if (DEBUG) {
    Serial.print("**DEBUG :: getDate()\t");
    Serial.print("2");
    if (Century) {      // Won't need this for 89 years.
    Serial.print("1");
  } else {
    Serial.print("0");
  }
  Serial.print(Clock.getYear(), DEC);
  Serial.print(' ');
  Serial.print(Clock.getMonth(Century), DEC);
  Serial.print(' ');
  Serial.print(Clock.getDate(), DEC);
  Serial.print(' ');
  Serial.print(Clock.getDoW(), DEC);
  Serial.print(' ');
  Serial.print(Clock.getHour(h12, PM), DEC);
  Serial.print(' ');
  Serial.print(Clock.getMinute(), DEC);
  Serial.print(' ');
  Serial.print(Clock.getSecond(), DEC);
  if (h12) {
    if (PM) {
      Serial.print(" PM ");
    } else {
      Serial.print(" AM ");
    }
  } else {
    Serial.println(" 24h ");
  }
}
return date;
}

//Envoie par SMS le statut
void sendStatus() { //TODO: ajouter le niveau de batterie
  gsm.print("AT+CMGS=\"");
  gsm.print(phoneNumber);
  gsm.println("\"");
  delay(500);
  gsm.print("Le chauffage est actuellement ");
  gsm.println(heating ? "ON" : "OFF"); // This is to show if the light is currently switched on or off
  gsm.print("Temp: ");
  gsm.print(temperature);
  gsm.print(" *C (");
  gsm.print(lastRefresh);
  gsm.print(" min ago, batt: ");
  gsm.print(batteryLevel);
  gsm.println("%)");
  gsm.print("Consigne: ");
  gsm.println(consigne);
  gsm.println(getDate());
  gsm.write( 0x1a );
}

//Ecrtiture par byte dans l'EEPROM
void i2c_eeprom_write_byte( int deviceaddress, unsigned int eeaddress, byte data ) {
  int rdata = data;
  Wire.beginTransmission(deviceaddress);
  Wire.write((int)(eeaddress >> 8)); // MSB
  Wire.write((int)(eeaddress & 0xFF)); // LSB
  Wire.write(rdata);
  Wire.endTransmission();
}

//Lecture par Byte dans l'EEPROM
byte i2c_eeprom_read_byte( int deviceaddress, unsigned int eeaddress ) {
  byte rdata = 0xFF;
  Wire.beginTransmission(deviceaddress);
  Wire.write((int)(eeaddress >> 8)); // MSB
  Wire.write((int)(eeaddress & 0xFF)); // LSB
  Wire.endTransmission();
  Wire.requestFrom(deviceaddress,1);
  if (Wire.available()) rdata = Wire.read();
  return rdata;
}

//Ecriture de la value dans l'EEPROM
void eepromWriteData(float value) {
  String stringValue = String(value);
  int valueLength = sizeof(stringValue);
  if (valueLength < 32) { //A priori il existe une limite, voir dans AT24C32_examples
    if (0) {// ATTENTION: génère une erreur quand actif
      Serial.print("**DEBUG :: eepromWriteData()\t");
      Serial.print("Longueur de la consigne : ");
      Serial.println(valueLength-1);
      Serial.print("Valeur de la consigne : ");
      Serial.println(stringValue);
    }
    for (int i = 0; i < valueLength - 1; i++) { // -1 pour ne pas récupérer le \n de fin de string
      i2c_eeprom_write_byte(0x57, i, stringValue[i]);
      delay(10);
    }
  }
}

//Renvoie la valeur de la consigne lue dans l'EEPROM
float eepromReadSavedConsigne() {
  String value;
  for(int i=0;i<5;i++) // la valeur sera "normalement" toujours 5 pour une consigne
  {
    int b = i2c_eeprom_read_byte(0x57, i); //access an address from the memory
    value += char(b);
  }
  if (0) {
    Serial.print("**DEBUG :: eepromReadSavedConsigne()");
    Serial.print("\tRead value: "); //ATTENTION : le 18/5/19 ce message provoquait une erreur quand il était plus long
    Serial.println(value);
  }
  return value.toFloat();
}

int getBijunctionState() {
  return (digitalRead(BIJUNCTION_PIN) ? DISABLED : ENABLED);
  //si le détecteur renvoie 1 (non présent), la fonction renvoie DISABLED (return 0)
  //Si le détecteur renvoie 0 (présent), la fonction renvoie ENABLED (return 1)
}
