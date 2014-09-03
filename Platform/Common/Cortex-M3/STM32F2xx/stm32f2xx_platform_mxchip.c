/*
 * Copyright 2013, Broadcom Corporation
 * All Rights Reserved.
 *
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Broadcom Corporation;
 * the contents of this file may not be disclosed to third parties, copied
 * or duplicated in any form, in whole or in part, without the prior
 * written permission of Broadcom Corporation.
 */

/** @file
 *
 */
#include "stm32f2xx_platform.h"
#include "stm32f2xx_flash.h"
#include "wwd_constants.h"
#include "wwd_assert.h"
#include "platform.h"
#include "platform_common_config.h"
#include "MICOPlatform.h"
#include "gpio_irq.h"
#include "watchdog.h"
#include "platform_dct.h"
#include <string.h> // For memcmp
#include "Platform/wwd_platform_interface.h"
#include "stm32f2xx_i2c.h"
#include "crt0.h"
#include "platform_sleep.h"
#include "rtc.h"
#include "MICORTOS.h"

#ifdef __GNUC__
#include "../../GCC/stdio_newlib.h"
#endif /* ifdef __GNUC__ */

#ifdef __GNUC__
#define WEAK __attribute__ ((weak))
#elif defined ( __IAR_SYSTEMS_ICC__ )
#define WEAK __weak
#endif /* ifdef __GNUC__ */

#ifndef WICED_DISABLE_BOOTLOADER
#include "bootloader_app.h"
#endif

/******************************************************
 *                      Macros
 ******************************************************/
#ifndef BOOTLOADER_MAGIC_NUMBER
#define BOOTLOADER_MAGIC_NUMBER 0x4d435242
#endif

#ifndef WICED_DISABLE_MCU_POWERSAVE
#define MCU_CLOCKS_NEEDED()       stm32f2xx_clocks_needed()
#define MCU_CLOCKS_NOT_NEEDED()   stm32f2xx_clocks_not_needed()
#define MCU_RTC_INIT()            RTC_Wakeup_init()
#else
#define MCU_CLOCKS_NEEDED()
#define MCU_CLOCKS_NOT_NEEDED()
#ifdef WICED_ENABLE_MCU_RTC
#define MCU_RTC_INIT() platform_rtc_init()
#else /* #ifdef WICED_ENABLE_MCU_RTC */
#define MCU_RTC_INIT()
#endif /* #ifdef WICED_ENABLE_MCU_RTC */
#endif /* ifndef WICED_DISABLE_MCU_POWERSAVE */

#define USE_RTC_BKP 0x00BB32F2// yhb defined, use RTC BKP to initilize system time.
#ifndef BUS_SPI
#define WICED_ENABLE_MCU_RTC // yhb defined, always enable MCU RTC. 
#endif


/******************************************************
 *                    Constants
 ******************************************************/

#define MAX_NUM_SPI_PRESCALERS     (8)
#define SPI_DMA_TIMEOUT_LOOPS      (10000)

#if defined( PLATFORM_STM32_VOLTAGE_1V8_TO_2V1 )
#define ERASE_VOLTAGE_RANGE ( VoltageRange_1 )
#define FLASH_WRITE_FUNC    ( FLASH_ProgramByte )
#define FLASH_WRITE_SIZE    ( 1 )
typedef uint8_t flash_write_t;
#elif defined( PLATFORM_STM32_VOLTAGE_2V1_TO_2V7 )
#define ERASE_VOLTAGE_RANGE ( VoltageRange_2 )
#define FLASH_WRITE_FUNC    ( FLASH_ProgramHalfWord )
#define FLASH_WRITE_SIZE    ( 2 )
typedef uint16_t flash_write_t;
#elif defined( PLATFORM_STM32_VOLTAGE_2V7_TO_3V6 )
#define ERASE_VOLTAGE_RANGE ( VoltageRange_3 )
#define FLASH_WRITE_FUNC    ( FLASH_ProgramWord )
#define FLASH_WRITE_SIZE    ( 4 )
typedef uint32_t flash_write_t;
#elif defined( PLATFORM_STM32_VOLTAGE_2V7_TO_3V6_EXTERNAL_VPP )
#define ERASE_VOLTAGE_RANGE ( VoltageRange_4 )
#define FLASH_WRITE_FUNC    ( FLASH_ProgramDoubleWord )
#define FLASH_WRITE_SIZE    ( 8 )
typedef uint64_t flash_write_t;
#else
/* No Voltage range defined for platform */
/* You need to define one of:
 *   PLATFORM_STM32_VOLTAGE_1V8_TO_2V1
 *   PLATFORM_STM32_VOLTAGE_2V1_TO_2V7
 *   PLATFORM_STM32_VOLTAGE_2V7_TO_3V6
 *   PLATFORM_STM32_VOLTAGE_2V7_TO_3V6_EXTERNAL_VPP
 */
#error Platform Voltage Range not defined
#endif

#define APP_HDR_START_ADDR   ((uint32_t)&app_hdr_start_addr_loc)
#define DCT1_START_ADDR  ((uint32_t)&dct1_start_addr_loc)
#define DCT1_SIZE        ((uint32_t)&dct1_size_loc)
#define DCT2_START_ADDR  ((uint32_t)&dct2_start_addr_loc)
#define DCT2_SIZE        ((uint32_t)&dct2_size_loc)
#define SRAM_START_ADDR  ((uint32_t)&sram_start_addr_loc)
#define SRAM_SIZE        ((uint32_t)&sram_size_loc)

#define PLATFORM_DCT_COPY1_START_SECTOR      ( FLASH_Sector_1  )
#define PLATFORM_DCT_COPY1_START_ADDRESS     ( DCT1_START_ADDR )
#define PLATFORM_DCT_COPY1_END_SECTOR        ( FLASH_Sector_1 )
#define PLATFORM_DCT_COPY1_END_ADDRESS       ( DCT1_START_ADDR + DCT1_SIZE )
#define PLATFORM_DCT_COPY2_START_SECTOR      ( FLASH_Sector_2  )
#define PLATFORM_DCT_COPY2_START_ADDRESS     ( DCT2_START_ADDR )
#define PLATFORM_DCT_COPY2_END_SECTOR        ( FLASH_Sector_2 )
#define PLATFORM_DCT_COPY2_END_ADDRESS       ( DCT1_START_ADDR + DCT1_SIZE )

#define ERASE_DCT_1()              platform_erase_flash(PLATFORM_DCT_COPY1_START_SECTOR, PLATFORM_DCT_COPY1_END_SECTOR)
#define ERASE_DCT_2()              platform_erase_flash(PLATFORM_DCT_COPY2_START_SECTOR, PLATFORM_DCT_COPY2_END_SECTOR)

#ifndef STDIO_BUFFER_SIZE
#define STDIO_BUFFER_SIZE   64
#endif

#define I2C_FLAG_CHECK_TIMEOUT      ( 1000 )
#define I2C_FLAG_CHECK_LONG_TIMEOUT ( 1000 )


#define I2C_MESSAGE_DMA_MASK_POSN 0
#define I2C_MESSAGE_NO_DMA    (0 << I2C_MESSAGE_DMA_MASK_POSN) /* No DMA is set to 0 because DMA should be enabled by */
#define I2C_MESSAGE_USE_DMA   (1 << I2C_MESSAGE_DMA_MASK_POSN) /* default, and turned off as an exception */


#define DMA_FLAG_TC(stream_id) dma_flag_tc(stream_id)
#define DMA_TIMEOUT_LOOPS      (10000000)

#define NUMBER_OF_LSE_TICKS_PER_MILLISECOND(scale_factor) ( 32768 / 1000 / scale_factor )
#define CONVERT_FROM_TICKS_TO_MS(n,s) ( n / NUMBER_OF_LSE_TICKS_PER_MILLISECOND(s) )
#define CK_SPRE_CLOCK_SOURCE_SELECTED 0xFFFF

/******************************************************
 *                   Enumerations
 ******************************************************/

/******************************************************
 *                 Type Definitions
 ******************************************************/

/******************************************************
 *                    Structures
 ******************************************************/

typedef struct
{
    uint16_t factor;
    uint16_t prescaler_value;
} spi_baudrate_division_mapping_t;

/******************************************************
 *               Function Declarations
 ******************************************************/

#ifndef WICED_DISABLE_MCU_POWERSAVE
void stm32f2xx_clocks_needed    ( void );
void stm32f2xx_clocks_not_needed( void );
static void RTC_Wakeup_init( void );
#endif

#if defined(WICED_ENABLE_MCU_RTC) && defined(WICED_DISABLE_MCU_POWERSAVE)
void platform_rtc_init( void );
#endif /* #if defined(WICED_ENABLE_MCU_RTC) && defined(WICED_DISABLE_MCU_POWERSAVE) */
void wake_up_interrupt_notify( void );

/* Interrupt service functions - called from interrupt vector table */
void RTC_WKUP_irq     ( void );

/******************************************************
 *               Variables Definitions
 ******************************************************/

/* These come from the linker script */
extern void* dct1_start_addr_loc;
extern void* dct1_size_loc;
extern void* dct2_start_addr_loc;
extern void* dct2_size_loc;
extern void* app_hdr_start_addr_loc;
extern void* sram_start_addr_loc;
extern void* sram_size_loc;

static const uint32_t spi_transfer_complete_flags[]=
{
    /* for every stream get a transfer complete flag */
    [0] =  DMA_FLAG_TCIF0,
    [1] =  DMA_FLAG_TCIF1,
    [2] =  DMA_FLAG_TCIF2,
    [3] =  DMA_FLAG_TCIF3,
    [4] =  DMA_FLAG_TCIF4,
    [5] =  DMA_FLAG_TCIF5,
    [6] =  DMA_FLAG_TCIF6,
    [7] =  DMA_FLAG_TCIF7,
};

static const spi_baudrate_division_mapping_t spi_baudrate_prescalers[MAX_NUM_SPI_PRESCALERS] =
{
    { 2,   SPI_BaudRatePrescaler_2   },
    { 4,   SPI_BaudRatePrescaler_4   },
    { 8,   SPI_BaudRatePrescaler_8   },
    { 16,  SPI_BaudRatePrescaler_16  },
    { 32,  SPI_BaudRatePrescaler_32  },
    { 64,  SPI_BaudRatePrescaler_64  },
    { 128, SPI_BaudRatePrescaler_128 },
    { 256, SPI_BaudRatePrescaler_256 },
};

static char stm32_platform_inited = 0;

#ifndef MICO_DISABLE_STDIO
static const mico_uart_config_t stdio_uart_config =
{
    .baud_rate    = 115200,
    .data_width   = DATA_WIDTH_8BIT,
    .parity       = NO_PARITY,
    .stop_bits    = STOP_BITS_1,
    .flow_control = FLOW_CONTROL_DISABLED,
};

static volatile ring_buffer_t stdio_rx_buffer;
static volatile uint8_t             stdio_rx_data[STDIO_BUFFER_SIZE];
mico_mutex_t        stdio_rx_mutex;
mico_mutex_t        stdio_tx_mutex;
#endif /* #ifndef MICO_DISABLE_STDIO */

static volatile uint8_t uart_break;

static DMA_InitTypeDef  i2c_dma_init; /* Should investigate why this is global */

//static wiced_spi_device_t* current_spi_device = NULL;

//#ifdef RTC_ENABLED
wiced_rtc_time_t wiced_default_time =
{
     /* set it to 12:20:30 08/04/2013 monday */
     .sec   = 30,
     .min   = 20,
     .hr    = 12,
     .weekday  = 1,
     .date  = 8,
     .month = 4,
     .year  = 13
};
//#endif /* #ifdef RTC_ENABLED */

static const uint16_t adc_sampling_cycle[] =
{
    [ADC_SampleTime_3Cycles  ] = 3,
    [ADC_SampleTime_15Cycles ] = 15,
    [ADC_SampleTime_28Cycles ] = 28,
    [ADC_SampleTime_56Cycles ] = 56,
    [ADC_SampleTime_84Cycles ] = 84,
    [ADC_SampleTime_112Cycles] = 112,
    [ADC_SampleTime_144Cycles] = 144,
    [ADC_SampleTime_480Cycles] = 480,
};

#ifndef WICED_DISABLE_MCU_POWERSAVE
static wiced_bool_t wake_up_interrupt_triggered  = WICED_FALSE;
static unsigned long rtc_timeout_start_time           = 0;
#endif /* #ifndef WICED_DISABLE_MCU_POWERSAVE */

/******************************************************
 *               Function Definitions
 ******************************************************/

/* STM32F2 common clock initialisation function
 * This brings up enough clocks to allow the processor to run quickly while initialising memory.
 * Other platform specific clock init can be done in init_platform() or init_architecture()
 */
WEAK void init_clocks( void )
{
    //RCC_DeInit( ); /* if not commented then the LSE PA8 output will be disabled and never comes up again */

    /* Configure Clocks */

    RCC_HSEConfig( HSE_SOURCE );
    RCC_WaitForHSEStartUp( );

    RCC_HCLKConfig( AHB_CLOCK_DIVIDER );
    RCC_PCLK2Config( APB2_CLOCK_DIVIDER );
    RCC_PCLK1Config( APB1_CLOCK_DIVIDER );

    /* Enable the PLL */
    FLASH_SetLatency( INT_FLASH_WAIT_STATE );
    FLASH_PrefetchBufferCmd( ENABLE );

    /* Use the clock configuration utility from ST to calculate these values
     * http://www.st.com/st-web-ui/static/active/en/st_prod_software_internet/resource/technical/software/utility/stsw-stm32090.zip
     */
    RCC_PLLConfig( PLL_SOURCE, PLL_M_CONSTANT, PLL_N_CONSTANT, PLL_P_CONSTANT, PPL_Q_CONSTANT ); /* NOTE: The CPU Clock Frequency is independently defined in <WICED-SDK>/Wiced/Platform/<platform>/<platform>.mk */
    RCC_PLLCmd( ENABLE );

    while ( RCC_GetFlagStatus( RCC_FLAG_PLLRDY ) == RESET )
    {
    }
    RCC_SYSCLKConfig( SYSTEM_CLOCK_SOURCE );

    while ( RCC_GetSYSCLKSource( ) != 0x08 )
    {
    }

    /* Configure HCLK clock as SysTick clock source. */
    SysTick_CLKSourceConfig( SYSTICK_CLOCK_SOURCE );

}

WEAK void init_memory( void )
{

}

void init_architecture( void )
{
    uint8_t i;

#ifdef INTERRUPT_VECTORS_IN_RAM
    SCB->VTOR = 0x20000000; /* Change the vector table to point to start of SRAM */
#endif /* ifdef INTERRUPT_VECTORS_IN_RAM */

    if ( stm32_platform_inited == 1 )
        return;

    watchdog_init( );

    /* Initialise the interrupt priorities to a priority lower than 0 so that the BASEPRI register can mask them */
    for ( i = 0; i < 81; i++ )
    {
        NVIC ->IP[i] = 0xff;
    }

    NVIC_PriorityGroupConfig( NVIC_PriorityGroup_4 );

#ifndef MICO_DISABLE_STDIO
    mico_rtos_init_mutex( &stdio_tx_mutex );
    mico_rtos_unlock_mutex ( &stdio_tx_mutex );
    mico_rtos_init_mutex( &stdio_rx_mutex );
    mico_rtos_unlock_mutex ( &stdio_rx_mutex );
    ring_buffer_init  ( (ring_buffer_t*)&stdio_rx_buffer, (uint8_t*)stdio_rx_data, STDIO_BUFFER_SIZE );
    MicoStdioUartInitialize( &stdio_uart_config, (ring_buffer_t*)&stdio_rx_buffer );
#endif

    /* Ensure 802.11 device is in reset. */
    host_platform_init( );

    MCU_RTC_INIT();

    /* Disable MCU powersave at start-up. Application must explicitly enable MCU powersave if desired */
    MCU_CLOCKS_NEEDED();

    stm32_platform_inited = 1;
}





wiced_result_t wiced_adc_init( wiced_adc_t adc, uint32_t sample_cycle )
{
    GPIO_InitTypeDef      gpio_init_structure;
    ADC_InitTypeDef       adc_init_structure;
    ADC_CommonInitTypeDef adc_common_init_structure;
    uint8_t a;

    MCU_CLOCKS_NEEDED();

    /* Initialize the associated GPIO */
    gpio_init_structure.GPIO_Pin   = (uint16_t) ( 1 << adc_mapping[adc].pin->number );
    gpio_init_structure.GPIO_Speed = (GPIOSpeed_TypeDef) 0;
    gpio_init_structure.GPIO_Mode  = GPIO_Mode_AN;
    gpio_init_structure.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    gpio_init_structure.GPIO_OType = GPIO_OType_OD;
    GPIO_Init( adc_mapping[adc].pin->bank, &gpio_init_structure );

    /* Ensure the ADC and GPIOA are enabled */
    RCC_AHB1PeriphClockCmd( adc_mapping[adc].pin->peripheral_clock, ENABLE);
    RCC_APB2PeriphClockCmd( adc_mapping[adc].adc_peripheral_clock, ENABLE );

    /* Initialize the ADC */
    ADC_StructInit( &adc_init_structure );
    adc_init_structure.ADC_Resolution         = ADC_Resolution_12b;
    adc_init_structure.ADC_ScanConvMode       = DISABLE;
    adc_init_structure.ADC_ContinuousConvMode = DISABLE;
    adc_init_structure.ADC_ExternalTrigConv   = ADC_ExternalTrigConvEdge_None;
    adc_init_structure.ADC_DataAlign          = ADC_DataAlign_Right;
    adc_init_structure.ADC_NbrOfConversion    = 1;
    ADC_Init( adc_mapping[adc].adc, &adc_init_structure );

    ADC_CommonStructInit(&adc_common_init_structure);
    adc_common_init_structure.ADC_Mode             = ADC_Mode_Independent;
    adc_common_init_structure.ADC_DMAAccessMode    = ADC_DMAAccessMode_Disabled;
    adc_common_init_structure.ADC_Prescaler        = ADC_Prescaler_Div2;
    adc_common_init_structure.ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles;
    ADC_CommonInit(&adc_common_init_structure);

    ADC_Cmd( adc_mapping[adc].adc, ENABLE );

    /* Find the closest supported sampling time by the MCU */
    for ( a = 0; ( a < sizeof( adc_sampling_cycle ) / sizeof(uint16_t) ) && adc_sampling_cycle[a] < sample_cycle; a++ )
    {
    }

    /* Initialize the ADC channel */
    ADC_RegularChannelConfig( adc_mapping[adc].adc, adc_mapping[adc].channel, adc_mapping[adc].rank, a );

    MCU_CLOCKS_NOT_NEEDED();

    return WICED_SUCCESS;
}

wiced_result_t wiced_adc_take_sample( wiced_adc_t adc, uint16_t* output )
{
    MCU_CLOCKS_NEEDED();

    /* Start conversion */
    ADC_SoftwareStartConv( adc_mapping[adc].adc );

    /* Wait until end of conversion */
    while ( ADC_GetFlagStatus( adc_mapping[adc].adc, ADC_FLAG_EOC ) == RESET )
    {
    }

    /* Read ADC conversion result */
    *output = ADC_GetConversionValue( adc_mapping[adc].adc );

    MCU_CLOCKS_NOT_NEEDED();

    return WICED_SUCCESS;
}

wiced_result_t wiced_adc_take_sample_stream( wiced_adc_t adc, void* buffer, uint16_t buffer_length )
{
    UNUSED_PARAMETER(adc);
    UNUSED_PARAMETER(buffer);
    UNUSED_PARAMETER(buffer_length);
    wiced_assert( "unimplemented", 0!=0 );
    return WICED_SUCCESS;
}

wiced_result_t wiced_adc_deinit( wiced_adc_t adc )
{
    UNUSED_PARAMETER(adc);
    wiced_assert( "unimplemented", 0!=0 );
    return WICED_SUCCESS;
}


wiced_result_t wiced_watchdog_kick( void )
{
    return watchdog_kick();
}

/******************************************************
 *                 DCT Functions
 ******************************************************/

#ifndef WICED_DISABLE_BOOTLOADER
platform_dct_data_t* platform_get_dct( void )
{
    platform_dct_header_t hdr =
    {
        .write_incomplete    = 0,
        .is_current_dct      = 1,
        .app_valid           = 1,
        .mfg_info_programmed = 0,
        .magic_number        = BOOTLOADER_MAGIC_NUMBER,
        .load_app_func       = NULL
    };

    platform_dct_header_t* dct1 = ((platform_dct_header_t*) PLATFORM_DCT_COPY1_START_ADDRESS);
    platform_dct_header_t* dct2 = ((platform_dct_header_t*) PLATFORM_DCT_COPY2_START_ADDRESS);

    if ( ( dct1->is_current_dct == 1 ) &&
         ( dct1->write_incomplete == 0 ) &&
         ( dct1->magic_number == BOOTLOADER_MAGIC_NUMBER ) )
    {
        return (platform_dct_data_t*)dct1;
    }

    if ( ( dct2->is_current_dct == 1 ) &&
         ( dct2->write_incomplete == 0 ) &&
         ( dct2->magic_number == BOOTLOADER_MAGIC_NUMBER ) )
    {
        return (platform_dct_data_t*)dct2;
    }

    /* No valid DCT! */
    /* Erase the first DCT and init it. */
    ERASE_DCT_1();
//    platform_bootloader_erase_dct( 1 );

    platform_write_flash_chunk( PLATFORM_DCT_COPY1_START_ADDRESS, (uint8_t*) &hdr, sizeof(hdr) );

    return (platform_dct_data_t*)dct1;
}

void platform_erase_dct( void )
{
    ERASE_DCT_1();
    ERASE_DCT_2();
}

/* TODO: Disable interrupts during function */
/* Note Function allocates a chunk of memory for the bootloader data on the stack - ensure the stack is big enough */
int platform_write_dct( uint16_t data_start_offset, const void* data, uint16_t data_length, int8_t app_valid, void (*func)(void) )
{
    platform_dct_header_t* new_dct;
    uint32_t               bytes_after_data;
    uint8_t*               new_app_data_addr;
    uint8_t*               curr_app_data_addr;
    platform_dct_header_t* curr_dct  = &platform_get_dct( )->dct_header;
    char                   zero_byte = 0;
    platform_dct_header_t  hdr =
    {
        .write_incomplete = 1,
        .is_current_dct   = 1,
        .magic_number     = BOOTLOADER_MAGIC_NUMBER
    };

    /* Check if the data is too big to write */
    if ( data_length + data_start_offset > ( PLATFORM_DCT_COPY1_END_ADDRESS - PLATFORM_DCT_COPY1_START_ADDRESS ) )
    {
        return -1;
    }

    /* Erase the non-current DCT */
    if ( curr_dct == ((platform_dct_header_t*)PLATFORM_DCT_COPY1_START_ADDRESS) )
    {
        new_dct = (platform_dct_header_t*)PLATFORM_DCT_COPY2_START_ADDRESS;
        ERASE_DCT_2();
    }
    else
    {
        new_dct = (platform_dct_header_t*)PLATFORM_DCT_COPY1_START_ADDRESS;
        ERASE_DCT_1();
    }

    /* Write the mfg data and initial part of app data before provided data */
    platform_write_flash_chunk( ((uint32_t) &new_dct[1]), (uint8_t*) &curr_dct[1], data_start_offset );

    /* Verify the mfg data */
    if ( memcmp( &new_dct[1], &curr_dct[1], data_start_offset ) != 0 )
    {
        return -2;
    }

    /* Write the app data */
    new_app_data_addr  =  (uint8_t*) &new_dct[1]  + data_start_offset;
    curr_app_data_addr =  (uint8_t*) &curr_dct[1] + data_start_offset;

    platform_write_flash_chunk( (uint32_t)new_app_data_addr, data, data_length );

    /* Verify the app data */
    if ( memcmp( new_app_data_addr, data, data_length ) != 0 )
    {
        /* Error writing app data */
        return -3;
    }

    bytes_after_data = ( PLATFORM_DCT_COPY1_END_ADDRESS - PLATFORM_DCT_COPY1_START_ADDRESS ) - (sizeof(platform_dct_header_t) + data_start_offset + data_length );

    if ( bytes_after_data != 0 )
    {
        new_app_data_addr += data_length;
        curr_app_data_addr += data_length;

        platform_write_flash_chunk( (uint32_t)new_app_data_addr, curr_app_data_addr, bytes_after_data );
        /* Verify the app data */
        if ( 0 != memcmp( new_app_data_addr, curr_app_data_addr, bytes_after_data ) )
        {
            /* Error writing app data */
            return -4;
        }
    }

    hdr.app_valid           = (char) (( app_valid == -1 )? curr_dct->app_valid : app_valid);
    hdr.load_app_func       = func;
    hdr.mfg_info_programmed = curr_dct->mfg_info_programmed;

    /* Write the header data */
    platform_write_flash_chunk( (uint32_t)new_dct, (uint8_t*) &hdr, sizeof(hdr) );

    /* Verify the header data */
    if ( memcmp( new_dct, &hdr, sizeof(hdr) ) != 0 )
    {
        /* Error writing header data */
        return -5;
    }

    /* Mark new DCT as complete and current */
    platform_write_flash_chunk( (uint32_t)&new_dct->write_incomplete, (uint8_t*) &zero_byte, sizeof(zero_byte) );
    platform_write_flash_chunk( (uint32_t)&curr_dct->is_current_dct, (uint8_t*) &zero_byte, sizeof(zero_byte) );

    return 0;
}

int platform_erase_flash( uint16_t start_sector, uint16_t end_sector )
{
    uint32_t i;

    /* Unlock the STM32 Flash */
    FLASH_Unlock( );

    /* Clear any error flags */
    FLASH_ClearFlag( FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR );

    watchdog_kick( );

    for ( i = start_sector; i <= end_sector; i += 8 )
    {
        if ( FLASH_EraseSector( i, ERASE_VOLTAGE_RANGE ) != FLASH_COMPLETE )
        {
            /* Error occurred during erase. */
            /* TODO: Handle error */
            while ( 1 )
            {
            }
        }
        watchdog_kick( );
    }

    FLASH_Lock( );

    return 0;
}

int platform_write_app_chunk( uint32_t offset, const uint8_t* data, uint32_t size )
{
    return platform_write_flash_chunk( APP_HDR_START_ADDR + offset, data, size );
}

int platform_write_flash_chunk( uint32_t address, const uint8_t* data, uint32_t size )
{
    uint32_t write_address  = address;
    const uint8_t* end_of_data = data + size;
    const uint8_t* data_iter   = data;
//    flash_write_t* data_ptr = (flash_write_t*) data;
//    flash_write_t* end_ptr  = (flash_write_t*) &data[size];

    FLASH_Unlock( );

    /* Clear any error flags */
    FLASH_ClearFlag( FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR );

    /* Write data to STM32 flash memory */
    while ( data_iter <  end_of_data )
    {
        FLASH_Status status;

        if ( ( ( ((uint32_t)write_address) & 0x03 ) == 0 ) && ( end_of_data - data_iter >= FLASH_WRITE_SIZE ) )
        {
            int tries = 0;
            /* enough data available to write as the largest size allowed by supply voltage */
            while ( ( FLASH_COMPLETE != ( status = FLASH_WRITE_FUNC( write_address, *((flash_write_t*)data_iter) ) ) ) && ( tries < 10 ) )
            {
                tries++;
            }
            if ( FLASH_COMPLETE != status )
            {
                /* TODO: Handle error properly */
                wiced_assert("Error during write", 0 != 0 );
                return 1;
            }
            write_address += FLASH_WRITE_SIZE;
            data_iter     += FLASH_WRITE_SIZE;
        }
        else
        {
            int tries = 0;
            /* Limited data available - write in bytes */
            while ( ( FLASH_COMPLETE != ( status = FLASH_ProgramByte( write_address, *data_iter ) ) ) && ( tries < 10 ) )
            {
                tries++;
            }
            if ( FLASH_COMPLETE != status )
            {
                /* TODO: Handle error properly */
                wiced_assert("Error during write", 0 != 0 );
                return 1;
            }
            ++write_address;
            ++data_iter;
        }

    }

    FLASH_Lock();

    return 0;
}
#endif

/******************************************************
 *            Interrupt Service Routines
 ******************************************************/

#ifdef BUS_SPI
WEAK void sdio_irq(void)
{
}
#endif


#define RTC_INTERRUPT_EXTI_LINE EXTI_Line22
#define WUT_COUNTER_MAX  0xffff

#define ENABLE_INTERRUPTS   __asm("CPSIE i")
#define DISABLE_INTERRUPTS  __asm("CPSID i")

#if defined(WICED_DISABLE_MCU_POWERSAVE) && defined(WICED_ENABLE_MCU_RTC)
/*  */
void platform_rtc_init(void)
{
    RTC_InitTypeDef RTC_InitStruct;

    RTC_DeInit( );

    RTC_InitStruct.RTC_HourFormat = RTC_HourFormat_24;

    /* RTC ticks every second */
    RTC_InitStruct.RTC_AsynchPrediv = 0x7F;
    RTC_InitStruct.RTC_SynchPrediv = 0xFF;

    RTC_Init( &RTC_InitStruct );
    /* Enable the LSE OSC */
    RCC_LSEConfig(RCC_LSE_ON);
    /* Wait till LSE is ready */
    while(RCC_GetFlagStatus(RCC_FLAG_LSERDY) == RESET)
    {
    }
    /* Select the RTC Clock Source */
    RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);

    /* Enable the RTC Clock */
    RCC_RTCCLKCmd(ENABLE);

    /* RTC configuration -------------------------------------------------------*/
    /* Wait for RTC APB registers synchronisation */
    RTC_WaitForSynchro();

#ifdef USE_RTC_BKP
    if (RTC_ReadBackupRegister(RTC_BKP_DR0) != USE_RTC_BKP) {
        /* set it to 12:20:30 08/04/2013 monday */
        wiced_default_time.sec   = 0,
        wiced_default_time.min   = 0,
        wiced_default_time.hr    = 0,
        wiced_default_time.weekday  = 1,
        wiced_default_time.date  = 8,
        wiced_default_time.month = 4,
        wiced_default_time.year  = 13
        platform_set_rtc_time(&wiced_default_time);
        RTC_WriteBackupRegister(RTC_BKP_DR0, USE_RTC_BKP);
    }
#else
    /* write default application time inside rtc */
    platform_set_rtc_time(&wiced_default_time);
#endif

}
#endif

/**
 * This function will return the value of time read from the on board CPU real time clock. Time value must be given in the format of
 * the structure wiced_rtc_time_t
 *
 * @return    WICED_SUCCESS : on success.
 * @return    WICED_ERROR   : if an error occurred with any step
 */
wiced_result_t wiced_platform_get_rtc_time(wiced_rtc_time_t* time)
{
#ifdef WICED_ENABLE_MCU_RTC
    RTC_TimeTypeDef rtc_read_time;
    RTC_DateTypeDef rtc_read_date;

    if( time == 0 )
    {
        return WICED_BADARG;
    }

    /* save current rtc time */
    RTC_GetTime( RTC_Format_BIN, &rtc_read_time );
    RTC_GetDate( RTC_Format_BIN, &rtc_read_date );

    /* fill structure */
    time->sec = rtc_read_time.RTC_Seconds;
    time->min = rtc_read_time.RTC_Minutes;
    time->hr = rtc_read_time.RTC_Hours;
    time->weekday = rtc_read_date.RTC_WeekDay;
    time->date = rtc_read_date.RTC_Date;
    time->month= rtc_read_date.RTC_Month;
    time->year = rtc_read_date.RTC_Year;

    return WICED_SUCCESS;
#else /* #ifdef WICED_ENABLE_MCU_RTC */
    UNUSED_PARAMETER(time);
    return WICED_UNSUPPORTED;
#endif /* #ifdef WICED_ENABLE_MCU_RTC */
}

/**
 * This function will set MCU RTC time to a new value. Time value must be given in the format of
 * the structure wiced_rtc_time_t
 *
 * @return    WICED_SUCCESS : on success.
 * @return    WICED_ERROR   : if an error occurred with any step
 */
wiced_result_t wiced_platform_set_rtc_time(wiced_rtc_time_t* time)
{
#ifdef WICED_ENABLE_MCU_RTC
    RTC_TimeTypeDef rtc_write_time;
    RTC_DateTypeDef rtc_write_date;
    wiced_bool_t    valid = WICED_FALSE;

    WICED_VERIFY_TIME(time, valid);
    if( valid == WICED_FALSE )
    {
        return WICED_BADARG;
    }
    rtc_write_time.RTC_Seconds = time->sec;
    rtc_write_time.RTC_Minutes = time->min;
    rtc_write_time.RTC_Hours   = time->hr;
    rtc_write_date.RTC_WeekDay = time->weekday;
    rtc_write_date.RTC_Date    = time->date;
    rtc_write_date.RTC_Month   = time->month;
    rtc_write_date.RTC_Year    = time->year;


    RTC_SetTime( RTC_Format_BIN, &rtc_write_time );
    RTC_SetDate( RTC_Format_BIN, &rtc_write_date );

    return WICED_SUCCESS;
#else /* #ifdef WICED_ENABLE_MCU_RTC */
    UNUSED_PARAMETER(time);
    return WICED_UNSUPPORTED;
#endif /* #ifdef WICED_ENABLE_MCU_RTC */
}

#ifndef WICED_DISABLE_MCU_POWERSAVE

static int stm32f2_clock_needed_counter = 0;

void stm32f2xx_clocks_needed( void )
{
    DISABLE_INTERRUPTS;
    if ( stm32f2_clock_needed_counter <= 0 )
    {
        SCB->SCR &= (~((unsigned long)SCB_SCR_SLEEPDEEP_Msk));
        stm32f2_clock_needed_counter = 0;
    }
    stm32f2_clock_needed_counter++;
    ENABLE_INTERRUPTS;
}

void stm32f2xx_clocks_not_needed( void )
{
    DISABLE_INTERRUPTS;
    stm32f2_clock_needed_counter--;
    if ( stm32f2_clock_needed_counter <= 0 )
    {
        SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
        stm32f2_clock_needed_counter = 0;
    }
    ENABLE_INTERRUPTS;
}


#ifndef WICED_DISABLE_MCU_POWERSAVE
void RTC_Wakeup_init(void)
{
    NVIC_InitTypeDef NVIC_InitStructure;
    EXTI_InitTypeDef EXTI_InitStructure;
    RTC_InitTypeDef RTC_InitStruct;

    RTC_DeInit( );

    RTC_InitStruct.RTC_HourFormat = RTC_HourFormat_24;

    /* RTC ticks every second */
    RTC_InitStruct.RTC_AsynchPrediv = 0x7F;
    RTC_InitStruct.RTC_SynchPrediv = 0xFF;

    RTC_Init( &RTC_InitStruct );

    /* Enable the PWR clock */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);

    /* RTC clock source configuration ------------------------------------------*/
    /* Allow access to BKP Domain */
    PWR_BackupAccessCmd(ENABLE);
#ifdef USE_RTC_BKP
    PWR_BackupRegulatorCmd(ENABLE);
#endif

    /* Enable the LSE OSC */
    RCC_LSEConfig(RCC_LSE_ON);
    /* Wait till LSE is ready */
    while(RCC_GetFlagStatus(RCC_FLAG_LSERDY) == RESET)
    {
    }

    /* Select the RTC Clock Source */
    RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);

    /* Enable the RTC Clock */
    RCC_RTCCLKCmd(ENABLE);

    /* RTC configuration -------------------------------------------------------*/
    /* Wait for RTC APB registers synchronisation */
    RTC_WaitForSynchro();

    RTC_WakeUpCmd( DISABLE );
    EXTI_ClearITPendingBit( RTC_INTERRUPT_EXTI_LINE );
    PWR_ClearFlag(PWR_FLAG_WU);
    RTC_ClearFlag(RTC_FLAG_WUTF);

    RTC_WakeUpClockConfig(RTC_WakeUpClock_RTCCLK_Div2);

    EXTI_ClearITPendingBit( RTC_INTERRUPT_EXTI_LINE );
    EXTI_InitStructure.EXTI_Line = RTC_INTERRUPT_EXTI_LINE;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);

    NVIC_InitStructure.NVIC_IRQChannel = RTC_WKUP_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
    RTC_ITConfig(RTC_IT_WUT, DISABLE);

    /* Prepare Stop-Mode but leave disabled */
//    PWR_ClearFlag(PWR_FLAG_WU);
    PWR->CR |= PWR_CR_LPDS;
    PWR->CR &= (unsigned long)(~(PWR_CR_PDDS));
    SCB->SCR |= ((unsigned long)SCB_SCR_SLEEPDEEP_Msk);

#ifdef USE_RTC_BKP
    if (RTC_ReadBackupRegister(RTC_BKP_DR0) != USE_RTC_BKP) {
        /* set it to 12:20:30 08/04/2013 monday */
        wiced_default_time.sec   = 0,
        wiced_default_time.min   = 0,
        wiced_default_time.hr    = 0,
        wiced_default_time.weekday  = 1,
        wiced_default_time.date  = 8,
        wiced_default_time.month = 4,
        wiced_default_time.year  = 13;
        platform_set_rtc_time(&wiced_default_time);
        RTC_WriteBackupRegister(RTC_BKP_DR0, USE_RTC_BKP);
    }
#else
//#ifdef RTC_ENABLED
    /* application must have wiced_application_default_time structure declared somewhere, otherwise it wont compile */
    /* write default application time inside rtc */
    platform_set_rtc_time(&wiced_default_time);
//#endif /* RTC_ENABLED */
#endif

}
#endif /* #ifndef WICED_DISABLE_MCU_POWERSAVE */


static wiced_result_t select_wut_prescaler_calculate_wakeup_time( unsigned long* wakeup_time, unsigned long delay_ms, unsigned long* scale_factor )
{
    unsigned long temp;
    wiced_bool_t scale_factor_is_found = WICED_FALSE;
    int i                              = 0;

    static unsigned long int available_wut_prescalers[] =
    {
        RTC_WakeUpClock_RTCCLK_Div2,
        RTC_WakeUpClock_RTCCLK_Div4,
        RTC_WakeUpClock_RTCCLK_Div8,
        RTC_WakeUpClock_RTCCLK_Div16
    };
    static unsigned long scale_factor_values[] = { 2, 4, 8, 16 };

    if ( delay_ms == 0xFFFFFFFF )
    {
        /* wake up in a 100ms, since currently there may be no tasks to run, but after a few milliseconds */
        /* some of them can get unblocked( for example a task is blocked on mutex with unspecified ) */
        *scale_factor = 2;
        RTC_WakeUpClockConfig( RTC_WakeUpClock_RTCCLK_Div2 );
        *wakeup_time = NUMBER_OF_LSE_TICKS_PER_MILLISECOND( scale_factor_values[0] ) * 100;
    }
    else
    {
        for ( i = 0; i < 4; i++ )
        {
            temp = NUMBER_OF_LSE_TICKS_PER_MILLISECOND( scale_factor_values[i] ) * delay_ms;
            if ( temp < WUT_COUNTER_MAX )
            {
                scale_factor_is_found = WICED_TRUE;
                *wakeup_time = temp;
                *scale_factor = scale_factor_values[i];
                break;
            }
        }
        if ( scale_factor_is_found )
        {
            /* set new prescaler for wakeup timer */
            RTC_WakeUpClockConfig( available_wut_prescalers[i] );
        }
        else
        {
            /* scale factor can not be picked up for delays more that 32 seconds when RTCLK is selected as a clock source for the wakeup timer
             * for delays more than 32 seconds change from RTCCLK to 1Hz ck_spre clock source( used to update calendar registers ) */
            RTC_WakeUpClockConfig( RTC_WakeUpClock_CK_SPRE_16bits );

            /* with 1Hz ck_spre clock source the resolution changes to seconds  */
            *wakeup_time = ( delay_ms / 1000 ) + 1;
            *scale_factor = CK_SPRE_CLOCK_SOURCE_SELECTED;

            return WICED_ERROR;
        }
    }

    return WICED_SUCCESS;
}

void wake_up_interrupt_notify( void )
{
    wake_up_interrupt_triggered = WICED_TRUE;
}

static unsigned long stop_mode_power_down_hook( unsigned long delay_ms )
{
    unsigned long retval;
    unsigned long wut_ticks_passed;
    unsigned long scale_factor = 0;
    UNUSED_PARAMETER(delay_ms);
    UNUSED_PARAMETER(rtc_timeout_start_time);
    UNUSED_PARAMETER(scale_factor);

   /* pick up the appropriate prescaler for a requested delay */
    select_wut_prescaler_calculate_wakeup_time(&rtc_timeout_start_time, delay_ms, &scale_factor );

    if ( ( ( SCB->SCR & (unsigned long)SCB_SCR_SLEEPDEEP_Msk) ) != 0 )
    {
        DISABLE_INTERRUPTS;

        SysTick->CTRL &= (~(SysTick_CTRL_TICKINT_Msk|SysTick_CTRL_ENABLE_Msk)); /* systick IRQ off */
        RTC_ITConfig(RTC_IT_WUT, ENABLE);

        EXTI_ClearITPendingBit( RTC_INTERRUPT_EXTI_LINE );
        PWR_ClearFlag(PWR_FLAG_WU);
        RTC_ClearFlag(RTC_FLAG_WUTF);

        RTC_SetWakeUpCounter( rtc_timeout_start_time );
        RTC_WakeUpCmd( ENABLE );
        rtc_sleep_entry();

        DBGMCU->CR |= 0x03; /* Enable debug in stop mode */

        /* This code will be running with BASEPRI register value set to 0, the main intention behind that is that */
        /* all interrupts must be allowed to wake the CPU from the power-down mode */
        /* the PRIMASK is set to 1( see DISABLE_INTERRUPTS), thus we disable all interrupts before entering the power-down mode */
        /* This may sound contradictory, however according to the ARM CM3 documentation power-management unit */
        /* takes into account only the contents of the BASEPRI register and it is an external from the CPU core unit */
        /* PRIMASK register value doesn't affect its operation. */
        /* So, if the interrupt has been triggered just before the wfi instruction */
        /* it remains pending and wfi instruction will be treated as a nop  */
        __asm("wfi");

        /* After CPU exits powerdown mode, the processer will not execute the interrupt handler(PRIMASK is set to 1) */
        /* Disable rtc for now */
        RTC_WakeUpCmd( DISABLE );
        RTC_ITConfig(RTC_IT_WUT, DISABLE);

        /* Initialise the clocks again */
        init_clocks( );

        /* Enable CPU ticks */
        SysTick->CTRL |= (SysTick_CTRL_TICKINT_Msk|SysTick_CTRL_ENABLE_Msk);

        /* Get the time of how long the sleep lasted */
        wut_ticks_passed = rtc_timeout_start_time - RTC_GetWakeUpCounter();
        UNUSED_VARIABLE(wut_ticks_passed);
        rtc_sleep_exit( delay_ms, &retval );
        /* as soon as interrupts are enabled, we will go and execute the interrupt handler */
        /* which triggered a wake up event */
        ENABLE_INTERRUPTS;
        wake_up_interrupt_triggered = WICED_FALSE;
        return retval;
    }
    else
    {
        UNUSED_PARAMETER(wut_ticks_passed);
        ENABLE_INTERRUPTS;
        __asm("wfi");

        /* Note: We return 0 ticks passed because system tick is still going when wfi instruction gets executed */
        return 0;
    }
}

#else /* WICED_DISABLE_MCU_POWERSAVE */

static unsigned long idle_power_down_hook( unsigned long delay_ms  )
{
    UNUSED_PARAMETER( delay_ms );
    ENABLE_INTERRUPTS;
    __asm("wfi");
    return 0;
}

#endif /* WICED_DISABLE_MCU_POWERSAVE */


unsigned long platform_power_down_hook( unsigned long delay_ms )
{
#ifndef WICED_DISABLE_MCU_POWERSAVE
    return stop_mode_power_down_hook( delay_ms );
#else
    return idle_power_down_hook( delay_ms );
#endif
}

void RTC_WKUP_irq( void )
{
    EXTI_ClearITPendingBit( RTC_INTERRUPT_EXTI_LINE );
}

void platform_idle_hook( void )
{
    __asm("wfi");
}

void host_platform_get_mac_address( wiced_mac_t* mac )
{
#ifndef WICED_DISABLE_BOOTLOADER
    memcpy(mac->octet, bootloader_api->get_wifi_config_dct()->mac_address.octet, sizeof(wiced_mac_t));
#else
    UNUSED_PARAMETER( mac );
#endif
}

int host_platform_rand( void *inBuffer, int inByteCount )
{
    // PLATFORM_TO_DO
    int idx;
    uint32_t *pWord = inBuffer;
    uint32_t tempRDM;
    uint8_t *pByte = NULL;
    int inWordCount;
    int remainByteCount;

    inWordCount = inByteCount/4;
    remainByteCount = inByteCount%4;
    pByte = (uint8_t *)pWord+inWordCount*4;

    RNG_DeInit();
    RCC_AHB2PeriphClockCmd(RCC_AHB2Periph_RNG, ENABLE);
    RNG_Cmd(ENABLE);

    for(idx = 0; idx<inWordCount; idx++, pWord++){
        while(RNG_GetFlagStatus(RNG_FLAG_DRDY)!=SET);
        *pWord = RNG_GetRandomNumber();
    }

    if(remainByteCount){
        while(RNG_GetFlagStatus(RNG_FLAG_DRDY)!=SET);
        tempRDM = RNG_GetRandomNumber();
        memcpy(pByte, &tempRDM, (size_t)remainByteCount);
    }
    
    RNG_DeInit();
    return 0;
}

void MicoSystemReboot(void)
{ 
  NVIC_SystemReset();
}

void MicoSystemStandBy(void)
{ 
  PWR_WakeUpPinCmd(ENABLE);
  PWR_EnterSTANDBYMode();
}


//These functions need to be deprecated

void wiced_platform_mcu_enable_powersave( void )
{
    MCU_CLOCKS_NOT_NEEDED();
}

void wiced_platform_mcu_disable_powersave( void )
{
    MCU_CLOCKS_NEEDED();
}