NodeMCU-based Home Automation Firmware
======================================

Firmware for the various NodeMCU boards around our house. A fairly random
collection of generally small programs built using PlatformIO.

* [`radio_board`](./radio_board/): Board which handles all 433 MHz radio
  related communications.

All of these can be built using Platform IO once `common/flags.txt` has been
populated with suitable values (see `common/flags.txt.example`).
