[Click here](../README.md) to view the README.

## Design and implementation

The design of this application is minimalistic to get started with DFU code examples on PSOC&trade; Edge MCU devices. All PSOC&trade; Edge E84 MCU applications have a dual-CPU three-project structure to develop code for the CM33 and CM55 cores. The CM33 core will have two separate application projects for the secure processing environment (SPE) and non-secure processing environment (NSPE). But this code example needs a fourth project for the EdgeProtect bootloader, which runs on CM33. A project folder consists of various subfolders, each denoting a specific aspect of the project. The four project folders are:

**Table 1. Application projects**

Project           | Description
-------           | -----------------------
*proj_cm33_s*     | Project for CM33 secure processing environment (SPE)
*proj_cm33_ns*    | Project for CM33 non-secure processing environment (NSPE)
*proj_cm55*       | CM55 project
*proj_bootloader* | EdgeProtect bootloader project for CM33 SPE. This project should be added by following the steps given in the [README](../README.md#add-the-edge-protect-bootloader)

<br>

In this code example, at device reset, the secure boot process starts from the ROM boot with the secure enclave (SE) as the root of trust (RoT). From the secure enclave, the boot flow is passed on to the extended boot, which launches the EdgeProtect Bootloader. 

After validating all the three application images, it launches the CM33 secure application. After all necessary secure configurations, the flow is passed on to the non-secure CM33 application. Resource initialization for this example is performed by this CM33 non-secure project. It configures the system clocks, pins, clock to peripheral connections, and other platform resources. It then enables the CM55 core using the `Cy_SysEnableCM55()` function and the CM55 core is subsequently put to DeepSleep mode.

In the CM33 non-secure application, the clocks and system resources are initialized by the BSP initialization function. The retarget-io middleware is configured to use the debug UART. The debug UART prints a message (as shown in [Terminal output on program startup](../images/boot.png)) on the terminal emulator; the onboard KitProg3 acts as the USB-UART bridge to create the virtual COM port.

   **Figure 1. Boot flow diagram**

   ![](images/bootflow.png)


1. CM33 non-secure application initializes the external flash, transport and DFU layers to inititate the firmware download process. 

2. The firmware download logic is a superloop-based design. The device receives the data sent from the DFU Host tool in chunks, processes it and program rows corresponding to 512 bytes of device memory at once

3. While downloading the firmware, the device also blinks an LED using a counter-based logic. After successful completion of firmware download, the project triggers a system reset to kick in the EdgeProtect bootloader to complete the firmware update

   **Figure 2. DFU process**

   ![](images/updateprocess.png)


### Switching the DFU Transport Interface

This example uses I2C as the default DFU transport interface and it lets you change the transport interface during runtime. The CM33 NS project registers a GPIO Interrupt that detects a GPIO button press. 

The GPIO ISR tracks the status of the previous transport interrupt switching request. If the previous switching request is completed, the ISR requests to switch the transport again, or else it ignores the interrupt.

The DFU application checks for pending transport switch when idle or if DFU communication has not started. It uses a circular state machine to switch the transport inteface. It also prints a message on the terminal when the DFU transport interface is switched.


   **Figure 3. Flow of DFU transport switching**

   ![](images/switch.png)


### DFU Transport interface configuration

The example supports I2C, USB-CDC, and USB-HID DFU interfaces to communicate with the DFU host or PC. 

See **Table 2** for the default configuration details of each interface. Ensure the configuration of the DFU Host tool matches this:

**Table 2. Serial transport configurations**

Interface | Configuration | Default Value | Description
:-------- | :------------ | :------------ | :----------
I2C  | Address <br> Data rate | 53 <br> 400 Kbps | 7-bit slave device address <br> DFU supports standard data rates from 50 Kbps to 1 Mbps
USB-CDC   | Baud rate     | 115200 bps    | Supports standard baud rates from 19200 bps to 115200 bps
USB-HID   |         -     |       -       | No additional configuration required

<br>