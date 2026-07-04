# OpenIPC GPIO PTZ

Camera-side GPIO pan/tilt motor control for OpenIPC cameras.

This repo contains a compiled ARM `gpio-motors` helper, its C source, documented motor config examples, a boot-time homing script, Majestic GPIO reference settings, and an SSH installer.

## Contents

```text
camera-motor-control/
  install-camera.sh
  CHECKSUMS.txt
  files/
    gpio-motors
    gpio-motors.c
    gpio-motors.conf.example
    gpio-motors.conf.driveway-invert
    majestic-gpio.yaml
    S99gpio-motors-home
```

## Install

From `camera-motor-control`:

```sh
chmod +x install-camera.sh
./install-camera.sh CAMERA_IP root
```

For a camera with reversed motor wiring:

```sh
GPIO_CONFIG=files/gpio-motors.conf.driveway-invert ./install-camera.sh CAMERA_IP root
```

To apply the included Majestic GPIO values into `/etc/majestic.yaml`:

```sh
APPLY_MAJESTIC_GPIO=1 ./install-camera.sh CAMERA_IP root
```

Manual commands on the camera:

```sh
gpio-motors --home
gpio-motors --status
gpio-motors --preset-set parking
gpio-motors --preset-go parking 10
```

If Majestic/OpenIPC does not show pan/tilt controls:

```sh
fw_setenv gpio_motors 1
sync
reboot
```

## License

MIT License. See `LICENSE`.
