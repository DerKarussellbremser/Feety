# Feety
"A project to transmit data via radio from pressure sensors in the foot or in the sole
of a prosthesis to a vibration module on the body. This way, a person with a prosthesis
can know when their foot is properly placed on the ground. More details to follow."
Also thinkable for arm prothesis with glooves or support from an manufacturer.

3D Files for printing the casees, a detailed plan how to build all the hardware is also comming soon.

And a little hint for free: dont use cheap hardware. i got a lot of problems with the signals on
cheap ESP32, i had wrong signals on all
pins whenever more then one at a time was used.

Very first things on the to-do List:
-tidy up the code
-power saveing
-change some delays to mills code so that the main loop keeps running
-making BT connection stable / reconnecting and secure
-..