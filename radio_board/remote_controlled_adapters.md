"Mercury" mains adapter remote control signalling
=================================================

Timings measured on scope
-------------------------

**Zero**

           ___          __...
     ...__/   \________/
          :   :        :
          :---'        :
          : 170us      :
          '------------'
               770us


**One**

           ________     __...
     ...__/        \___/
          :        :   :
          :--------'   :
          :  550us     :
          '------------'
               770us


Recorded button codes
---------------------

Recorded using FourThreeThree with settings:

    FourThreeThree_rx_begin(rx_pin, 100ul, 300ul, 450ul, 600ul, 1000ul);

Button 1 (On then off)
[11053670,25]
[11053688,25]

Button 2 (On then off)
[11053958,25]
[11053976,25]

Button 3 (On then off)
[11054598,25]
[11054616,25]

Button 4 (On then off)
[11057670,25]
[11057688,25]

Button 5 (On then off)
[11069958,25]
[11069976,25]

