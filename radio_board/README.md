Radio Interfaces Board
======================

A board which runs 433 MHz radio proxies for cheap wireless gadgets.

Once deployed with a 433 MHz receiver's output pin connected to D2, the
following Qth interfaces should appear:

* `sys/433mhz/rx_unknown_code`: An event produced whenever an unrecognised code
  is recieved. Each event is a `[code, code_length]` pair.
* `sys/433mhz/rx_codes` is an object `{"qth/path/here": [code, code_length],
  ...}`. Defines Qth events to create which are fired whenever particular codes
  are received.
