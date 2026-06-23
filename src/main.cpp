#include <Arduino.h>
#include <string>
#include "freertos/FreeRTOS.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <esp_now.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

// Pinos
#define SENSOR1 25
#define SENSOR2 26
#define SENSOR3 14
#define BUZZER 32
#define RESET GPIO_NUM_0

#define BUZZER_CHANNEL 0
#define TamEEPROM 64

enum SensorState
{
  WAIT_FIRST_EDGE,
  WAIT_NEXT_EDGE
};

typedef struct
{
  int PIN;
  int awardValue;
} SensorLDR;

SensorLDR s1 = {SENSOR1, 0};
SensorLDR s2 = {SENSOR2, 1};
SensorLDR s3 = {SENSOR3, 2};

uint8_t peerAddress[] = {
    0xD8, 0x13, 0x2A, 0x74, 0x28, 0xBC};

// quantidade de cada premio ganho
int quant1 = 0, quant2 = 0, quant3 = 0;
// Variável que mostra os prêmios ganhos pelo usuário
std::string stringDisplay;
// inicializando a quantidade restante de premios
int quantPremios = 0;
// cooldown leitura do sensor
TickType_t cooldownTicks = pdMS_TO_TICKS(1000);

SemaphoreHandle_t xStringDisplayMutex;

// Task que escreve a string que vai pro display
TaskHandle_t xEscreve;
// Task que controla o display
TaskHandle_t xDisplay;
// Task que toca a melodia
TaskHandle_t xBuzzer;
// Task que reseta o sistema
TaskHandle_t xReset;

void vSensor(void *pvParameters);
void vEscreve(void *pvParameters);
void vDisplay(void *pvParameters);
void vBuzzer(void *pvParameters);
void vReset(void *pvParameters);
void IRAM_ATTR botao_reset_handler(void *arg);

void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len);

void setup()
{
  Serial.begin(9600);

  // MAC ARMA D8:13:2A:74:28:BC
  // MAC BARRACA D4:E9:F4:BC:8E:A4

  //configurando interrupção
  gpio_config_t io_reset_conf = {
        .pin_bit_mask = (1ULL << RESET),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE   // interrupção na borda de descida
    };

    gpio_config(&io_reset_conf);

    gpio_install_isr_service(0);

    gpio_isr_handler_add(RESET, botao_reset_handler, NULL);

  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK)
  {
    Serial.println("Erro ao iniciar ESP-NOW");
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, peerAddress, sizeof(peerAddress));
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK)
  {
    Serial.println("Erro ao adicionar peer");
  }

  esp_now_register_recv_cb(OnDataRecv);
  /*
   */
  ledcSetup(BUZZER_CHANNEL, 1000, 8);
  ledcAttachPin(BUZZER, BUZZER_CHANNEL);

  lcd.init();
  lcd.backlight();

  EEPROM.begin(TamEEPROM);
  quantPremios = EEPROM.readInt(0);

  // Na primeira inicialização o sistema

  if (quantPremios <= 0 || quantPremios > 100)
  {
    quantPremios = 100;
    EEPROM.writeInt(0, quantPremios);
    EEPROM.commit();
  }

  lcd.setCursor(0, 0);
  lcd.print("Iniciar!!!");

  lcd.setCursor(0, 1);
  lcd.print("Premios: " + String(quantPremios));

  Serial.println("Quantidade de prêmios inicializada: ");
  Serial.println(quantPremios);

  pinMode(s1.PIN, INPUT);
  pinMode(s2.PIN, INPUT);
  pinMode(s3.PIN, INPUT);

  xStringDisplayMutex = xSemaphoreCreateMutex();

  xTaskCreate(vSensor, "Sensor 1", 2048, &s1, 1, NULL);
  xTaskCreate(vSensor, "Sensor 2", 2048, &s2, 1, NULL);
  xTaskCreate(vSensor, "Sensor 3", 2048, &s3, 1, NULL);
  xTaskCreate(vEscreve, "Escreve", 2048, NULL, 1, &xEscreve);
  xTaskCreate(vDisplay, "Display", 2048, NULL, 1, &xDisplay);
  xTaskCreate(vBuzzer, "Buzzer", 2048, NULL, 1, &xBuzzer);
  xTaskCreate(vReset, "Reset", 2048, NULL,1, &xReset);
}

void loop()
{
  // put your main code here, to run repeatedly:
}

void vSensor(void *pvParameters)
{
  SensorLDR *s = (SensorLDR *)pvParameters;

  const TickType_t minPeriod = pdMS_TO_TICKS(15);
  const TickType_t maxPeriod = pdMS_TO_TICKS(25);

  const int edgesNeeded = 5;

  SensorState state = WAIT_FIRST_EDGE;
  bool lastVal = digitalRead(s->PIN);

  TickType_t lastEdgeTick = 0;
  TickType_t lastTriggerTick = 0;

  int edgeCount = 0;

  while (1)
  {
    TickType_t now = xTaskGetTickCount();
    bool val = digitalRead(s->PIN);

    switch (state)
    {
    case WAIT_FIRST_EDGE:
      if ((now - lastTriggerTick) > cooldownTicks && lastVal == HIGH && val == LOW)
      {
        //Serial.printf("Detectou primeira borda\n");
        lastEdgeTick = now;
        edgeCount = 1;
        state = WAIT_NEXT_EDGE;
      }
      break;
    case WAIT_NEXT_EDGE:
      if (val != lastVal)
      {
        TickType_t dt = now - lastEdgeTick;

        if (dt >= minPeriod && dt <= maxPeriod)
        {
          edgeCount++;
          //Serial.printf("Borda %d, intervalo = %u ms\n", edgeCount, pdTICKS_TO_MS(dt));

          if (edgeCount >= edgesNeeded)
          {
            Serial.println("Tiro detectado");
            lastTriggerTick = now;
            xTaskNotify(xEscreve, s->awardValue, eSetValueWithOverwrite);
            xTaskNotifyGive(xBuzzer);
            edgeCount = 0;
            state = WAIT_FIRST_EDGE;
          }

          lastEdgeTick = now;
        }
        else
        {
          //Serial.printf("Intervalo invalido: %u ms\n", pdTICKS_TO_MS(dt));
          edgeCount = 0;
          state = WAIT_FIRST_EDGE;
        }
      }
      break;
    }

    lastVal = val;

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
// variável para pausa quando fizer 3 acertos
int stop = 0;

std::string StringBuffer(uint32_t valor)
{
  stop = 0;
  char temp[32];
  std::string buffer = "  GANHOU ";

  if (quant1 + quant2 + quant3 >= 3)
  {
   stop = 1;
  }
  
  if (valor == 3)
  {
    buffer = "Inicar!!!";
  }
  

  if (stop != 1)
  {
    switch (valor)
    {
    case 0:
      quant1++;
      break;
    case 1:
      quant2++;
      break;
    case 2:
      quant3++;
      break;
    }
  }
  

  

  
  if (quant1 > 0)
  {
    snprintf(temp, sizeof(temp), "%d XILITO ", quant1);
    buffer += temp;
  }
  if (quant2 > 0)
  {
    snprintf(temp, sizeof(temp), "%d PIRULITO ", quant2);
    buffer += temp;
  }
  if (quant3 > 0)
  {
    snprintf(temp, sizeof(temp), "%d CHICLETE ", quant3);
    buffer += temp;
  }
  

  return buffer;
}

void vEscreve(void *pvParameters)
{
  uint32_t valor;
  while (1)
  {
    xTaskNotifyWait(0, 0, &valor, portMAX_DELAY);
    std::string buffer = StringBuffer(valor);
    xSemaphoreTake(xStringDisplayMutex, portMAX_DELAY);
    stringDisplay = buffer;

    if (valor < 3)
    {
      stringDisplay += stringDisplay;
    }
    

    xSemaphoreGive(xStringDisplayMutex);
    if (quantPremios > 0 and stop == 0)
      quantPremios -= 1;
    EEPROM.writeInt(0, quantPremios);
    EEPROM.commit();

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void vDisplay(void *pvParameters)
{
  std::string texto;
  int pos = 0;

  while (1)
  {
    xSemaphoreTake(xStringDisplayMutex, portMAX_DELAY);
    texto = stringDisplay;
    xSemaphoreGive(xStringDisplayMutex);

    if (texto.empty())
    {
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }

    lcd.setCursor(0, 0);
    lcd.print(texto.substr(pos, 16).c_str());
    pos++;
    lcd.setCursor(0, 1);
    lcd.print("Premios: " + String(quantPremios));
    if (pos == stringDisplay.size() / 2)
      pos = 0;

    vTaskDelay(pdMS_TO_TICKS(300));
  }
}

void vBuzzer(void *pvParameters)
{

  const int melody[] = {
      784, 988, 1319, 1568};

  const int duration[] = {
      80, 80, 80, 120};

  while (1)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    for (int i = 0; i < 4; i++)
    {
      ledcWriteTone(BUZZER_CHANNEL, melody[i]);

      vTaskDelay(pdMS_TO_TICKS(duration[i]));

      ledcWriteTone(BUZZER_CHANNEL, 0);

      vTaskDelay(pdMS_TO_TICKS(25));
    }
  }
}

void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len)
{
  if (strcmp((char *)data, "ACABOU_MUNICAO") == 0)
  {
    Serial.println("Acabou a munição da arma");
  }
}
//interrupção para ativar o Reset
void IRAM_ATTR botao_reset_handler(void *arg){
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  vTaskNotifyGiveFromISR(xReset, &xHigherPriorityTaskWoken);

  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

uint8_t msg = 1;

void vReset( void *pvParameters){
  while (true)
  { 
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // resetando quantidade de cada premio
    quant1 = 0, quant2 = 0, quant3 = 0;
    xTaskNotify(xEscreve, 3, eSetValueWithoutOverwrite);
    Serial.println("Resetou");
    esp_now_send(peerAddress, (uint8_t*)&msg, sizeof(msg));
  }
}