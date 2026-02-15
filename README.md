# Zephyr Linux Setup

## Prerequisites
- Docker, VSCode, Dev Containers extension
- USB device connected

## Setup

### 1. Detect serial port
```bash
ls /dev/ttyUSB* /dev/ttyACM*
```

### 2. Configure device
Edit `.devcontainer/devcontainer.json`:
```json
"--device=/dev/ttyUSB0"
```

### 3. Configure version
Edit `manifest/west.yml`:
```yaml
revision: v3.7.0
```

### 4. Initialize Zephyr
From DevContainer (`/workdir`):
```bash
cd manifest
west init -l .
cd ..
west update
```

### 5. ESP32 only
```bash
west blobs fetch hal_espressif
```

## Resources
- Boards: https://docs.zephyrproject.org/latest/boards/index.html