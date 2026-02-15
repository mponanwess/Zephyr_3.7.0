# Zephyr Build & Flash

## Verify Board
```bash
west boards | grep <board_name>
```

## Configure

### Board Files

Rename:

```
boards/esp32_devkitc.overlay
```

to:

```
boards/<board_name>.overlay
```

### CMakeLists.txt

Update according to the selected board:

```cmake
set(BOARD <board_path>)
set(DTC_OVERLAY_FILE boards/<board_name>.overlay)
```

### Zephyr v3.7.0

Required in `prj.conf`:

```conf
CONFIG_NEWLIB_LIBC=y
```

## Build

```bash
west build -p always -b <board_name> ./apps/helloworld
```

## Flash

```bash
west flash
```

## Monitor (ESP32 only)

```bash
west espressif monitor -p /dev/ttyUSB0
```

**Note:**
All commands must be executed inside the DevContainer from `/workdir`.