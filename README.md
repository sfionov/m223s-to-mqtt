# m223s-to-mqtt

This is Redmond RMC-M223S to MQTT mapper.

It doesn't support remote program start because I don't need this. :)

Supported features:
1. Reporting state, program, temperature, time to `home/m223s/state` MQTT topic
2. Turning off by `PRESS` command on `home/m223s/off` MQTT topic
