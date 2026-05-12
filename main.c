#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "esp_attr.h"

// ======================================================
// GPIO CONFIGURATION
// ======================================================

#define BUTTON_GPIO 4

// ======================================================
// WATCHDOG TIMEOUT
// ======================================================

#define WATCHDOG_TIMEOUT_SECONDS 8

// ======================================================
// SYSTEM STATES
// ======================================================

typedef enum
{
    SYSTEM_NORMAL,
    SYSTEM_WARNING,
    SYSTEM_CRITICAL,
    SYSTEM_ESTOP

} system_state_t;

// ======================================================
// SENSOR DATA STRUCTURE
// ======================================================

typedef struct
{
    int temperature;
    int gas_level;

} sensor_data_t;

// ======================================================
// LOGGER MESSAGE STRUCTURE
// ======================================================

typedef struct
{
    char message[128];

} log_message_t;

// ======================================================
// GLOBAL VARIABLES
// ======================================================

SemaphoreHandle_t xEmergencySemaphore = NULL;

QueueHandle_t xSensorQueue = NULL;
QueueHandle_t xLogQueue = NULL;

volatile bool emergency_triggered = false;

volatile system_state_t current_state = SYSTEM_NORMAL;

// Watchdog heartbeat timestamps

volatile uint32_t sensor_task_heartbeat = 0;
volatile uint32_t safety_task_heartbeat = 0;
volatile uint32_t logger_task_heartbeat = 0;

// Task handles

TaskHandle_t sensorTaskHandle = NULL;

// ======================================================
// LOGGER HELPER FUNCTION
// ======================================================

void send_log(const char *text)
{
    log_message_t log;

    snprintf(
        log.message,
        sizeof(log.message),
        "%s",
        text
    );

    xQueueSend(
        xLogQueue,
        &log,
        portMAX_DELAY
    );
}

// ======================================================
// ISR - EMERGENCY BUTTON
// ======================================================

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    if (emergency_triggered == false)
    {
        emergency_triggered = true;

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        xSemaphoreGiveFromISR(
            xEmergencySemaphore,
            &xHigherPriorityTaskWoken
        );

        gpio_intr_disable(BUTTON_GPIO);

        if (xHigherPriorityTaskWoken)
        {
            portYIELD_FROM_ISR();
        }
    }
}

// ======================================================
// SENSOR TASK
// ======================================================

void sensor_task(void *pvParameters)
{
    sensor_data_t sensor_data;
    char log_buffer[128];

    while (1)
    {
        // Watchdog heartbeat update
        sensor_task_heartbeat = xTaskGetTickCount();

        // Suspend during ESTOP
        if (current_state == SYSTEM_ESTOP)
        {
            send_log("[SENSOR TASK] Suspended");

            vTaskSuspend(NULL);
        }

        // Simulated sensor values
        sensor_data.temperature = 25 + (rand() % 20);
        sensor_data.gas_level = rand() % 100;

        // Send sensor data
        xQueueSend(
            xSensorQueue,
            &sensor_data,
            portMAX_DELAY
        );

        snprintf(
            log_buffer,
            sizeof(log_buffer),
            "[SENSOR DATA] Temp: %d C | Gas: %d",
            sensor_data.temperature,
            sensor_data.gas_level
        );

        send_log(log_buffer);

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ======================================================
// LOGGER TASK
// ======================================================

void logger_task(void *pvParameters)
{
    log_message_t received_log;

    while (1)
    {
        // Watchdog heartbeat update
        logger_task_heartbeat = xTaskGetTickCount();

        if (xQueueReceive(
                xLogQueue,
                &received_log,
                portMAX_DELAY
            ))
        {
            printf("\n[LOGGER] %s\n", received_log.message);
        }
    }
}

// ======================================================
// SAFETY TASK
// ======================================================

void safety_task(void *pvParameters)
{
    sensor_data_t received_data;

    while (1)
    {
        // Watchdog heartbeat update
        safety_task_heartbeat = xTaskGetTickCount();

        // Emergency interrupt handling
        if (xSemaphoreTake(xEmergencySemaphore, 0))
        {
            current_state = SYSTEM_ESTOP;

            send_log("=====================================");
            send_log("[CRITICAL] EMERGENCY STOP ACTIVATED");
            send_log("[SYSTEM] FAIL-SAFE MODE ACTIVE");
            send_log("[STATUS] SYSTEM LOCKED");

            // Suspend normal tasks
            if (sensorTaskHandle != NULL)
            {
                vTaskSuspend(sensorTaskHandle);
            }

            send_log("[SAFETY TASK] NON-CRITICAL TASKS HALTED");
            send_log("[SYSTEM] SAFE STATE ACHIEVED");
            send_log("=====================================");
        }

        // Ignore sensor processing during ESTOP
        if (current_state == SYSTEM_ESTOP)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Receive sensor data
        if (xQueueReceive(
                xSensorQueue,
                &received_data,
                pdMS_TO_TICKS(100)
            ))
        {
            if (received_data.gas_level > 80)
            {
                current_state = SYSTEM_CRITICAL;

                send_log("[CRITICAL] GAS LEAK DETECTED");
            }

            else if (received_data.temperature > 38)
            {
                current_state = SYSTEM_WARNING;

                send_log("[WARNING] HIGH TEMPERATURE");
            }

            else
            {
                current_state = SYSTEM_NORMAL;

                send_log("[NORMAL] SYSTEM STABLE");
            }

            switch (current_state)
            {
                case SYSTEM_NORMAL:
                    send_log("[STATE] NORMAL");
                    break;

                case SYSTEM_WARNING:
                    send_log("[STATE] WARNING");
                    break;

                case SYSTEM_CRITICAL:
                    send_log("[STATE] CRITICAL");
                    break;

                case SYSTEM_ESTOP:
                    send_log("[STATE] ESTOP");
                    break;
            }
        }
    }
}

// ======================================================
// WATCHDOG TASK
// ======================================================

void watchdog_task(void *pvParameters)
{
    uint32_t current_tick;

    while (1)
    {
        current_tick = xTaskGetTickCount();

        send_log("[WATCHDOG] Monitoring Tasks");

        // Check Sensor Task
        if ((current_tick - sensor_task_heartbeat)
            > pdMS_TO_TICKS(WATCHDOG_TIMEOUT_SECONDS * 1000))
        {
            send_log("[WATCHDOG ERROR] Sensor Task Timeout");
            send_log("[WATCHDOG] System Recovery Required");
        }

        // Check Safety Task
        if ((current_tick - safety_task_heartbeat)
            > pdMS_TO_TICKS(WATCHDOG_TIMEOUT_SECONDS * 1000))
        {
            send_log("[WATCHDOG ERROR] Safety Task Timeout");
            send_log("[WATCHDOG] Critical Failure Detected");
        }

        // Check Logger Task
        if ((current_tick - logger_task_heartbeat)
            > pdMS_TO_TICKS(WATCHDOG_TIMEOUT_SECONDS * 1000))
        {
            send_log("[WATCHDOG ERROR] Logger Task Timeout");
            send_log("[WATCHDOG] Logging System Failure");
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ======================================================
// MAIN APPLICATION
// ======================================================

void app_main()
{
    srand(time(NULL));

    // Create semaphore
    xEmergencySemaphore = xSemaphoreCreateBinary();

    // Create queues
    xSensorQueue = xQueueCreate(
        5,
        sizeof(sensor_data_t)
    );

    xLogQueue = xQueueCreate(
        20,
        sizeof(log_message_t)
    );

    // Configure GPIO

    gpio_reset_pin(BUTTON_GPIO);

    gpio_set_direction(
        BUTTON_GPIO,
        GPIO_MODE_INPUT
    );

    gpio_set_pull_mode(
        BUTTON_GPIO,
        GPIO_PULLUP_ONLY
    );

    gpio_set_intr_type(
        BUTTON_GPIO,
        GPIO_INTR_NEGEDGE
    );

    // Install ISR service

    gpio_install_isr_service(0);

    gpio_isr_handler_add(
        BUTTON_GPIO,
        gpio_isr_handler,
        NULL
    );

    // Create tasks

    xTaskCreate(
        logger_task,
        "Logger_Task",
        4096,
        NULL,
        5,
        NULL
    );

    xTaskCreate(
        safety_task,
        "Safety_Task",
        4096,
        NULL,
        10,
        NULL
    );

    xTaskCreate(
        sensor_task,
        "Sensor_Task",
        4096,
        NULL,
        4,
        &sensorTaskHandle
    );

    xTaskCreate(
        watchdog_task,
        "Watchdog_Task",
        4096,
        NULL,
        6,
        NULL
    );

    // Startup logs

    send_log("=========================================");
    send_log("INDUSTRIAL SAFETY GATEWAY ONLINE");
    send_log("FreeRTOS Multi-Tasking Active");
    send_log("Centralized Logger Enabled");
    send_log("Watchdog Monitoring Enabled");
    send_log("Press RED Button for E-STOP");
    send_log("=========================================");
}
