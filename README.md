# Virtual Sensor Hub

A small embedded-style monitoring system for Linux:

* a **kernel character-device driver** that simulates a temperature/humidity sensor,
* a **multithreaded C++ daemon** that reads it, logs data, raises alerts and serves remote clients,
* a **TCP command-line client**.

```
 timer ──► /dev/vsensor ──read()──► sensor_daemon ──TCP──► monitor_client
 (kernel)   char driver              (C++ threads)          (user)
                                      ├─ readings.csv
                                      └─ alerts.log
```

Languages: C (kernel module), C++17 (user space). Platform: Linux only.

## Repository layout

| Path | Contents |
|---|---|
| `driver/` | `vsensor.c` kernel module, `vsensor_ioctl.h` (shared with user space), `Makefile` |
| `daemon/` | `main.cpp` + header-only classes: `Sensor`/`DeviceSensor`, `ThreadSafeQueue<T>`, `Logger`, `Server`, `SharedState` |
| `client/` | `client.cpp` - `monitor_client` |
| `tests/` | `fake_sensor.cpp` (stand-in for the driver) and `run_tests.sh` (integration test) |
| `docs/` | `DESIGN.md` - architecture and UML diagrams |

## Requirements

```bash
sudo apt install build-essential linux-headers-$(uname -r)
```

> **Load the kernel module in a VM (or QEMU), not on your main machine.** A bug in any
> kernel module can crash the whole system.

## Build

```bash
make            # daemon, client, fake sensor  -> ./build/
make driver     # kernel module                -> driver/vsensor.ko
```

## Run with the real driver

```bash
sudo insmod driver/vsensor.ko              # optional: interval_ms=500
dmesg | tail -3                            # "vsensor: loaded: /dev/vsensor ..."
ls -l /dev/vsensor
sudo cat /sys/module/vsensor/parameters/interval_ms

sudo ./build/sensor_daemon                 # Ctrl-C to stop (needs access to /dev/vsensor)
# in a second terminal:
./build/monitor_client GET
./build/monitor_client STATUS
./build/monitor_client SET_THRESHOLD 30
./build/monitor_client SET_INTERVAL 200    # goes through ioctl() into the driver
./build/monitor_client WATCH               # live view, Ctrl-C to stop

tail -f logs/readings.csv logs/alerts.log
sudo rmmod vsensor
```

Run in the background with `sudo ./build/sensor_daemon --daemon` and stop it with `sudo pkill sensor_daemon`.
`./build/sensor_daemon --help` lists all options.

## Run without the kernel module (testing)

```bash
make test
```

`tests/fake_sensor` writes the same binary records the driver would into a named pipe, so the
whole user-space stack (threads, alert state machine, logging, TCP protocol, clean shutdown) is
tested with no kernel module. `SET_INTERVAL` is *expected* to fail there (a pipe has no ioctl).

## Network protocol

One text command per line, one reply line per command.

| Command | Reply |
|---|---|
| `GET` | `OK seq=12 temp=25.40 hum=51.20 ts=2026-10-04T10:15:02` |
| `STATUS` | `OK state=NORMAL readings=120 alerts=1 threshold=35.00 uptime=120s` |
| `SET_THRESHOLD <degC>` | `OK threshold=30.00` |
| `SET_INTERVAL <ms>` | `OK interval=200` (valid 10..60000) |
| `QUIT` | `OK bye` |

The server listens on `127.0.0.1` only by default (there is no authentication). Use `--any`
to listen on all interfaces.

## Concepts demonstrated

| Area | Where |
|---|---|
| Char device driver, `file_operations`, `cdev`, device class | `driver/vsensor.c` |
| Kernel timer, softirq context rules, spinlock, wait queue | `driver/vsensor.c` |
| `copy_to_user` / `copy_from_user`, `ioctl`, module parameter (sysfs) | `driver/vsensor.c` |
| OOP: abstract class, polymorphism, RAII, deleted copy | `daemon/sensor.hpp` |
| Templates, mutex + condition variable | `daemon/thread_safe_queue.hpp` |
| `std::thread`, atomics, producer/consumer | `daemon/main.cpp`, `state.hpp` |
| Sockets, `poll()`, thread per client | `daemon/server.hpp`, `client/client.cpp` |
| Signals (`sigwait`, signal masks), daemonization (double fork), file descriptors | `daemon/main.cpp` |
| Makefile, bash testing, Git | `Makefile`, `tests/run_tests.sh` |
| Requirements, UML, state machine | `docs/DESIGN.md` |

## Limitations / future work

* The sensor is simulated. A real one would use GPIO / I2C through a platform driver.
* No authentication or encryption on the TCP port (TLS would be the next step).
* One thread per client with no limit; a thread pool would be better.
* Single device instance; multiple sensors would need minor numbers per device.
* Driver built and run on Linux 7.0 (Ubuntu in VirtualBox); also compiles against 6.8 headers.
