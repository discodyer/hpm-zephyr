.. SPDX-License-Identifier: Apache-2.0

HPM hardware bring-up
####################

Prints a unique Zephyr startup message, echoes console input and prints an
uptime/cycle-counter heartbeat every second. Also toggles ``led0`` if defined.
Does not enable the flash driver or exercise external peripheral buses.

Build from the workspace root:

.. code-block:: sh

   export ZEPHYR_EXTRA_MODULES="$PWD/hpm-zephyr"
   export HPM_SDK_BASE="$PWD/hpm_sdk"
   west build -b hpm5300evk/hpm5361 -d build/hpm5300evk-bringup-xip \
     hpm-zephyr/samples/bringup

Use HPMicro's OpenOCD fork, which provides the ``hpm_xpi`` flash driver:

.. code-block:: sh

   west flash -d build/hpm5300evk-bringup-xip \
     --openocd /usr/local/bin/openocd --verify
   west debug -d build/hpm5300evk-bringup-xip \
     --openocd /usr/local/bin/openocd --no-load

Flash first, then use ``--no-load`` to debug the programmed XIP image with its
matching ELF. The local wrapper resets and halts on GDB attach so the XPI
probe algorithm can execute before application PMP initialization. For attach
without reset, disable the GDB flash memory map/programming and override the
attach event to ``halt``; see the workspace debugging guide.

To restart an already programmed EVK when hart examination fails:

.. code-block:: sh

   /usr/local/bin/openocd -s /usr/local/share/openocd/scripts \
     -s hpm_sdk/boards/openocd \
     -f hpm-zephyr/boards/hpmicro/hpm5300evk/support/reset.cfg \
     -c 'adapter speed 500; init; hpm5300evk_reset; shutdown'

This pulses the board RESETN line without requiring halt or changing flash.
It uses local FT2232 reset wiring overrides and keeps SDK files unchanged.

Console: 115200 baud, 8N1, no flow control. The container exposes the FT2232
UART as ``/dev/ttyHPM``. The EVK uses FTDI JTAG, not SEGGER J-Link.

.. code-block:: sh

   python3 -m serial.tools.miniterm --raw --dtr 0 --rts 0 /dev/ttyHPM 115200

Exit miniterm with Ctrl+]. Hardware/software flow control and local echo are
off by default. Keep only one reader open on the UART; it can run alongside
OpenOCD on the other FT2232 interface.

On RevD with both BOOT switches OFF, software reset now starts Zephyr XIP;
sustained heartbeat and UART echo pass. With the DM PMP region installed,
normal examination/halt, ``west flash --verify``, GDB hardware breakpoints and
single-step also pass. LED electrical behavior and full driver tests remain
pending.

Use short or paced console input: this sample polls RX every 10 ms, and a
burst longer than the HPM5361's 16-byte FIFO can lose data. IRQ/DMA receive
throughput needs a separate test.

Acceptance requires the ``HPM Zephyr bring-up`` message, advancing uptime
at measured one-second intervals, UART echo, LED observation and a working
GDB breakpoint. An image transfer alone does not satisfy acceptance.

An experimental RAM image can be built in a separate directory by passing
``-- -DCONFIG_XIP=n`` to ``west build``. Its current link address is DLM
``0x00080000``; executable mapping and debugger restart state remain unverified.
