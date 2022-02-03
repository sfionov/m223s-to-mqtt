# m223s-to-mqtt

This is Redmond RMC-M223S to MQTT mapper.

It doesn't support remote program start because I don't need this. :)

Supported features:
1. Reporting state, program, temperature, time to `home/m223s/state` MQTT topic
2. Turning off by `PRESS` command on `home/m223s/off` MQTT topic

## How to build

```bash
apt install libsystemd-dev libmosquitto-dev libexpat-dev
mkdir -p build
cd build
cmake ..
make
./m223s
```

## How to pair

Start program, auth command will return `ff 00 aa` code. `00` means fail. 
Wait while your M223S starts beeping, long press '+' key until auth command returns successful `ff 01 aa` code.

## TODO:
- [ ] Customize address and auth key
- [ ] Customize mqtt settings
- [ ] Support remote start
