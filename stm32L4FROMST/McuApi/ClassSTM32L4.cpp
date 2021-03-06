/*
 __  __ _       _                                 
 |  \/  (_)     (_)                                
 | \  / |_ _ __  _ _ __ ___   ___  _   _ ___  ___  
 | |\/| | | '_ \| | '_ ` _ \ / _ \| | | / __|/ _ \
 | |  | | | | | | | | | | | | (_) | |_| \__ \  __/ 
 |_|  |_|_|_| |_|_|_| |_| |_|\___/ \__,_|___/\___| 
                                                   
                                                   
Description       : Flash Api.  


License           : Revised BSD License, see LICENSE.TXT file include in the project

Maintainer        : Fabien Holin (SEMTECH)
*/
#include "ClassSTM32L4.h"
#include "stdint.h"
#include "ApiMcu.h"
#include "stm32l4xx_hal.h"
#include "time.h"
#include "spi.h"
#include "rtc.h"
#include "gpio.h"
#include "usart.h"
#include "stm32l4xx_it.h"
#include "lptim.h"
#include "main.h"
#include "wwdg.h"
#include "iwdg.h"
#include "UserDefine.h"
#if DEBUG_TRACE == 1
    #include <stdarg.h>
    #include <string.h>
#endif
#define WATCH_DOG_PERIOD_RELEASE 30 // this period have to be lower than the Watch Dog period of 32 seconds




__STATIC_INLINE uint32_t LL_SPI_IsActiveFlag_TXE(SPI_TypeDef *SPIx)
{
  return (READ_BIT(SPIx->SR, SPI_SR_TXE) == (SPI_SR_TXE));
}

__STATIC_INLINE void LL_SPI_TransmitData8(SPI_TypeDef *SPIx, uint8_t TxData)
{
  *((__IO uint8_t *)&SPIx->DR) = TxData;
}

__STATIC_INLINE uint32_t LL_SPI_IsActiveFlag_RXNE(SPI_TypeDef *SPIx)
{
  return (READ_BIT(SPIx->SR, SPI_SR_RXNE) == (SPI_SR_RXNE));
}

__STATIC_INLINE uint8_t LL_SPI_ReceiveData8(SPI_TypeDef *SPIx)
{
  return (uint8_t)(READ_REG(SPIx->DR));
}

/********************************************************************/
/*                           Flash local functions                  */
/********************************************************************/
#define DATA_EEPROM_BASE       ( ( uint32_t )0x8000000 )              /*!< DATA_EEPROM base address in the alias region */
#define DATA_EEPROM_END        ( ( uint32_t )DATA_EEPROM_BASE + 4096 ) /*!< DATA EEPROM end address in the alias region */

/**
  * @brief  This function does an erase of all user flash area
  * @param  bank_active: start of user flash area
  * @retval FLASHIF_OK : user flash area successfully erased
  *         FLASHIF_ERASEKO : error occurred
  */
enum
{
    FLASHIF_OK = 0,
    FLASHIF_ERASEKO,
    FLASHIF_WRITINGCTRL_ERROR,
    FLASHIF_WRITING_ERROR,
    FLASHIF_CRCKO,
    FLASHIF_RECORD_ERROR,
    FLASHIF_EMPTY,
    FLASHIF_PROTECTION_ERRROR
};

enum{
    FLASHIF_PROTECTION_NONE         = 0,
    FLASHIF_PROTECTION_PCROPENABLED = 0x1,
    FLASHIF_PROTECTION_WRPENABLED   = 0x2,
    FLASHIF_PROTECTION_RDPENABLED   = 0x4,
};

/* protection update */
enum {
    FLASHIF_WRP_ENABLE,
    FLASHIF_WRP_DISABLE
};
uint32_t BankActive = 0, BFSysMem = 0;
FLASH_OBProgramInitTypeDef OBConfig;
/* Notable Flash addresses */
#define FLASH_START_BANK1             ((uint32_t)0x08000000)
#define FLASH_START_BANK2             ((uint32_t)0x08080000)
#define USER_FLASH_END_ADDRESS        ((uint32_t)0x08100000)

#define NVIC_VT_FLASH_B2           FLASH_START_BANK1
#define NVIC_VT_FLASH_B1           FLASH_START_BANK2

uint32_t FLASH_If_Erase(uint32_t bank_active)
{
    uint32_t bank_to_erase, error = 0;
    FLASH_EraseInitTypeDef pEraseInit;
    HAL_StatusTypeDef status = HAL_OK;
    if (bank_active == 0) {
        bank_to_erase = FLASH_BANK_2;
    } else {
        bank_to_erase = FLASH_BANK_1;
    }
    /* Unlock the Flash to enable the flash control register access *************/
    HAL_FLASH_Unlock();
    pEraseInit.Banks = bank_to_erase;
    pEraseInit.NbPages = 255;
    pEraseInit.Page = 0;
    pEraseInit.TypeErase = FLASH_TYPEERASE_MASSERASE;
    status = HAL_FLASHEx_Erase(&pEraseInit, &error);

    /* Lock the Flash to disable the flash control register access (recommended
     to protect the FLASH memory against possible unwanted operation) *********/
    HAL_FLASH_Lock();
    if (status != HAL_OK) {
    /* Error occurred while page erase */
        return FLASHIF_ERASEKO;
    }
    return FLASHIF_OK;
}

/**
  * @brief  This function does an CRC check of an application loaded in a memory bank.
  * @param  start: start of user flash area
  * @retval FLASHIF_OK: user flash area successfully erased
  *         other: error occurred
  */
uint32_t FLASH_If_Check(uint32_t start)
{
    /* checking if the data could be code (first word is stack location) */
    if ((*(uint32_t*)start >> 24) != 0x20 ) return FLASHIF_EMPTY;
    return FLASHIF_OK;
}

/**
  * @brief  This function writes a data buffer in flash (data are 32-bit aligned).
  * @note   After writing data buffer, the flash content is checked.
  * @param  destination: start address for target location
  * @param  p_source: pointer on buffer with data to write
  * @param  length: length of data buffer (unit is 32-bit word)
  * @retval uint32_t 0: Data successfully written to Flash memory
  *         1: Error occurred while writing data in Flash memory
  *         2: Written Data in flash memory is different from expected one
  */
uint32_t FLASH_If_Write(uint32_t destination, uint32_t *p_source, uint32_t length)
{
    uint32_t status = FLASHIF_OK;
    uint32_t i = 0;

    /* Unlock the Flash to enable the flash control register access *************/
    HAL_FLASH_Unlock();

    /* DataLength must be a multiple of 64 bit */
    for (i = 0; (i < length / 2) && (destination <= (USER_FLASH_END_ADDRESS - 8)); i++) {
        /* Device voltage range supposed to be [2.7V to 3.6V], the operation will
           be done by word */
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, destination, *((uint64_t *)(p_source + 2*i))) == HAL_OK) {
          /* Check the written value */
            if (*(uint64_t*)destination != *(uint64_t *)(p_source + 2*i)) {
            /* Flash content doesn't match SRAM content */
                status = FLASHIF_WRITINGCTRL_ERROR;
                break;
            }
          /* Increment FLASH destination address */
            destination += 8;
        } else {
          /* Error occurred while writing data in Flash memory */
            status = FLASHIF_WRITING_ERROR;
            break;
        }
    }

    /* Lock the Flash to disable the flash control register access (recommended
     to protect the FLASH memory against possible unwanted operation) *********/
    HAL_FLASH_Lock();
    return status;
}

/**
  * @brief  Configure the write protection status of user flash area.
  * @retval uint32_t FLASHIF_OK if change is applied.
  */
uint32_t FLASH_If_WriteProtectionClear( void ) {
    FLASH_OBProgramInitTypeDef OptionsBytesStruct1;
    /* Unlock the Flash to enable the flash control register access *************/
    HAL_FLASH_Unlock();
    /* Unlock the Options Bytes *************************************************/
    HAL_FLASH_OB_Unlock();
    OptionsBytesStruct1.RDPLevel = OB_RDP_LEVEL_0;
    OptionsBytesStruct1.OptionType = OPTIONBYTE_WRP;
    OptionsBytesStruct1.WRPArea = OB_WRPAREA_BANK2_AREAA;
    OptionsBytesStruct1.WRPEndOffset = 0x00;
    OptionsBytesStruct1.WRPStartOffset = 0xFF;
    HAL_FLASHEx_OBProgram(&OptionsBytesStruct1);
    OptionsBytesStruct1.WRPArea = OB_WRPAREA_BANK2_AREAB;
    HAL_FLASHEx_OBProgram(&OptionsBytesStruct1);
    return (0);
}

/**
  * @brief  Modify the BFB2 status of user flash area.
  * @param  none
  * @retval HAL_StatusTypeDef HAL_OK if change is applied.
  */
HAL_StatusTypeDef FLASH_If_BankSwitch(void)
{
    FLASH_OBProgramInitTypeDef ob_config;
    HAL_StatusTypeDef result;
    HAL_FLASH_Lock();
    /* Clear OPTVERR bit set on virgin samples */
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPTVERR);
    /* Get the current configuration */
    HAL_FLASHEx_OBGetConfig( &ob_config );
    ob_config.OptionType = OPTIONBYTE_USER;
    ob_config.USERType = OB_USER_BFB2;
    if ((ob_config.USERConfig) & (OB_BFB2_ENABLE)) {
        ob_config.USERConfig = OB_BFB2_DISABLE;
    } else {
        ob_config.USERConfig = OB_BFB2_ENABLE;
    }

    /* Initiating the modifications */
    result = HAL_FLASH_Unlock();
    /* program if unlock is successful */
    if ( result == HAL_OK ) {
        result = HAL_FLASH_OB_Unlock();
    /* program if unlock is successful*/
        if ((READ_BIT(FLASH->CR, FLASH_CR_OPTLOCK) == RESET)) {
            result = HAL_FLASHEx_OBProgram(&ob_config);
        }
        if (result == HAL_OK) {
            HAL_FLASH_OB_Launch();
        }
    }
    return result;
}


void FlashPageErase( uint32_t page, uint32_t banks )
{
    // Check the parameters
    assert_param( IS_FLASH_PAGE( page ) );
    assert_param( IS_FLASH_BANK_EXCLUSIVE( banks ) );
    if( ( banks & FLASH_BANK_1 ) != RESET ) {
        CLEAR_BIT( FLASH->CR, FLASH_CR_BKER );
    } else {
        SET_BIT( FLASH->CR, FLASH_CR_BKER );
    }
    // Proceed to erase the page
    MODIFY_REG( FLASH->CR, FLASH_CR_PNB, ( page << 3 ) );
    SET_BIT( FLASH->CR, FLASH_CR_PER );
    SET_BIT( FLASH->CR, FLASH_CR_STRT );
}

uint8_t EepromMcuWriteBuffer( uint32_t addr, uint8_t *buffer, uint16_t size )
{   
    HAL_StatusTypeDef status = HAL_OK; 
    uint64_t *flash = ( uint64_t* )buffer;
    uint32_t Findpage = (addr - 0x8000000 )>>11;
    uint32_t NumberOfPage = (size >> 11)+1;	
    HAL_FLASH_Unlock( );
    for (uint32_t i = 0 ; i < NumberOfPage; i ++){
        FlashPageErase( Findpage + i, 1 );
    }
    WRITE_REG( FLASH->CR, 0x40000000 );
    for( uint32_t i = 0; i < size; i++ )
    {
        if (HAL_FLASH_Program( FLASH_TYPEPROGRAM_DOUBLEWORD, addr + ( 8 * i ), flash[i] ) == HAL_OK)
        {
            /* Check the written value */
            if (*(uint64_t*)(addr + ( 8 * i )) != flash[i])
            {
            /* Flash content doesn't match SRAM content */
                status = HAL_ERROR;
                break;
            }
        } else {
           /* Error occurred while writing data in Flash memory */
            status = HAL_ERROR;
            break;
        }
    }
    HAL_FLASH_Lock( );
    return status;
}

uint8_t EepromMcuReadBuffer( uint32_t addr, uint8_t *buffer, uint16_t size )
{
    assert_param( buffer != NULL ); 
    //assert_param( addr >= DATA_EEPROM_BASE );
    assert_param( buffer != NULL );
    assert_param( size < ( DATA_EEPROM_END - DATA_EEPROM_BASE ) );
    for( uint32_t i = 0; i < size; i++ ) {
        buffer[i]= *((( uint8_t* )addr)+i);
    }
    return SUCCESS;
}

void EepromMcuSetDeviceAddr( uint8_t addr )
{
    assert_param( FAIL );
}

uint8_t EepromMcuGetDeviceAddr( void )
{
    assert_param( FAIL );
    return 0;
}


/********************************************************************/
/*                         Wake Up local functions                  */
/********************************************************************/
static RTC_HandleTypeDef RtcHandle;


/*!
 * Irq Handler dedicated for wake up It
 * 
 * \param [IN]  void
 * \param [OUT] void         
 */




/*!
* WakeUpAlarmMSecond : Configures the application wake up timer with a delay duration in ms
 * When the timer expires , the rtc block generates an It to wake up the Mcu 
 * \remark this function is not used by the LoRaWAN object, only provided for application purposes.
 * \param [IN]  int delay in ms
 * \param [OUT] void         
 */

void WakeUpAlarmMSecond ( int delay) {
    int DelayMs2tick = delay * 2 + ( ( 6 * delay ) >> 7);
    HAL_RTCEx_SetWakeUpTimer_IT(&RtcHandle, DelayMs2tick, RTC_WAKEUPCLOCK_RTCCLK_DIV16);
}
/*!
* WakeUpAlarmMecond : Configure the wake up timer with a delay duration in second
 * \remark this function is not used by the LoRaWAN object, only provided for application purposes.
 * When the timer expires , the rtc block generates an It to wake up the Mcu 
 * 
 * \param [IN]  int delay in s
 * \param [OUT] void         
 */
void WakeUpAlarmSecond ( int delay) {
    HAL_RTCEx_SetWakeUpTimer_IT(&RtcHandle, delay, 4);
}



/********************************************************************/
/*                 Low Power Timer local functions                  */
/********************************************************************/

/*!
 * Irq Handler dedicated for Low power Timer reserved for lorawan layer
 *\remark LowPowerTimerLora.timerISR() is used to callback the timer Interupt Service Routine of the current obj LoraWanObject 
 * \param [IN]  void
 * \param [OUT] void         
 */




/********************************************************************/
/*                        Gpio Handler  functions                   */
/********************************************************************/

void  IrqHandlerRadio ( void ){
    int pintmp = TX_RX_IT;
    if ( pintmp < 15 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << pintmp) );
    } else if ( pintmp < 31 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << ( pintmp - 16 )) );
    } else if ( pintmp < 47 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << ( pintmp - 32 )) );
    } else if ( pintmp < 63 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << ( pintmp - 48 )) );
    } else if ( pintmp < 79 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << ( pintmp - 64 )) );
    } else if ( pintmp < 95 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << ( pintmp - 80 )) );
    } else if ( pintmp < 111 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << ( pintmp - 96 )) );
    } 
    pintmp = RX_TIMEOUT_IT;
    if ( pintmp < 15 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << pintmp) );
    } else if ( pintmp < 31 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << ( pintmp - 16 )) );
    } else if ( pintmp < 47 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << ( pintmp - 32 )) );
    } else if ( pintmp < 63 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << ( pintmp - 48 )) );
    } else if ( pintmp < 79 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << ( pintmp - 64 )) );
    } else if ( pintmp < 95 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << ( pintmp - 80 )) );
    } else if ( pintmp < 111 ) {
        HAL_GPIO_EXTI_IRQHandler( (1 << ( pintmp - 96 )) );
    } 
   
    mcu.ExtISR();
}


    

/*************************************************************/
/*           Mcu Object Definition Constructor               */
/*************************************************************/
McuSTM32L4::McuSTM32L4(PinName mosi, PinName miso, PinName sclk )  {
    Func = DoNothing; // don't modify
    obj = NULL;       // don't modify
    McuMosi = mosi;   // don't modify
    McuMiso = miso;   // don't modify
    McuSclk = sclk;   // don't modify
}     
McuSTM32L4::~McuSTM32L4(){
      // to be completed by mcu providers
} 

/*******************************************/
/*                  Mcu Init               */
/*******************************************/
void McuSTM32L4::InitMcu( void ) {
    // system clk Done with mbed to be completed by mcu providers if mbed is removed
      /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_LPTIM1_Init();
  MX_USART2_UART_Init();
  MX_SPI1_Init();
  MX_RTC_Init();
 // MX_I2C1_Init();
  //MX_WWDG_Init();
  
  
  

    /*For dual boot */
    FLASH_If_WriteProtectionClear();
    /* Test from which bank the program runs */
    /* Bit 8 FB_MODE: Flash Bank mode selection
    0: Flash Bank 1 mapped at 0x0800 0000 (and aliased @0x0000 0000(1)) and Flash Bank 2
    mapped at 0x0808 0000 (and aliased at 0x0008 0000)
    1: Flash Bank2 mapped at 0x0800 0000 (and aliased @0x0000 0000(1)) and Flash Bank 1
    mapped at 0x0808 0000 (and aliased at 0x0008 0000)
            */
    BankActive = READ_BIT(SYSCFG->MEMRMP, SYSCFG_MEMRMP_FB_MODE);
    if (BankActive == 0) { //bank 1
        //DEBUG_MSG("Dual Boot is activated and code running on Bank 1 \n");
    } else {
        //DEBUG_MSG("Dual Boot is activated and code running on Bank 2 \n");
        uint32_t result = FLASH_If_Erase( BankActive ); //Erase the 0x8080000
        if (result == FLASHIF_OK) {
       // DEBUG_MSG("Copying BANK1 to BANK2\n");
        result = FLASH_If_Write( FLASH_START_BANK2, (uint32_t*)FLASH_START_BANK1, 20480);
        }
        if (result != FLASHIF_OK) {
         //   DEBUG_PRINTF("Failure! %d \n",result);
        } else {
          //  DEBUG_MSG("Sucess!\n");
            FLASH_If_BankSwitch();
            NVIC_SystemReset();
        }
    }
}

void  McuSTM32L4::Init_Irq ( PinName pin) {

}    
/******************************************************************************/
/*                                Mcu Spi Api                                 */
/******************************************************************************/
    /** Create a SPI master connected to the specified pins
    *
    *  @param mosi SPI Master Out, Slave In pin
    *  @param miso SPI Master In, Slave Out pin
    *  @param sclk SPI Clock pin
    */

void McuSTM32L4::InitSpi ( ){

}
    /** Write to the SPI Slave and return the response
    *
    *  @param value Data to be sent to the SPI slave
    *
    *  @returns
    *    Response from the SPI slave
    */
uint8_t McuSTM32L4::SpiWrite(int value){
          uint8_t rxData = 0;
        while( LL_SPI_IsActiveFlag_TXE ( SPI1 ) == 0  ){};
        LL_SPI_TransmitData8 (SPI1, uint8_t (value&0xFF));
        while( LL_SPI_IsActiveFlag_RXNE ( SPI1 ) == 0 ){};
        rxData =  LL_SPI_ReceiveData8( SPI1 );
        return (rxData);
}
/******************************************************************************/
/*                                Mcu Flash Api                               */
/******************************************************************************/
int McuSTM32L4::RestoreContext(uint8_t *buffer, uint32_t addr, uint32_t size){
     /* have to be implemented by mcu providers
    the following code propose a lite implementation without any error cases*/
    uint16_t sizet = size & 0xFFFF;
    EepromMcuReadBuffer( addr, buffer, sizet );
    return ( 0 ); 
}
static uint8_t copyPage [2048] ;
int McuSTM32L4::WriteFlashWithoutErase(uint8_t *buffer, uint32_t addr, uint32_t size){

    int findPage = 0 ;
    uint32_t findByteAdress = 0 ;
    uint32_t findLastAdress = 0 ;
    int status = 0;
    uint32_t i;
    uint32_t flashBaseAdress;    
    uint64_t *flash = ( uint64_t* )copyPage;
    assert_param( buffer != NULL );
    assert_param( size < ( 2048 ) );
    findPage = ((addr - 0x8080000 )) >> 11 ;  // 2048 page size;
    findByteAdress = ( (addr - 0x8080000 )  - ( findPage << 11 ));
    findLastAdress = findByteAdress + size ; //if >2048 across two pages !!
    flashBaseAdress = 0x8080000 ;
    HAL_FLASH_Unlock( );

    for( i = 0; i < 2048; i++ ) {
        copyPage[i]= *(( uint8_t* )(flashBaseAdress + (findPage * 2048))+i);
    }
    FlashPageErase(   findPage, 2 );
    WRITE_REG( FLASH->CR, 0x40000000 );
    if ( findLastAdress < 2048) { // all is done on the same page
        for( i = 0; i < size; i++ ) {
            copyPage[i + findByteAdress] = buffer [i];
        }
        for( i = 0; i < 256; i++ ) {
            status += HAL_FLASH_Program( FLASH_TYPEPROGRAM_DOUBLEWORD, flashBaseAdress + (findPage * 2048) + ( 8 * i ), flash[i] );
        }
    } else { // require 2 pages
        for( i = 0; i < (2048-findByteAdress); i++ ) {
            copyPage[i + findByteAdress] = buffer [i];
        }
        for( i = 0; i < 256; i++ ) {
            status += HAL_FLASH_Program( FLASH_TYPEPROGRAM_DOUBLEWORD, flashBaseAdress + (findPage * 2048) + ( 8 * i ),  flash[i] );
        }
        for( i = 0; i < 2048; i++ ) { //copy the next page
            copyPage[i]= *(( uint8_t* )(flashBaseAdress + ((findPage + 1) * 2048))+i);
        }
        FlashPageErase(   1 + findPage, 2 );
        WRITE_REG( FLASH->CR, 0x40000000 );
        for( i = 0; i < (findLastAdress - 2048); i++ ) {
            copyPage[i ] =  buffer [i + 2048 - findByteAdress ];
        }
        for( i = 0; i < 256; i++ ) {
            status += HAL_FLASH_Program( FLASH_TYPEPROGRAM_DOUBLEWORD, flashBaseAdress + ((findPage + 1) * 2048) + ( 8 * i ),  flash[i] );
        }
    }
    if ( status > 0 ) {
        // DEBUG_MSG("ERROR HAL FLASH \n");
    }
    HAL_FLASH_Lock( );
    return ( 0 ); 
}



int McuSTM32L4::StoreContext(const void *buffer, uint32_t addr, uint32_t size){
    /* have to be implemented by mcu providers
    the following code propose a lite implementation without any error cases
    this section have to be very robust, have to support failure mode such as  power off during flash programmation    
    This basic implementation suppose that the addr is 4 Bytes aligned and suppose also that the size can be divide by 4.
    */
    uint16_t sizet = size & 0xFFFF;
    while ( EepromMcuWriteBuffer( addr,  (uint8_t*) buffer, sizet ) != HAL_OK) { // in case of infinite error watchdog will expire
        mwait_ms ( 300 );
    }
    mwait_ms ( 300 );
    return ( 0 ); 
} 
   


/******************************************************************************/
/*                                Mcu RTC Api                                 */
/******************************************************************************/

void McuSTM32L4::RtcInit (void)
{

}

uint32_t McuSTM32L4::RtcGetTimeMs( void )
{
    RTC_DateTypeDef dateStruct;
    RTC_TimeTypeDef timeStruct;
    struct tm timeinfo;
    RtcHandle.Instance = RTC;
    HAL_RTC_GetTime(&RtcHandle, &timeStruct, FORMAT_BIN);
    HAL_RTC_GetDate(&RtcHandle, &dateStruct, FORMAT_BIN);
    timeinfo.tm_wday = dateStruct.WeekDay;
    timeinfo.tm_mon  = dateStruct.Month ;
    timeinfo.tm_mday = dateStruct.Date;
    timeinfo.tm_year = dateStruct.Year ;
    timeinfo.tm_hour = timeStruct.Hours;
    timeinfo.tm_min  = timeStruct.Minutes;
    timeinfo.tm_sec  = timeStruct.Seconds;
    // Convert to timestamp
    time_t t = mktime(&timeinfo);
    return ( ( t * 1000 ) + ( 999 - ( ( timeStruct.SubSeconds *999) / 255 ) ) );  // get time en ms
}

uint32_t McuSTM32L4::RtcGetTimeSecond( void )
{
    RTC_DateTypeDef dateStruct;
    RTC_TimeTypeDef timeStruct;
    struct tm timeinfo;
    RtcHandle.Instance = RTC;

    HAL_RTC_GetTime(&RtcHandle, &timeStruct, FORMAT_BIN);
    HAL_RTC_GetDate(&RtcHandle, &dateStruct, FORMAT_BIN);
    timeinfo.tm_wday = dateStruct.WeekDay;
    timeinfo.tm_mon  = dateStruct.Month ;
    timeinfo.tm_mday = dateStruct.Date;
    timeinfo.tm_year = dateStruct.Year ;
    timeinfo.tm_hour = timeStruct.Hours;
    timeinfo.tm_min  = timeStruct.Minutes;
    timeinfo.tm_sec  = timeStruct.Seconds;
    time_t t = mktime(&timeinfo);
    return ( t );
}

/******************************************************************************/
/*                                Mcu Sleep Api                               */
/******************************************************************************/
void McuSTM32L4::GotoSleepSecond (int duration ) {
#if LOW_POWER_MODE == 1
    int cpt = duration ;
    while ( cpt > ( WATCH_DOG_PERIOD_RELEASE ) ) {
        cpt -= WATCH_DOG_PERIOD_RELEASE ;
        WakeUpAlarmSecond( WATCH_DOG_PERIOD_RELEASE );
        sleep();
        WatchDogRelease ( );
    }
    WakeUpAlarmSecond( cpt );
    sleep();
# else
    int cpt = duration ;
    WatchDogRelease ( );
    while ( cpt > ( WATCH_DOG_PERIOD_RELEASE ) ) {
        cpt -= WATCH_DOG_PERIOD_RELEASE ;
        mwait( WATCH_DOG_PERIOD_RELEASE );
        WatchDogRelease ( );
    }
    mwait( cpt );
    WatchDogRelease ( );
#endif
}

void McuSTM32L4::GotoSleepMSecond (int duration ) {
#if LOW_POWER_MODE == 1
    WakeUpAlarmMSecond ( duration );
    sleep();
    WatchDogRelease ( );
# else
    mwait_ms ( duration ) ;
    WatchDogRelease ( );
#endif
}


/******************************************************************************/
/*                             Mcu WatchDog Api                               */
/******************************************************************************/

static IWDG_HandleTypeDef Iwdg;

/*!
 * Watch Dog Init And start with a period befor ereset set to 32 seconds
*/
void McuSTM32L4::WatchDogStart ( void ) {
 
}
/*!
 * Watch Dog Release
*/
void McuSTM32L4::WatchDogRelease ( void ) {
    HAL_IWDG_Refresh(&Iwdg);
};
/******************************************************************************/
/*                             Mcu LOwPower timer Api                         */
/******************************************************************************/
void McuSTM32L4::LowPowerTimerLoRaInit ( ) {

    Func = DoNothing;
    obj = NULL;
    //Initialize delay Systick timer for wait function
    //TM_DELAY_Init();
};
/*!
 * LowPowerTimerLoRa AttachMsecond
 *
 * \param void (* _Func) (void *) a static method member of the current Obj
 * \param *_obj a pointer to the current objet
 * \param int delay in ms delay should be between 1ms and 16s.
 * \param [OUT] void         
 * \remark the code  Func =  _Func ; and obj  = _obj; isn't mcu dependent
 * \remark starts the LoRaWAN dedicated timer and attaches the IRQ to the handling Interupt SErvice Routine in the LoRaWAN object.
 */
void McuSTM32L4::StartTimerMsecond ( void (* _Func) (void *) , void * _obj, int delay){
    Func =  _Func ;
    obj  = _obj;
    int DelayMs2tick = delay * 2 + ( ( 6 * delay ) >> 7);
    HAL_LPTIM_TimeOut_Start_IT(&hlptim1, 65535, DelayMs2tick); // MCU specific
};

/******************************************************************************/
/*                           Mcu Gpio Api                                     */
/******************************************************************************/
void McuSTM32L4::SetValueDigitalOutPin ( PinName Pin, int Value ){
        uint16_t pintmp ;
        if ( Pin < 15 ) {
            pintmp = (uint16_t) Pin;
            HAL_GPIO_WritePin(GPIOA, ( 1 << pintmp ), (GPIO_PinState) Value);
        } else if ( Pin < 31 ) {
            pintmp = (uint16_t)Pin-16;
            HAL_GPIO_WritePin(GPIOB, ( 1 << pintmp ), (GPIO_PinState) Value);
        } else if ( Pin < 47 ) {
            pintmp = Pin-32;
            HAL_GPIO_WritePin(GPIOC, ( 1 << pintmp ), (GPIO_PinState) Value);
        } else if ( Pin < 63 ) {
            pintmp = Pin-48;
          HAL_GPIO_WritePin(GPIOD, ( 1 << pintmp ), (GPIO_PinState) Value);
        } else if ( Pin < 79 ) {
            pintmp = Pin-64;
            HAL_GPIO_WritePin(GPIOE, ( 1 << pintmp ), (GPIO_PinState) Value);
        } else if ( Pin < 95 ) {
            pintmp = Pin-80;
            HAL_GPIO_WritePin(GPIOF, ( 1 << pintmp ), (GPIO_PinState) Value);
        } else  {
            pintmp = Pin-96;
            HAL_GPIO_WritePin(GPIOG, ( 1 << pintmp ), (GPIO_PinState) Value);
        }
};
int McuSTM32L4::GetValueDigitalInPin ( PinName Pin ){
    uint16_t pintmp ;
    if ( Pin < 15 ) {
        pintmp = (uint16_t) Pin;
        return ( (int) HAL_GPIO_ReadPin(GPIOA, ( 1 << pintmp ) ) );
    } else if ( Pin < 31 ) {
        pintmp = (uint16_t)Pin-16;
        return ( (int) HAL_GPIO_ReadPin(GPIOB, ( 1 << pintmp ) ) );
    } else if ( Pin < 47 ) {
        pintmp = Pin-32;
       return ( (int) HAL_GPIO_ReadPin(GPIOC, ( 1 << pintmp ) ) );
    } else if ( Pin < 63 ) {
        pintmp = Pin-48;
      return ( (int) HAL_GPIO_ReadPin(GPIOD, ( 1 << pintmp ) ) );
    } else if ( Pin < 79 ) {
        pintmp = Pin-64;
        return ( (int) HAL_GPIO_ReadPin(GPIOE, ( 1 << pintmp ) ) );
    } else if ( Pin < 95 ) {
        pintmp = Pin-80;
        return ( (int) HAL_GPIO_ReadPin(GPIOF, ( 1 << pintmp ) ) );
    } else {
        pintmp = Pin-96;
        return ( (int) HAL_GPIO_ReadPin(GPIOG, ( 1 << pintmp ) ) );
    }
};


void  McuSTM32L4::AttachInterruptIn       (  void (* _Funcext) (void *) , void * _objext) {
    Funcext =  _Funcext ;
    objext  = _objext;
    userIt  = 0 ; 
};

/******************************************************************************/
/*                           Mcu Uart Api                                     */
/******************************************************************************/

 

void vprint(const char *fmt, va_list argp)
{
#if DEBUG_TRACE == 1
    char string[200];
    if(0 < vsprintf(string,fmt,argp)) // build string
    {
        HAL_UART_Transmit(&huart2, (uint8_t*)string, strlen(string), 0xffffff); // send message via UART
    }
#endif
}



void McuSTM32L4::UartInit ( void ) {
#if DEBUG_TRACE == 1
  
#endif
};
void McuSTM32L4::MMprint( const char *fmt, ...){
#if DEBUG_TRACE == 1
  va_list argp;
  va_start(argp, fmt);
  vprint(fmt, argp);
  va_end(argp);
#endif
};
