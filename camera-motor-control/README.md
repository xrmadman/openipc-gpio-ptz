# Camera Motor Control Package

Installs the compiled `gpio-motors` helper, its config file, and an optional
boot-time homing script onto an OpenIPC camera over SSH.

## Files

- `files/gpio-motors`: ARM binary installed to `/usr/bin/gpio-motors`.
- `files/gpio-motors.conf.example`: documented helper config installed to `/etc/gpio-motors.conf`.
- `files/S99gpio-motors-home`: optional boot script installed to `/etc/init.d/S99gpio-motors-home`.
- `files/majestic-gpio.yaml`: GPIO reference copied from the living room camera.
- `install-camera.sh`: SSH/SCP installer.

The installer also tries to enable OpenIPC/Majestic's GPIO motor UI flag with:

```sh
fw_setenv gpio_motors 1
```

Set `ENABLE_OPENIPC_GPIO_MOTORS=0` when running the installer if you do not want
that environment flag changed.

## Install

```sh
chmod +x install-camera.sh
./install-camera.sh CAMERA_IP root
```

For a camera with reversed motor wiring, edit `files/gpio-motors.conf.example`
before installing:

```conf
pan_motor_invert=1
tilt_motor_invert=1
```

To also apply the living-room Majestic GPIO settings:

```sh
APPLY_MAJESTIC_GPIO=1 ./install-camera.sh CAMERA_IP root
```

To start a homing calibration immediately after install:

```sh
RUN_HOME=1 ./install-camera.sh CAMERA_IP root
```

## Useful Camera Commands

Run these on the camera over SSH:

```sh
# Trigger a homing calibration. If a parking preset exists, the camera
# automatically returns to parking after calibration completes.
gpio-motors --home

# Show current saved position and whether the motors are moving.
gpio-motors --status

# Save the current position as the parking preset.
gpio-motors --preset-set parking

# Move back to the parking preset.
gpio-motors --preset-go parking 10
```

If the Majestic/OpenIPC pan-tilt controls are not visible, enable the firmware
flag and reboot:

```sh
fw_setenv gpio_motors 1
sync
reboot
```
