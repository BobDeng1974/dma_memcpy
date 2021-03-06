/****************************************************************************
*
* Copyright (C) 2017, Jon Magnuson <my.name at google's mail service>
* All Rights Reserved.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along
* with this program; if not, write to the Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*
****************************************************************************/


/* Standard includes. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* Platform includes. */
#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/udma.h"

/* FreeRTOS kernel includes. */
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* Application includes. */
#include "dma_memcpy.h"

/* Size of memcpy buffer */
#define MEM_BUFFER_SIZE         1024

typedef struct TaskParameters_t
{
    SemaphoreHandle_t *pcSemaphores;
    uint32_t buffer[MEM_BUFFER_SIZE];

} TaskParameters;

/* Application task prototypes. */
void prvProducerTask( void *pvParameters );
void prvConsumerTask( void *pvParameters );

/* FreeRTOS function/hook prototypes. */
void vApplicationMallocFailedHook( void );
void vApplicationIdleHook( void );
void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName );
void vApplicationTickHook( void );

#if defined(ccs)
#pragma DATA_ALIGN(pui8ControlTable, 1024)
uint8_t pui8ControlTable[1024];
#else /* gcc */
uint8_t pui8ControlTable[1024] __attribute__ ((aligned(1024)));
#endif

static void init_dma()
{
    /* Enable udma peripheral controller */
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_UDMA);
    ROM_SysCtlPeripheralSleepEnable(SYSCTL_PERIPH_UDMA);

    /* Enable udma error interrupt */
    ROM_IntEnable(INT_UDMAERR);

    /* Enable udma */
    ROM_uDMAEnable();

    /* Set udma control table */
    ROM_uDMAControlBaseSet(pui8ControlTable);

    /* Enable udma interrupts */
    ROM_IntEnable(INT_UDMA);
}

void init_led( void )
{
    /* Enable GPIO port N */
    ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPION);

    /* Enable GPIO pin N0 */
    ROM_GPIOPinTypeGPIOOutput(GPIO_PORTN_BASE, GPIO_PIN_0);
}

static SemaphoreHandle_t isrSemaphore;
static void set_udma_txfer_done(int status){
    if (status==0){
        /* Success */
        xSemaphoreGive(isrSemaphore);
    }
    else {
        /* Failed */
        while (1);
    }
}

int main_rtos( void )
{
    /* Variable declarations */
    static TaskParameters    taskParams      = {NULL, NULL};
    static SemaphoreHandle_t pcSemaphores[2] = {NULL, NULL};

    /* Set up clock for 120MHz */
    MAP_SysCtlClockFreqSet(
        SYSCTL_XTAL_25MHZ
      | SYSCTL_OSC_MAIN
      | SYSCTL_USE_PLL
      | SYSCTL_CFG_VCO_480,
      120000000);

    init_led();

    init_dma();

    init_dma_memcpy(UDMA_CHANNEL_SW);

    pcSemaphores[0] = xSemaphoreCreateBinary();
    pcSemaphores[1] = xSemaphoreCreateBinary();

    taskParams.pcSemaphores = &pcSemaphores[0];


    if (pcSemaphores[0] == NULL || pcSemaphores[1] == NULL)
    {
        return 1;
    }

    static uint32_t task_result = NULL;

    if( task_result =
        xTaskCreate(
            prvProducerTask,
            (portCHAR *)"prvProducerTask",
            configMINIMAL_STACK_SIZE+MEM_BUFFER_SIZE,
            (void*)&taskParams,
            (tskIDLE_PRIORITY+1),
            NULL
        ) != pdTRUE)
    {
        /* Task not created.  Stop here for debug. */
        while (1);
    }

    if( task_result =
        xTaskCreate(
            prvConsumerTask,
            (portCHAR *)"prvConsumerTask",
            configMINIMAL_STACK_SIZE,
            (void*)&taskParams,
            (tskIDLE_PRIORITY+1),
            NULL
            )
        != pdTRUE)
    {
        /* Task not created.  Stop here for debug. */
        while (1);
    }


    vTaskStartScheduler();

    for( ;; );
    return 0;

}

void prvProducerTask( void *pvParameters )
{
    uint32_t src_buffer[MEM_BUFFER_SIZE] = {0};

    SemaphoreHandle_t *sem_array;
    uint32_t          *dest_buffer;
    unsigned int      passes = 0;
    unsigned int      p = 0;

    sem_array =   (SemaphoreHandle_t*)(((TaskParameters*)pvParameters)->pcSemaphores);
    dest_buffer =          (uint32_t*)(((TaskParameters*)pvParameters)->buffer);

    void *cbfn_ptr[2] = {&set_udma_txfer_done, NULL};
    isrSemaphore = xSemaphoreCreateBinary();

    for (;;)
    {
        { /* Scope out the counter */
            uint_fast16_t ui16Idx;

            /* Fill source buffer with pseudo-random numbers */
            for(ui16Idx = 0; ui16Idx < MEM_BUFFER_SIZE; ui16Idx++)
            {
                src_buffer[ui16Idx] = ui16Idx + passes;
            }

        }

        /* Toggle LED */
        ROM_GPIOPinWrite(
            GPIO_PORTN_BASE,
            GPIO_PIN_0,
            ~ROM_GPIOPinRead(GPIO_PORTN_BASE, GPIO_PIN_0)
        );

        dma_memcpy(
            dest_buffer,
            &src_buffer[0],
            MEM_BUFFER_SIZE,
            UDMA_CHANNEL_SW,
            cbfn_ptr[0]
        );

        /* Wait for transfer completion via cb/semaphore */
        xSemaphoreTake(isrSemaphore, portMAX_DELAY);

        /* Signal data ready */
        xSemaphoreGive(sem_array[0]);

        /* Wait for data clear */
        xSemaphoreTake(sem_array[1], portMAX_DELAY);
    }
}

void prvConsumerTask( void *pvParameters )
{
    SemaphoreHandle_t *sem_array;
    uint32_t *buffer;

    sem_array = ((TaskParameters*)pvParameters)->pcSemaphores;
    buffer =    ((TaskParameters*)pvParameters)->buffer;

    for (;;)
    {

        /* Wait for data ready */
        xSemaphoreTake(sem_array[0], portMAX_DELAY);

        /* Toggle LED */
        ROM_GPIOPinWrite(
            GPIO_PORTN_BASE,
            GPIO_PIN_0,
            ~ROM_GPIOPinRead(GPIO_PORTN_BASE, GPIO_PIN_0)
        );

        buffer[0] = 0;

        /* Simulate processing the data */
        //portNOP();
        vTaskDelay(500 / portTICK_RATE_MS);

        /* Signal data clear */
        xSemaphoreGive(sem_array[1]);
    }
}

/*-----------------------------------------------------------*/

void vApplicationMallocFailedHook( void )
{
    //vAssertCalled( __LINE__, __FILE__ );
    __asm("    nop\n");
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook( void )
{
    __asm("    nop\n");
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName )
{
    ( void ) pcTaskName;
    ( void ) pxTask;

}
/*-----------------------------------------------------------*/

void vApplicationTickHook( void )
{
    __asm("    nop\n");
}
/*-----------------------------------------------------------*/

void vAssertCalled( unsigned long ulLine, const char * const pcFileName )
{
    __asm("    nop\n");
}

