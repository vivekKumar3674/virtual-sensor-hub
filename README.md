# Virtual Sensor Hub

A practice project where I tried to connect the pieces of a small Linux monitoring setup: a kernel driver that pretends to be a temperature/humidity sensor, a C++ background program that reads it, and a small client to talk to that program over the network.

I wanted to see how data actually travels from the kernel up to a normal program, so everything here is deliberately small.

```
 kernel timer -> /dev/vsensor -> sensor_daemon -> TCP -> monitor_client
 (driver, C)                     (C++ threads)           (C++)
                                  |- logs/readings.csv
                                  '- logs/alerts.log
```

## What each part does

- **driver/** - a character device driver. A kernel timer makes a new fake reading every second (it wanders between 15-45 C and 20-90 % humidity). A program that reads `/dev/vsensor` gets one reading and waits if there is nothing new yet. The sampling interval can be changed with an `ioctl` or through `/sys/module/vsensor/parameters/interval_ms`.
- **daemon/** - the C++ program. One thread reads the device, one writes every reading to a CSV file, one watches for a too-high temperature, and one serves TCP clients. Ctrl+C shuts it all down cleanly.
- **client/** - a tiny command-line tool: you type `GET` or `STATUS` and it prints the daemon's answer.
- **tests/** - a fake sensor (feeds the daemon through a named pipe) and a shell script that tests the daemon without needing the kernel module.
- **docs/DESIGN.md** - my design notes and diagrams.

## Setup

You need a Linux machine. For the driver, use a virtual machine, because a mistake in a kernel module can crash the whole system. I used Ubuntu in VirtualBox.

```bash
sudo apt install build-essential linux-headers-$(uname -r) git
git clone https://github.com/vivekKumar3674/virtual-sensor-hub.git
cd virtual-sensor-hub
make            # daemon, client, fake sensor  -> build/
make driver     # kernel module                -> driver/vsensor.ko
```

## Running it with the real driver

```bash
sudo insmod driver/vsensor.ko
sudo dmesg | tail -3          # should say "vsensor: loaded: /dev/vsensor ..."

# terminal 1 - leave this one running
sudo ./build/sensor_daemon --threshold 26

# terminal 2
./build/monitor_client GET
./build/monitor_client STATUS
./build/monitor_client SET_THRESHOLD 30
./build/monitor_client SET_INTERVAL 200
./build/monitor_client WATCH      # live view, Ctrl+C to stop
```

Logs end up in `logs/`. When you are done, press Ctrl+C in terminal 1, then:

```bash
sudo rmmod vsensor
```

## Trying it without the kernel module

If you only want to check the C++ side (this also works in WSL):

```bash
make test
```

It starts the fake sensor and the daemon, sends some commands and checks the answers and log files. `SET_INTERVAL` is *supposed* to fail there, since a pipe has no `ioctl`.

## Commands the daemon understands

| Command | Example reply |
|---|---|
| `GET` | `OK seq=475 temp=20.03 hum=32.04 ts=2026-10-04T07:14:58` |
| `STATUS` | `OK state=NORMAL readings=27 alerts=0 threshold=22.00 uptime=26s` |
| `SET_THRESHOLD <C>` | `OK threshold=30.00` |
| `SET_INTERVAL <ms>` | `OK interval=200` (10 to 60000) |
| `QUIT` | `OK bye` |

The alert switches to ALERT when the temperature goes above the threshold and back to NORMAL only once it drops 1 C below it, so it doesn't flip back and forth around the limit. The server only listens on localhost unless you pass `--any`. There is no login, so don't expose it.

## Things that went wrong along the way

- On Ubuntu with Linux 7.0 headers the driver didn't compile at first: `no_llseek` no longer exists there. The device already refuses seeking via `nonseekable_open()`, so I just removed that line.
- `dmesg` said "Operation not permitted" until I used `sudo`.
- The client said "cannot connect" because I had stopped the daemon with Ctrl+C. It has to stay running in its own terminal.
- WSL can't load kernel modules, so I tested the C++ part there and the driver in the VM.

## Limits

- The sensor is fake. A real one would need a platform driver and GPIO or I2C.
- No encryption or authentication on the TCP port.
- One thread per client, with no limit.
- I've only run it on one machine (Ubuntu with Linux 7.0 in VirtualBox). The driver also compiled against 6.8 headers.

## Credits

Practice project. I built and tested it myself on a real kernel, with an AI assistant helping with the initial code and design.
