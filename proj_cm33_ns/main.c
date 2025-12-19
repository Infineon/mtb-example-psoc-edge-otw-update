/*******************************************************************************
* File Name        : main.c
*
* Description      : This source file contains the main routine for non-secure
*                    application in the CM33 CPU
*
* Related Document : See README.md
*
********************************************************************************
 * (c) 2023-2025, Infineon Technologies AG, or an affiliate of Infineon
 * Technologies AG.  SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*******************************************************************************/

/*******************************************************************************
* Header Files
*******************************************************************************/

#include "cybsp.h"
#include "cy_pdl.h"
#include "cycfg_pins.h"
#include "mtb_hal.h"
#include "retarget_io_init.h"
#include "cy_dfu.h"
#include "mtb_serial_memory.h"
#include "mtb_hal_i2c.h"
#include "cy_scb_i2c.h"
#include "cy_sysint.h"
#include "transport_i2c.h"
#include "transport_emusb_cdc.h"
#include "transport_emusb_hid.h"
#include "USB.h"
#include "USB_HID.h"
#include "cy_dfu_logging.h"
#if defined(_MTB_HAL_DRIVER_AVAILABLE_IRQ) && (_MTB_HAL_DRIVER_AVAILABLE_IRQ)
#include "mtb_hal_irq_impl.h"
#endif /* defined(_MTB_HAL_DRIVER_AVAILABLE_IRQ) && (_MTB_HAL_DRIVER_AVAILABLE_IRQ) */

/*******************************************************************************
 * Macros
 *******************************************************************************/

/* App boot address for CM55 project */
#define CM55_APP_BOOT_ADDR (CYMEM_CM33_0_m55_nvm_START + \
                            CYBSP_MCUBOOT_HEADER_SIZE)

/* The timeout value in microsecond used to wait for CM55 core to be booted */
#define CM55_BOOT_WAIT_TIME_USEC (10U)

/* Number of DFU transports supported */
#define MAX_DFU_TRANSPORT (3)

/* Default DFU transport */
#define DEFAULT_DFU_TRANSPORT (CY_DFU_I2C)

/* Timeout for Cy_DFU_Continue(), in milliseconds */
#define DFU_SESSION_TIMEOUT_MS (20u)

/* DFU idle timeout: 300 seconds */
#define DFU_IDLE_TIMEOUT_MS (300000u)

/* DFU command timeout: 5 seconds */
#define DFU_COMMAND_TIMEOUT_MS (5000u)

/* USER BTN1 Interrupt Priority*/
#define GPIO_INTERRUPT_PRIORITY (7u)

/* Select LED based on the Image Type*/
#ifdef BOOT_IMAGE
    #define LED_TOGGLE_INTERVAL_MS (1000u)
    #define DFU_LED_PORT CYBSP_USER_LED1_PORT
    #define DFU_LED_PIN CYBSP_USER_LED1_PIN
    #define APP_VERSION "1.0.0"
#elif defined(UPDATE_IMAGE)
    #define LED_TOGGLE_INTERVAL_MS (500u)
    #define DFU_LED_PORT CYBSP_USER_LED2_PORT
    #define DFU_LED_PIN CYBSP_USER_LED2_PIN
    #define APP_VERSION "2.0.0"
#endif

/*******************************************************************************
 * Global Variables
 *******************************************************************************/

/* For the Serial Memory usage */
static mtb_serial_memory_t smif0_obj;
static cy_stc_smif_mem_context_t smif0_mem_cxt;
static cy_stc_smif_mem_info_t smif0_mem_info;

/* For DFU Transport switching */
static cy_en_dfu_transport_t dfu_transport = DEFAULT_DFU_TRANSPORT;
static cy_en_dfu_transport_t new_dfu_transport = DEFAULT_DFU_TRANSPORT;
const static cy_en_dfu_transport_t dfu_transport_supported[MAX_DFU_TRANSPORT] = 
    {CY_DFU_I2C, CY_DFU_USB_CDC, CY_DFU_USB_HID};

/* I2C transport HAL object  */
static mtb_hal_i2c_t             dfuI2cHalObj;
static cy_stc_scb_i2c_context_t  dfuI2cContext;

/* Data structure for emUSB-CDC-Device */
static const USB_DEVICE_INFO USB_DeviceInfo_CDC =
{
    .VendorId = 0x058B,
    .ProductId = 0xF21D,
    .sVendorName = "Infineon",
    .sProductName = "PSOC-DFU-USB-CDC",
    .sSerialNumber = "0132456789"
};

/* Data structure for emUSB-HID-Device */
static const USB_DEVICE_INFO USB_DeviceInfo_HID =
{
    .VendorId = 0x058B,
    .ProductId = 0xF21D,
    .sVendorName = "Infineon",
    .sProductName = "PSOC-DFU-USB-HID",
    .sSerialNumber = "9876543210"
};

/*******************************************************************************
 * Function Prototypes
 *******************************************************************************/

static char *dfu_status_in_str(cy_en_dfu_status_t dfu_status);
static void dfu_transport_check(void);
static void user_btn1_isr(void);
static void dfuI2cIsr(void);
static void dfuI2cTransportCallback(cy_en_dfu_transport_i2c_action_t action);
static void dfuUsbCdcTransportCallback(cy_en_dfu_transport_usb_cdc_action_t action);
static void dfuUsbHidTransportCallback(cy_en_dfu_transport_usb_hid_action_t action);
static void dfu_transport_init(cy_en_dfu_transport_t transport);
static void dfu_usb_hid_transport_init(void);
static void dfu_usb_cdc_transport_init(void);
static void dfu_i2c_transport_init(void);

/*******************************************************************************
 * Function Name: main
 ********************************************************************************
 * Summary:
 *  This is the main function. it initialize DFU to receive and program application
 *  images to device memory. It also blinks an LED.
 *
 * Parameters:
 *  none
 *
 * Return:
 *  int
 *
 *******************************************************************************/
int main(void)
{
    uint32_t count = 0;
    cy_rslt_t result;
    cy_en_dfu_status_t dfu_status = CY_DFU_ERROR_UNKNOWN;
    uint32_t dfu_state = CY_DFU_STATE_NONE;

    /* Buffer to store DFU commands. */
    CY_ALIGN(4)
    static uint8_t dfu_buffer[CY_DFU_SIZEOF_DATA_BUFFER];
    /* Buffer for DFU data packets for transport API. */
    CY_ALIGN(4)
    static uint8_t dfu_packet[CY_DFU_SIZEOF_CMD_BUFFER];

    /* DFU params, used to configure DFU. */
    cy_stc_dfu_params_t dfu_params =
    {
        .timeout = DFU_SESSION_TIMEOUT_MS,
        .dataBuffer = &dfu_buffer[0],
        .packetBuffer = &dfu_packet[0],
    };

    /* Interrupt config structure */
    cy_stc_sysint_t intrCfg =
    {
        .intrSrc = CYBSP_USER_BTN_IRQ,
        .intrPriority = GPIO_INTERRUPT_PRIORITY
    };

    /* Initialize the device and board peripherals */
    result = cybsp_init();

    /* Board init failed. Stop program execution */
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Enable global interrupts */
    __enable_irq();

    /* Initialize retarget-io middleware */
    init_retarget_io();

    /* Register interrupt callback for USER_BTN1 */
    Cy_SysInt_Init(&intrCfg, &user_btn1_isr);

    /* Enable the interrupt in the NVIC */
    Cy_GPIO_ClearInterrupt(CYBSP_USER_BTN1_PORT, CYBSP_USER_BTN1_PIN);
    NVIC_ClearPendingIRQ(CYBSP_USER_BTN1_IRQ);
    NVIC_EnableIRQ(intrCfg.intrSrc);

    printf("\r\n\n***************** PSOC Edge MCU: DFU Code Example *****************\r\n\n");

    printf("For more projects, visit our code examples repositories:\r\n\n");

    printf("https://github.com/Infineon/Code-Examples-for-ModusToolbox-Software\r\n\n");

    printf("\n====================================================================\n");
    printf("\r [DFU APP] Version: %s | CPU: CM33\r", APP_VERSION);
    printf("\n====================================================================\n");

    /* Initialize serial memory middleware */
    result = mtb_serial_memory_setup(&smif0_obj, MTB_SERIAL_MEMORY_CHIP_SELECT_1,
                                     CYBSP_SMIF_CORE_0_XSPI_FLASH_hal_config.base,
                                     CYBSP_SMIF_CORE_0_XSPI_FLASH_hal_config.clock,
                                     &smif0_mem_cxt, &smif0_mem_info, &smif0BlockConfig);
    if (result != CY_RSLT_SUCCESS)
    {
        printf("Serial memory setup failed\r\n");
        CY_ASSERT(0);
    }

    /* Enable CM55 */
    Cy_SysEnableCM55(MXCM55, CM55_APP_BOOT_ADDR, CM55_BOOT_WAIT_TIME_USEC);

    /* Add External memory to DFU middleware */
    Cy_DFU_AddExtMemory(&smif0_obj);

    /* Initialize DFU Structure. */
    dfu_status = Cy_DFU_Init(&dfu_state, &dfu_params);
    if (CY_DFU_SUCCESS != dfu_status)
    {
        printf("DFU initialization failed \r\n");
        CY_ASSERT(0);
    }

    printf("\r\n STARTING DFU Transport ");
    /* Initialize DFU communication. */
    dfu_transport_init(dfu_transport);
    Cy_DFU_TransportStart(dfu_transport);

    for (;;)
    {
        dfu_status = Cy_DFU_Continue(&dfu_state, &dfu_params);
        count++;
        if (CY_DFU_STATE_FINISHED == dfu_state)
        {
            printf("\r\n DFU_STATE_FINISHED - %s \r\n Launching Bootloader\r", dfu_status_in_str(dfu_status));
            Cy_SysLib_Delay(1000);

            /* All went well, Restarting the device to complete the upgrade */
            NVIC_SystemReset();
        }
        else if (CY_DFU_STATE_FAILED == dfu_state)
        {
            printf("\r DFU_STATE_FAILED - %s \r", dfu_status_in_str(dfu_status));

            /* An error occurred. Handle it here.
             * This code just restarts the DFU */
            count = 0u;
            Cy_DFU_Init(&dfu_state, &dfu_params);
            dfu_transport_check();
        }
        else if (dfu_state == CY_DFU_STATE_UPDATING)
        {
            if (dfu_status == CY_DFU_SUCCESS)
            {
                count = 0u;
            }
            else if (dfu_status == CY_DFU_ERROR_TIMEOUT)
            {
                if (count >= (DFU_COMMAND_TIMEOUT_MS / DFU_SESSION_TIMEOUT_MS))
                {
                    /* No command has been received since last 5 seconds. Restart DFU */
                    count = 0u;
                    Cy_DFU_Init(&dfu_state, &dfu_params);
                    dfu_transport_check();
                }
            }
            else
            {
                /* Delay because Transport still may be sending error response to a host. */
                Cy_SysLib_Delay(DFU_SESSION_TIMEOUT_MS);

                /* Restart DFU. */
                count = 0u;
                dfu_transport_check();
            }
        }
        else
        {
            /* dfu_state == CY_DFU_STATE_NONE */
            if (count >= (DFU_IDLE_TIMEOUT_MS / DFU_SESSION_TIMEOUT_MS))
            {
                /* No DFU request received in 300 seconds, lets start over.
                 * Final application can change it to either assert, reboot,
                 * enter low power mode etc, based on usecase requirements. */
                count = 0;
            }

            dfu_transport_check();
        }

        /* Blink once per second */
        if ((count % (LED_TOGGLE_INTERVAL_MS / DFU_SESSION_TIMEOUT_MS)) == 0u)
        {
            /* Invert the USER LED state */
            Cy_GPIO_Inv(DFU_LED_PORT, DFU_LED_PIN);
        }

        Cy_SysLib_Delay(1);
    }
}

/*******************************************************************************
 * Function Name: dfu_status_in_str
 ********************************************************************************
 * Summary:
 * This is the function to convert DFU status in elaborative text
 *
 * Parameters:
 *  dfu_status
 *
 * Return:
 *  string pointer
 *
 *******************************************************************************/
static char *dfu_status_in_str(cy_en_dfu_status_t dfu_status)
{
    switch (dfu_status)
    {
        case CY_DFU_SUCCESS:
            return "DFU: success";

        case CY_DFU_ERROR_VERIFY:
            return "DFU:Verification failed";

        case CY_DFU_ERROR_LENGTH:
            return "DFU: The length the packet is outside of the expected range";

        case CY_DFU_ERROR_DATA:
            return "DFU: The data in the received packet is invalid";

        case CY_DFU_ERROR_CMD:
            return "DFU: The command is not recognized";

        case CY_DFU_ERROR_CHECKSUM:
            return "DFU: The checksum does not match the expected value ";

        case CY_DFU_ERROR_ADDRESS:
            return "DFU: The wrong address";

        case CY_DFU_ERROR_TIMEOUT:
            return "DFU: The command timed out";

        case CY_DFU_ERROR_BAD_PARAM:
            return "DFU: One or more of input parameters are invalid";

        case CY_DFU_ERROR_UNKNOWN:
            return "DFU: did not recognize error";

        default:
            return "Not recognized DFU status code";
    }
}

/*******************************************************************************
 * Function Name: dfu_transport_check
 ********************************************************************************
 * Summary:
 * This is the function to check for pending transport switch request. It switches
 * DFU transport dynamically on GPIO interrupt request.
 *
 * Parameters:
 *  void
 *
 * Return:
 *  void
 *
 *******************************************************************************/
static void dfu_transport_check(void)
{
    /* Check of DFU transport switch is requested*/
    if (new_dfu_transport != dfu_transport)
    {
        /* Stop the current DFU transport */
        Cy_DFU_TransportReset();
        Cy_DFU_TransportStop();

        /* Start the new DFU transport */
        printf("\r\n SWITCHING DFU Transport to ");
        dfu_transport_init(new_dfu_transport);
        Cy_DFU_TransportStart(new_dfu_transport);

        /* Conclude the switch */
        dfu_transport = new_dfu_transport;
    }
    else
    {
        /* DFU transport switch is not requested, reset the transport */
        Cy_DFU_TransportReset();
    }
}

/*******************************************************************************
 * Function Name: user_btn1_isr
 ********************************************************************************
 * Summary:
 *  USER BTN1 interrupt callback
 *
 * Parameters:
 *  void
 *
 * Return:
 *  void
 *
 *******************************************************************************/
static void user_btn1_isr(void)
{
    static uint8_t btn_count = 0;

    /* Delay to handle de-bouncing  */
    Cy_SysLib_Delay(500);

    /* Clear the Interrupt */
    Cy_GPIO_ClearInterrupt(CYBSP_USER_BTN_PORT, CYBSP_USER_BTN_PIN);
    NVIC_ClearPendingIRQ(CYBSP_USER_BTN1_IRQ);

    if (new_dfu_transport == dfu_transport)
    {
        /* Trigger DFU transport switch request  */
        btn_count = (btn_count + 1) % MAX_DFU_TRANSPORT;
        new_dfu_transport = dfu_transport_supported[btn_count];
    }
    else
    {
        /* Previous switch request is not processed yet, ignore the new request*/
    }
}

/*******************************************************************************
 * Function Name: dfuI2cIsr
 ********************************************************************************
 * Summary:
 *  I2C interrupt callback
 *
 * Parameters:
 *  void
 *
 * Return:
 *  void
 *
 *******************************************************************************/
static void dfuI2cIsr(void)
{
    mtb_hal_i2c_process_interrupt(&dfuI2cHalObj);
}

/*******************************************************************************
 * Function Name: dfuI2CTransportCallback
 ********************************************************************************
 * Summary:
 *  Callback to enable or disable DFU I2C transport
 *
 * Parameters:
 *  action : Callback trigger
 *
 * Return:
 *  void
 *
 *******************************************************************************/
static void dfuI2cTransportCallback(cy_en_dfu_transport_i2c_action_t action)
{
    if (action == CY_DFU_TRANSPORT_I2C_INIT)
    {
        Cy_SCB_I2C_Enable(DFU_I2C_HW);
    }
    else if (action == CY_DFU_TRANSPORT_I2C_DEINIT)
    {
        Cy_SCB_I2C_Disable(DFU_I2C_HW, &dfuI2cContext);
    }
}

/*******************************************************************************
 * Function Name: dfuUsbCdcTransportCallback
 ********************************************************************************
 * Summary:
 *  Callback to enable or disable DFU USB CDC transports
 *
 * Parameters:
 *  action : Callback trigger
 *
 * Return:
 *  void
 *
 *******************************************************************************/
static void dfuUsbCdcTransportCallback(cy_en_dfu_transport_usb_cdc_action_t action)
{
    switch (action)
    {
        case CY_DFU_TRANSPORT_USB_CDC_INIT:
            USBD_Init();
            break;
        case CY_DFU_TRANSPORT_USB_CDC_ENABLE:
            USBD_SetDeviceInfo(&USB_DeviceInfo_CDC);
            USBD_Start();
            break;
        case CY_DFU_TRANSPORT_USB_CDC_DEINIT:
            USBD_DeInit();
            break;
        case CY_DFU_TRANSPORT_USB_CDC_DISABLE:
            USBD_Stop();
            break;
        default:
            CY_ASSERT(0);
            break;
    }
}

/*******************************************************************************
 * Function Name: dfuUsbHidTransportCallback
 ********************************************************************************
 * Summary:
 *  Callback to enable or disable DFU USB HID transports
 *
 * Parameters:
 *  action : Callback trigger
 *
 * Return:
 *  void
 *
 *******************************************************************************/
static void dfuUsbHidTransportCallback(cy_en_dfu_transport_usb_hid_action_t action)
{
    switch (action)
    {
        case CY_DFU_TRANSPORT_USB_HID_INIT:
            USBD_Init();
            break;
        case CY_DFU_TRANSPORT_USB_HID_ENABLE:
            USBD_SetDeviceInfo(&USB_DeviceInfo_HID);
            USBD_Start();
            break;
        case CY_DFU_TRANSPORT_USB_HID_DEINIT:
            USBD_DeInit();
            break;
        case CY_DFU_TRANSPORT_USB_HID_DISABLE:
            USBD_Stop();
            break;
        default:
            CY_ASSERT(0);
            break;
    }
}

/*******************************************************************************
 * Function Name: dfu_transport_init
 ********************************************************************************
 * Summary:
 *  Configure requested DFU transport to receive data from DFU Host Tool
 *
 * Parameters:
 *  transport   DFU transport to initialize
 *
 * Return:
 *  void
 *
 *******************************************************************************/
static void dfu_transport_init(cy_en_dfu_transport_t transport)
{
    switch (transport)
    {
        case CY_DFU_I2C:
            dfu_i2c_transport_init();
            printf("I2C\r\n");
            break;
        case CY_DFU_USB_CDC:
            dfu_usb_cdc_transport_init();
            printf("USB-CDC\r\n");
            break;
        case CY_DFU_USB_HID:
            dfu_usb_hid_transport_init();
            printf("USB-HID\r\n");
            break;
        default:
            break;
    }
}

/*******************************************************************************
 * Function Name: dfu_usb_cdc_transport_init
 ********************************************************************************
 * Summary:
 *  Configure DFU USB CDC transport to receive data from DFU Host Tool
 *
 * Parameters:
 *  void
 *
 * Return:
 *  void
 *
 *******************************************************************************/
static void dfu_usb_cdc_transport_init()
{
    cy_stc_dfu_transport_usb_cdc_cfg_t usb_cdc_TransportCfg =
    {
        .callback = (Cy_DFU_TransportUsbCdcCallback)dfuUsbCdcTransportCallback,
    };

    Cy_DFU_TransportUsbCdcConfig(&usb_cdc_TransportCfg);
}

/*******************************************************************************
 * Function Name: dfu_usb_hid_transport_init
 ********************************************************************************
 * Summary:
 *  Configure DFU USB HID transport to receive data from DFU Host Tool
 *
 * Parameters:
 *  void
 *
 * Return:
 *  void
 *
 *******************************************************************************/
static void dfu_usb_hid_transport_init()
{
    cy_stc_dfu_transport_usb_hid_cfg_t usb_hid_TransportCfg =
    {
        .callback = (Cy_DFU_TransportUsbHidCallback)dfuUsbHidTransportCallback,
    };

    Cy_DFU_TransportUsbHidConfig(&usb_hid_TransportCfg);
}

/*******************************************************************************
 * Function Name: dfu_i2c_transport_init
 ********************************************************************************
 * Summary:
 *  Configure DFU I2C transport to receive data from DFU Host Tool
 *
 * Parameters:
 *  void
 *
 * Return:
 *  void
 *
 *******************************************************************************/
static void dfu_i2c_transport_init()
{
    cy_en_scb_i2c_status_t pdlI2cStatus;
    cy_en_sysint_status_t pdlSysIntStatus;
    cy_rslt_t halStatus;

    pdlI2cStatus = Cy_SCB_I2C_Init(DFU_I2C_HW, &DFU_I2C_config, &dfuI2cContext);
    if (CY_SCB_I2C_SUCCESS != pdlI2cStatus)
    {
        CY_DFU_LOG_ERR("Error during I2C PDL initialization. Status: %X", (unsigned int)pdlI2cStatus);
    }
    else
    {
        halStatus = mtb_hal_i2c_setup(&dfuI2cHalObj, &DFU_I2C_hal_config, &dfuI2cContext, NULL);
        if (CY_RSLT_SUCCESS != halStatus)
        {
            CY_DFU_LOG_ERR("Error during I2C HAL initialization. Status: %X", (unsigned int)halStatus);
        }
        else
        {
            cy_stc_sysint_t i2cIsrCfg =
            {
                .intrSrc = DFU_I2C_IRQ,
                .intrPriority = 3U
            };
            pdlSysIntStatus = Cy_SysInt_Init(&i2cIsrCfg, dfuI2cIsr);
            if (CY_SYSINT_SUCCESS != pdlSysIntStatus)
            {
                CY_DFU_LOG_ERR("Error during I2C Interrupt initialization. Status: %X", (unsigned int)pdlSysIntStatus);
            }
            else
            {
                NVIC_EnableIRQ((IRQn_Type)i2cIsrCfg.intrSrc);
                CY_DFU_LOG_INF("I2C transport is initialized");
            }
        }
    }
    cy_stc_dfu_transport_i2c_cfg_t i2cTransportCfg =
    {
        .i2c = &dfuI2cHalObj,
        .callback = dfuI2cTransportCallback,
    };
    Cy_DFU_TransportI2cConfig(&i2cTransportCfg);
}

/* [] END OF FILE */
