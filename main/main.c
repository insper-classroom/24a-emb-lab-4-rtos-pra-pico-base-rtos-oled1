#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <semphr.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "ssd1306.h"
#include "gfx.h"

const uint TRIG_PIN = 12;
const uint ECHO_PIN = 13;
const uint MAX_DISTANCE_CM = 400; // Máxima distância em cm que queremos medir

ssd1306_t disp;
#define OLED_WIDTH  128
#define OLED_HEIGHT 32

// Filas e semáforos para sincronização
QueueHandle_t xQueueEchoTimes, xQueueDistance;
SemaphoreHandle_t xSemaphoreTrigger;

static void pin_callback(uint gpio, uint32_t events) {
    static uint64_t start_time_us = 0;
    if (events & GPIO_IRQ_EDGE_RISE) {
        start_time_us = to_us_since_boot(get_absolute_time());
    } else if (events & GPIO_IRQ_EDGE_FALL && start_time_us > 0) {
        uint64_t end_time_us = to_us_since_boot(get_absolute_time());
        uint64_t pulse_duration_us = end_time_us - start_time_us;
        xQueueSendFromISR(xQueueEchoTimes, &pulse_duration_us, NULL);
    }
}

static void trigger_task(void *pvParameters) {
    while (1) {
        gpio_put(TRIG_PIN, 1);
        busy_wait_us_32(10); // Trigger pulse for 10 us
        gpio_put(TRIG_PIN, 0);
        xSemaphoreGive(xSemaphoreTrigger); // Sinaliza que o trigger foi enviado
        vTaskDelay(pdMS_TO_TICKS(1000)); // Espera antes do próximo trigger
    }
}

static void echo_task(void *pvParameters) {
    uint64_t pulse_duration_us;
    while (1) {
        if (xQueueReceive(xQueueEchoTimes, &pulse_duration_us, portMAX_DELAY)) {
            float distance_cm = (float)pulse_duration_us * 0.0343 / 2.0;
            if (distance_cm <= MAX_DISTANCE_CM) {
                xQueueSend(xQueueDistance, &distance_cm, portMAX_DELAY);
            } else {
                float error_val = -1.0f; // Valor de erro ou fora de alcance
                xQueueSend(xQueueDistance, &error_val, portMAX_DELAY);
            }
        }
    }
}

static void oled_task(void *pvParameters) {
    float distance_cm;
    char buffer[32];

    ssd1306_init(); // Assuming initialization details are handled
    gfx_init(&disp, OLED_WIDTH, OLED_HEIGHT);

    while (1) {
        if (xSemaphoreTake(xSemaphoreTrigger, portMAX_DELAY) && xQueueReceive(xQueueDistance, &distance_cm, 0)) {
            gfx_clear_buffer(&disp);
            if (distance_cm >= 0) {
                snprintf(buffer, sizeof(buffer), "Dist: %.2f cm", distance_cm);
            } else {
                snprintf(buffer, sizeof(buffer), "Sensor falhou");
            }
            gfx_draw_string(&disp, 0, 0, 1, buffer);
            int bar_length = (int)((distance_cm / MAX_DISTANCE_CM) * (OLED_WIDTH - 1));
            gfx_draw_line(&disp, 0, OLED_HEIGHT - 10, bar_length, OLED_HEIGHT - 10);

            gfx_show(&disp);
        }
    }
}

int main(void) {
    stdio_init_all();
    gpio_init(TRIG_PIN);
    gpio_set_dir(TRIG_PIN, GPIO_OUT);
    gpio_put(TRIG_PIN, 0);

    gpio_init(ECHO_PIN);
    gpio_set_dir(ECHO_PIN, GPIO_IN);
    gpio_set_irq_enabled_with_callback(ECHO_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &pin_callback);

    xQueueEchoTimes = xQueueCreate(10, sizeof(uint64_t)); // Para duração do pulso em us
    xQueueDistance = xQueueCreate(10, sizeof(float)); // Para distâncias calculadas em cm
    xSemaphoreTrigger = xSemaphoreCreateBinary();

    xTaskCreate(trigger_task, "Trigger Task", 256, NULL, 1, NULL);
    xTaskCreate(echo_task, "Echo Task", 256, NULL, 1, NULL);
    xTaskCreate(oled_task, "OLED Task", 256, NULL, 1, NULL);

    vTaskStartScheduler();

    // A execução nunca deve chegar aqui, o controle é tomado pelo FreeRTOS
    for (;;) {}
    return 0; // Este ponto nunca será alcançado
}
