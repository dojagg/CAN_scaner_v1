#include <Arduino.h>                                  // подключение необходимых библиотек
#include <driver/twai.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>

#define RX_PIN 4                                     // пины для подключения CAN-шины
#define TX_PIN 5

#define RPM 0x0C                                     // PID-запросы для получения параметров
#define COOL_TEMP 0x05
#define THROTTLE_POS 0x11
#define VOLT 0x42
#define LOAD 0x04
#define DIST 0x31

#define DTC "GET_DTC"                                // запрос на получение кодов неисправностей
#define VIN "GET_VIN"                                // запрос на получение VIN-номера 
#define CLEAR "CLEAR_DTC"                            // запрос на удаление кодов неисправностей

int cur_rpm;                                         // переменные для каждого параметра
int cur_cool_temp;
int cur_throttle_pos;
float cur_volt;
int cur_dist;
int cur_load;

const u_int8_t pids[] = {RPM, COOL_TEMP, THROTTLE_POS, LOAD, VOLT, DIST}; 
const int pid_amount = sizeof(pids)/sizeof(pids[0]);

unsigned long requestTime = 0;                       // время ответа от ЭБУ
unsigned long lastSendTime = 0;                      // время отправки последнего запроса 
int state = 0;                                       // состояние системы(1-ожидание ответа от ЭБУ / 0-формирование сообщения для отправки)
int pidInd = 0;                          
                                      
bool requestVIN = false;      
String vin_code;

int dtcStage = 0;
bool requestDTC = false;
String dtc;

bool requestClear = false;

String isotp_buffer;                                   // строка байтов
int isotp_total_len;                                   // длина кадра 
int isotp_received_len;                                // полученное кол-во байт


AsyncWebServer server(80);                             
AsyncWebSocket ws("/ws");


String decodeDTC(uint8_t byteA, uint8_t byteB)         // функция декодирования DTC
{
  String local_dtc;
  uint8_t type = (byteA >> 6) & 0x03;                  // 00 - P (Powertrain), 01 - C (Chassis), 10 - B (Body), 11 - U (Network)
  if(type== 0) local_dtc+= "P";
  else if(type == 1) local_dtc+= "C";
  else if(type == 2) local_dtc+= "B";
  else if(type == 3) local_dtc+= "U";

  local_dtc+= String((byteA>>4) & 0x03);

  local_dtc += String(byteA & 0x0F, HEX);

  local_dtc += String((byteB >> 4) & 0x0F, HEX);

  local_dtc += String(byteB & 0x0F, HEX);
  
  local_dtc.toUpperCase();
  return local_dtc;
}
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t *data, size_t len)  
{                                                     // обработчик событий 
  if(type == WS_EVT_DATA)
  {
    String message;
    for(int i=0; i < len; i++)
    message += (char)data[i];

    if(message == DTC)
    {  
      requestDTC = true;
      dtcStage =1;
    }
    else if(message == VIN)
      requestVIN = true;
    else if(message == CLEAR)
      requestClear = true;
  }
}
void sendDTC(String codes, uint8_t mode)                  // отправка DTC на веб-сервер для вывода
{
  String json_response =""; 
  if(mode == 0x43 || mode == 0x03)                        // активные ошибки
    json_response = "{\"active\":\""+ codes+"\"}";
  else if(mode == 0x44 || mode == 0x04)                   // удаленные ошибки
    json_response = "{\"cleared\":\""+ codes+"\"}";
  else if(mode == 0x47 || mode == 0x07)
    json_response = "{\"pending\":\""+ codes+"\"}";       // временные ошибки
  else if(mode == 0x4A || mode == 0x0A)
    json_response = "{\"permanent\":\""+ codes+"\"}";     // постоянные ошибки
  else
    json_response = "{\"dtc\":\"" + codes + "\"}";         // отправка сообщений в неизвестном режиме или таймаута 
  ws.textAll(json_response);
}
void sendVIN(String vin_code)                              // функция формирования json VIN-номера для отправки на веб-сервер
{
  String json_response = "{\"vin\":\""+ vin_code +"\"}";
  ws.textAll(json_response);
}
void sendData()                                            // формирование строки из данных по PID запросам
{
  if (millis() - lastSendTime < 200) return;
  lastSendTime = millis();

  char json_buffer[128];
  snprintf(json_buffer, sizeof(json_buffer),
           "{\"rpm\":%d,\"cool\":%d,\"throttle\":%d,\"dist\":%d,\"load\":%d,\"volt\":%f}",
           cur_rpm, cur_cool_temp, cur_throttle_pos, cur_dist, cur_load, cur_volt);
                
  ws.textAll(json_buffer);                                 // отправка полученной строки
}
void setup() 
{
  LittleFS.begin(true);                                   // инициализация файловой системы
  WiFi.softAP("Scanner", "11111111");                     // создание точки доступа
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html"); 
  server.addHandler(&ws);
  ws.onEvent(onWsEvent);
  server.begin();                                         // запуск веб-сервера


  // инициализация конфигурации в которой будет работать сканер 
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)TX_PIN, (gpio_num_t)RX_PIN, TWAI_MODE_NORMAL); 
  
  // Настройка скорости (500 kbps)
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  
  twai_filter_config_t f_config;
  f_config.acceptance_code = (0x7E8 << 21);                // аппаратный фильтр CAN: пропускаем только сообщения с ID 0x7E8 (ответы от ЭБУ)
  f_config.acceptance_mask = ~(0x7FF << 21);
  f_config.single_filter = true;

  twai_driver_install(&g_config, &t_config, &f_config);
  twai_start();
 
}

void loop() 
{
  ws.cleanupClients();

  twai_message_t message;
  if(state == 0)                                    // этап формирования сообщения для отправки в ЭБУ
  {
    message.identifier = 0x7DF;
    message.data_length_code = 8;
    message.extd = 0;
    message.rtr = 0;
    for(int i = 0; i < 8; i++)
        message.data[i] = 0x00;

    if(requestDTC == true)                        // формирования запроса на получение DTC
    { 
      if(dtcStage ==1)                            // если это первый кадр с ошибками(макс. 3 ошибки)
      {
        message.data[0] = 0x01;
        message.data[1] = 0x03;
      }
      else if(dtcStage == 2)                      // последующий кадр с ошибками
      {
        message.data[0] = 0x01;
        message.data[1] = 0x07;
      }
    }
    else if(requestVIN == true)                  // формирования запроса на получение VIN-номера
    {
      message.data[0] = 0x02;
      message.data[1] = 0x09;
      message.data[2] = 0x02;
    }
    else if(requestClear == true)                // формирования запроса на удаление DTC
    {
      message.data[0] = 0x01;
      message.data[1] = 0x04; 
    }
    else                                         // формирования запроса на получение параметров авто
    {
    message.data[0] = 0x02;
    message.data[1] = 0x01;
    message.data[2] = pids[pidInd];
    }

    twai_transmit(&message, 0);
    requestTime = millis();
    state = 1;
  }
  else if(state == 1)                                      // этап расшифровки ответа от ЭБУ
  {
    if(twai_receive(&message, pdMS_TO_TICKS(1)) == ESP_OK)
    {
      if(message.data[0] < 0x10)                                       // для одиночного кадра
      { 
        if(message.data[1] == 0x41)                                    // режим для PID запросов
        {
        switch (message.data[2])
        { 
          case RPM:                                                    // расшифровка значения оборотов двигателя
            cur_rpm = ((message.data[3]*256)+message.data[4])/4;
            break;
        
          case COOL_TEMP:                                               // расшифровка значения темп. ОЖ
            cur_cool_temp = message.data[3] - 40;
            break;
 
          case THROTTLE_POS:                                            // расшифровка значения положения дросселя
            cur_throttle_pos = (message.data[3] * 100)/255;
            break;

          case LOAD:                                                     // расшифровка значения нагрузки на двигатель
            cur_load = (message.data[3] * 100)/255;
            break;
          
          case DIST:                                                     // расшифровка значения пройденной дистанции после последнего удаления DTC
            cur_dist = (message.data[3] * 256)+message.data[4];
            break;

          case VOLT:                                                     // расшифровка значения напряжения бортовой сети
            cur_volt = ((message.data[3] * 256)+message.data[4]) / 1000.0;
            break;

          default:
            break;
        }
        pidInd = (pidInd +1)%pid_amount;
        state = 0;
        sendData();
       }
       else if(message.data[1] == 0x44)                                // удаление ошибок 
       {
        sendDTC("Ошибки успешно удалены!", 0x44);
        requestClear = false;
        state = 0;
       }
       else if(message.data[1] == 0x47)                               // режим чтения временных ошибок 
       {
        if(message.data[2] == 0x00 && message.data[3] == 0x00)        // нет ошибок
        {
          if(dtc == "")
            dtc = "Ошибок не обнаружено!";
        }
        else                                                          // есть ошибки, но строго <= 3
        {
          if(dtc != "" && dtc != "Ошибок не обнаружено!")
            dtc += ", ";
          else
            dtc = ""; 

          dtc = decodeDTC(message.data[2], message.data[3]);
          if(message.data[4] != 0x00 && message.data[5] != 0x00)
            dtc+= ", " + decodeDTC(message.data[4], message.data[5]);
          if(message.data[6] != 0x00 && message.data[7] != 0x00)
            dtc+= ", " + decodeDTC(message.data[6], message.data[7]);
        }
        sendDTC(dtc, 0x47);
        requestDTC = false;
        dtcStage =0;
        state = 0;
       }
       else if(message.data[1] == 0x7F && message.data[2] == 0x07)     // также нет ошибок(обработка другого варианта ответа от ЭБУ)
       {
        if(dtc == "")
          dtc = "Ошибок не обнаружено!";
        
        sendDTC(dtc, 0x47);
        requestDTC = false;
        dtcStage = 0;
        state = 0;
       }
       else if(message.data[1] == 0x7F && message.data[2] == 0x03)    // нет активных ошибок - начинаем читать временные 
       {
        dtcStage = 2;
        state = 0;
       }
       else if(message.data[1] == 0x43)                               // режим чтения кодов неисправностей
       {
        dtc = "";
        if(message.data[2] == 0x00 && message.data[3] == 0x00)        // нет ошибок
          dtc = "Ошибок не обнаружено!";
          
        else                                                          // есть ошибки, но строго <= 3
        {
          dtc = decodeDTC(message.data[2], message.data[3]);
          if(message.data[4] != 0x00 && message.data[5] != 0x00)
            dtc+= ", " + decodeDTC(message.data[4], message.data[5]);
          if(message.data[6] != 0x00 && message.data[7] != 0x00)
            dtc+= ", " + decodeDTC(message.data[6], message.data[7]);
        }
        dtcStage = 2;
        state = 0;
       }
      }
      else if((message.data[0] & 0xF0) == 0x10)                       // для первого кадра большой длины
      {
        isotp_total_len = ((message.data[0] & 0x0F) << 8) + message.data[1]; // расчет длины ответа от ЭБУ
        isotp_buffer = ""; 
        for(int i = 2; i < 8; i++)
          isotp_buffer += (char)message.data[i];                       // заполнение полученной информацией
        isotp_received_len = 6;

        twai_message_t fc_message;                                     // формирование flow control кадра для подтверждения получения
        fc_message.identifier = 0x7E0;                                 // адрес в ЭБУ для запроса
        fc_message.data_length_code = 8;
        fc_message.extd = 0;
        fc_message.rtr = 0x00;
        fc_message.data[0] = 0x30;                                     // отправка кадра flow control
        fc_message.data[1] = 0x00;
        for(int i =2; i < 8; i++)
          fc_message.data[i] = 0x00;

        twai_transmit(&fc_message, 0);                                // отправка сообщения
      }
      else if((message.data[0] & 0xF0) == 0x20)                       // для последующего кадра
      {
        int remaining = isotp_total_len - isotp_received_len;         // сколько всего байт осталось
        int bytes_to_copy;                                            
        if(remaining > 7)
          bytes_to_copy = 7;
        else
          bytes_to_copy = remaining;

        for(int i =1; i <= bytes_to_copy; i++)                          // заполняем буфер данными из сообщения
        {
          isotp_buffer += (char)message.data[i];
          isotp_received_len++;
        }

        if(isotp_received_len >= isotp_total_len)                       // если получили все байты
        {
          if(requestDTC == true)                                       // проверка что запрашивается
          {
            int num_errors = (isotp_total_len - 1)/2;                 // кол-во кодов неисправностей
            dtc = "";
            for(int j =0; j < num_errors; j++)
            {
              if(j > 0)
                dtc += ", ";

              int ind = 1 +(j*2);                                      // расчет индекса первого байта кода неисправности
              dtc += decodeDTC((uint8_t)isotp_buffer[ind], (uint8_t)isotp_buffer[ind+1]);
            }
           sendDTC(dtc, (uint8_t)isotp_buffer[0]);
           requestDTC = false;
          }
          else if(requestVIN == true)                                   // формирование VIN-номера для отправки на веб-сервер
          {
            vin_code = "";
            for(int j = 3; j < isotp_total_len; j++)
              vin_code += (char)isotp_buffer[j];
            sendVIN(vin_code);
            requestVIN = false;
          }
          state = 0;
        }
      }
  }
    else                                             // обработка случаев при долгом ответе от ЭБУ
    {
      if(millis() - requestTime > 300)
      {
        if(requestDTC == true) 
        {        
          requestDTC = false;
          sendDTC("Timeout EBU", 0);
        }
        else if(requestVIN == true) 
        {
          requestVIN = false;
          sendVIN("Timeout EBU"); 
        }
        else if(requestClear == true)
        {
          requestClear = false;
          sendDTC("Timeout EBU", 0);
        }
        pidInd = (pidInd +1)%pid_amount;
        state = 0;
      }
    }
  }

}
