# Windows Embedded Compact (CE) on Raspberry Pi

Original Creator:

Doug Boling<br>
Boling Consulting Inc.<br>
www.bolingconsulting.com

License: **Mozilla Public License Version 2.0**

Codeplex source: https://web.archive.org/web/20140706023858/http://ceonpi.codeplex.com/documentation

## Overview

CEonPI is a board support package (BSP) that enables Windows Embedded Compact 7 to run on the Raspberry Pi.  This website has all the source code of this BSP.  The current code is incomplete but provides the following features:

1. A bootloader SLOADER1 that boots and allows testing of code on the hardware. Output is provided through UART1 as well as through video.

2. A bootloader SBOOT that boots and connects with Platform Builder on a PC through UART1 and downloads and launches code provided by Platform Builder.

3. OAL code that can be used to boot the Windows Embedded Compact 7 kernel on the Pi.

4. A KITL (Kernel Independent Transport Layer) driver to enable remote debugging of code running on the Pi by Platform Builder.

5. A basic (unaccelerated) video driver for Windows Embedded Compact.

I expect to add additional drivers to the BSP as they are developed. I welcome contributions from interested parties.

## Rights

The BSP source code is free to use on any Windows CE or Windows Embedded Compact image.

## Board versions

The BSP works with both the Raspberry Pi "A" and "B" versions. At the present time I haven’t modified the BSP to detect and use the maximum amount of RAM on each system. 256 Meg is a huge amount for CE so I haven’t bothered.

## Instructions

These instructions assume you have a basic knowledge of building Windows Embedded Compact images.

### Downloading and installing Windows Embedded Compact

To use this BSP, you will need to have an installed copy of Platform Builder 7. You can download an evaluation copy of Platform Builder 7 from the Microsoft Web Site at: http://www.microsoft.com/windowsembedded/en-us/downloads.aspx

Scroll down to "Windows Embedded Compact 7.  Expand that link and then click on "Register to get the product key and download the Windows Embedded Compact 7 evaluation". You’ll need to provide some basic information but after that you’ll be able to download Platform Builder 7 and get a registration key.

Platform Builder 7 installs into Visual Studio 2008.  If you don’t have a copy of VS’08, you can download an evaluation copy at the same Windows Embedded website.

Install VS ’08, VS’08 SP1 and then Platform Builder 7.

### Downloading the BSP and project files

Select the Download page of the Codeplex project.  Download the BSP in the .zip file `RASPI2BSP.zip`.  Unzip the file into the `\wince700\platform directory`. The BSP is now installed under the folder name \wince700\platform\raspi2

You can also download a preconfigured project RaspiCE in the file `RaspiCEProject.zip`.  Unzip that file into `\wince700\OSDesigns`.  The Project is now installed in `\wince700\OSDesigns\RaspiCE`.

If you choose, you can create your own project instead.

Start Visual Studio 2008. Open the RaspiCE project or create your own.  If you create your own project, select the RASPI2 BSP when creating the project.

### Building the image

Build the project by selecting the Build|Build Solution menu item.  When the build is finished, the result will be a file kernel.img in the Release Directory.  Open a command line window to the release directory by selecting the Build | "Open release directory in build window" menu item.

Platform Builder can create 3 different versions of your build:

1. Retail build – No debug messages, kernel debugger not enabled. KITL is enabled.

2. Checked Build – Debug message enabled, debugger enabled. Code still optimized for size

3. Debug Build – Debug message enabled, debugger enabled. Code not optimized allowing for breakpoints to be set on discrete lines of code.

Of course, it’s possible to enable the debugger in Retail builds and to disable the debugger in Checked and Debug builds. KITL can be disabled in any of the builds as well. This can be configured in the Project | Properties dialog.

You will need to perform a complete build on each of the 3 configurations you want to use. The easy way to do this is to go to the Build | Batch Build menu item and select all 3 to be built sequentially. This takes some time but frees you to do other tasks instead of watching for each individual build to complete.

The result of a build will be a kernel.img file in the Release Directory for each of the build types. This is simply a renamed nk.nb0 file. Take care that your image size does not exceed the size of NKLEN in `wince700\platform\raspi2\files\config.bib`.  This setting defines the length of a "ROM" that the romimage tool uses to layout the .nb0 file. If the image is too big, part of the image will be placed in nk.nb1 and the image will fail to boot. I have put a check in the build to detect this condition. If the image is too large the MAKEIMG tool will indicate an error. Look at the end of the makeimg.log file in the release directory for the explanation of the error.
Creating a bootable SD image

Windows Embedded CE doesn’t need the additional Linux partition that is standard on a bootable SD card.  Instead, a simple FAT formatted SD card will do. 

The card does need a minimal number of files used by the Pi during the boot process before the kernel is loaded.  These files can be found on the Raspberry Pi organization’s website: http://www.raspberrypi.org/. The minimal files necessary for the Pi to boot are:

| File | Description |
|------|----|
| `bootcode.bin`                 |   The initial boot code used by the Raspberry Pi before the kernel is loaded |
| `start.elf`       |                                         Configuration file needed by the boot sequence |
| `kernel.img`         |                 the Windows Embedded Compact kernel image |


The `bootcode.bin` and `start.elf` files can be found on the Raspberry Pi website.
 

A safer way to create the bootable image is to follow the directions on the Raspberry Pi website to create one of the many Linux images for the Pi.  I suggest you try to boot that version of Linux on your system to confirm that the SD card is formatted correctly.

Once you have a bootable SD card, simply copy the kernel.img file from the release directory to the SD card. This replaces the Linux image with the Windows Embedded Compact boot image. Leave the other files intact.

### Video Resolution

The image as built will configure the display as 1920x1080 with 16 bit color.  To change the video resolution, edit the file `\wince700\platform\raspi2\src\inc\image_cfg.h`.  In the file change the lines

    #define VID_WIDTH                           1920
    #define VID_HEIGHT                          1080

To the resolution desired.  I have only tested the video at resolutions 640x480, 1280x1024 and 1920x1080 but I wouldn’t expect problems at other resolutions as long as the Raspberry Pi can handle it.  Pixel depth (bits per pixel) defaults to 16 bpp although 24 bpp is also supported.  24bpp is slower given the software only blits that are currently implemented.

After changing any of the settings in `image_cfg.h`, rebuild the entire BSP. Do this by selecting the menu item `Build` | `Advanced Build Commands` | `Build Current BSP and Subprojects`. You don’t need to build the entire solution if changes are being made only to the BSP.

Also note that the BSP has only been tested with video through the HDMI connector.  If an HDMI monitor is not connected when the Raspberry Pi boots the display cannot be seen when it is connected later.  This can be avoided by adding the line:

    hdmi_force_hotplug=1

to the config.txt file in the root directory of the SD card.

KITL Support

The BSP includes support for KITL over serial using the Raspberry Pi’s UART1 accessed through the GPIO lines on the expansion connector P1.  The GPIO pins are 3.3 volt and cannot be directly connected to a standard RS-232 UART without damaging the Pi. Instead use a FTDI board that provides a 3.3 volt UART on one side and a USB connection on the other. 

I ordered my FTDI board from SparkFun. The board can be found at the link:

https://www.sparkfun.com/products/9873

#### FTDI Serial to USB pin out and setup

| FTDI board | Pin   | RasPi P1 |
|------------|-------|----------|
| 1          | GND   | 6        |
| 2          | CTS   | 11       |
| 3          | POWER | N/C      |
| 4          | TDX   | 10       |
| 5          | RXI   | 8        |
| 6          | DTR   | N/C      |

All images are built by default with KITL enabled. To build without kitl disable through the Project | Properties dialog box or if at the command line set the environment variable `IMGNOKITL=1`.

By default, any image that has KITL enabled will output some initial diagnostic information to the video screen on boot.  To prevent this, disable KITL (`IMGNOKITL=1`) and rebuild the BSP.

### Disabling KITL

By default all three builds have KITL enabled because the BSP doesn’t yet support USB and therefore neither keyboards or mice can be used. However, this also means that all the images require the FTDI serial connection.

To create a bootable image that doesn’t require the FTDI serial, disable KITL and rebuild the BSP.  The easiest way to do this is to disable KITL by selecting the Project | Properties menu item. Then select "Configuration Properties" and the "Build Options"  in the left pane of the dialog box. About halfway down in the right pane click on the "Enable KITL" line.  A dropdown box will appear and "No `IMGNOKITL=1` can be selected. Click OK and rebuild the BSP with the menu item Build | Advanced Build Commands | Build Current BSP and Subprojects".

The image created should boot fairly quickly and display the desktop on the screen.

### Setting up Platform Builder for Serial KITL

Select the Target | Connectivity Options menu in Platform Builder. In the left panel of the dialog box, select "Add Device".  Enter whatever name you choose such as "Serial KITL". The "Associated  OS Design/SDK should be set to "Windows CE".  Click "Add" to return to the main dialog page.

Under "Kernel Download" select None. We won’t be downloading the image as it will already be on the SD card. Note, it is possible to download a kernel image but the images are large enough and serial slow enough that it is much more practical to simply copy over kernel.img to the device on the SD card.

Under "Kernel Transport" select "Serial". Click on the Settings button next to the Kernel Transport dropdown.  Enter the COM port settings for the port that was added when you connected the FDTI board to your PC via USB. To determine the port, you may need to look in the Windows Device Manager. Note this is the serial port on the Windows side, not UART 1 on the Raspberry Pi. Set the speed to 115200, parity to None, Data bits to 8, Stop bits to 1 and the Flow Control to Hardware. Click OK to close and confirm these settings.  They should now show up on the main dialog underneath the Kernel Transport dropdown. Click on the Apply button and then the Close button to lock in these changes.

In Platform Builder, make sure the "device" toolbar is displayed. It will have a dropdown box that indicates the KITL target device. Make sure it is set to the name you gave the target device when configuring KITL.

### Booting

Simply put in the modified SD card image in the Pi and apply power. The image should boot fairly quickly.  If KITL is enabled, the video will display a few lines of console output indicating that the device is waiting for Platform Builder to connect.

In Platform Builder, select the Target | Attach Device menu item. If KITL successfully connects, you should see additional text on the Pi video and within a few seconds, debug text should be seen in Platform Builder’s debug output window.  If the debugger isn’t enabled, the OS should boot to the desktop within a few seconds. If the debug is selected this will take 20-30 seconds.

Normal operation is indicated by the Raspberry Pi’s OK LED flashing on and off approximately every second. This indicates that the scheduler is running and interrupts are being process appropriately.

### Keyboard and mouse support

Unfortunately, the Pi uses USB to interface with keyboards and mice. This is a PITA because writing the Windows Compact USB Host controller driver is a total PITA and I’ve not yet completed this task. Sorry, I’m working on it…