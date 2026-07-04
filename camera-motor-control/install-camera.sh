#!/bin/sh
set -eu

usage() {
  cat <<'USAGE'
Usage:
  ./install-camera.sh CAMERA_IP [USER]

Examples:
  ./install-camera.sh CAMERA_IP
  ./install-camera.sh CAMERA_IP root

Environment overrides:
  SSH_PORT=22
  GPIO_CONFIG=files/gpio-motors.conf.example
  ENABLE_OPENIPC_GPIO_MOTORS=1|0
  APPLY_MAJESTIC_GPIO=0|1
  RUN_HOME=0|1

Notes:
  - The script uses ssh and scp from this machine.
  - It creates timestamped backups on the camera before replacing files.
  - Edit files/gpio-motors.conf.example before installing if the target camera
    needs motor inversion or speed changes.
USAGE
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ] || [ $# -lt 1 ]; then
  usage
  exit 0
fi

CAMERA_IP="$1"
CAMERA_USER="${2:-root}"
SSH_PORT="${SSH_PORT:-22}"
GPIO_CONFIG="${GPIO_CONFIG:-files/gpio-motors.conf.example}"
ENABLE_OPENIPC_GPIO_MOTORS="${ENABLE_OPENIPC_GPIO_MOTORS:-1}"
APPLY_MAJESTIC_GPIO="${APPLY_MAJESTIC_GPIO:-0}"
RUN_HOME="${RUN_HOME:-0}"

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BINARY="$ROOT_DIR/files/gpio-motors"
CONFIG="$ROOT_DIR/$GPIO_CONFIG"
BOOT_HOME="$ROOT_DIR/files/S99gpio-motors-home"
MAJESTIC_GPIO="$ROOT_DIR/files/majestic-gpio.yaml"

if [ ! -f "$BINARY" ]; then
  echo "Missing binary: $BINARY" >&2
  exit 1
fi
if [ ! -f "$CONFIG" ]; then
  echo "Missing config: $CONFIG" >&2
  exit 1
fi

REMOTE="${CAMERA_USER}@${CAMERA_IP}"
REMOTE_TMP="/tmp/gpio-motors-install.$$"

echo "Uploading motor files to $REMOTE..."
ssh -p "$SSH_PORT" "$REMOTE" "mkdir -p '$REMOTE_TMP'"
scp -P "$SSH_PORT" "$BINARY" "$REMOTE:$REMOTE_TMP/gpio-motors"
scp -P "$SSH_PORT" "$CONFIG" "$REMOTE:$REMOTE_TMP/gpio-motors.conf"
scp -P "$SSH_PORT" "$BOOT_HOME" "$REMOTE:$REMOTE_TMP/S99gpio-motors-home"
scp -P "$SSH_PORT" "$MAJESTIC_GPIO" "$REMOTE:$REMOTE_TMP/majestic-gpio.yaml"

echo "Installing on camera..."
ssh -p "$SSH_PORT" "$REMOTE" "REMOTE_TMP='$REMOTE_TMP' ENABLE_OPENIPC_GPIO_MOTORS='$ENABLE_OPENIPC_GPIO_MOTORS' APPLY_MAJESTIC_GPIO='$APPLY_MAJESTIC_GPIO' RUN_HOME='$RUN_HOME' sh -s" <<'REMOTE_SCRIPT'
set -eu

stamp=$(date +%Y%m%d-%H%M%S)
backup_dir="/root/gpio-motors-backup-$stamp"
mkdir -p "$backup_dir"

[ -e /usr/bin/gpio-motors ] && cp /usr/bin/gpio-motors "$backup_dir/gpio-motors.before" || true
[ -e /etc/gpio-motors.conf ] && cp /etc/gpio-motors.conf "$backup_dir/gpio-motors.conf.before" || true
[ -e /etc/init.d/S99gpio-motors-home ] && cp /etc/init.d/S99gpio-motors-home "$backup_dir/S99gpio-motors-home.before" || true
[ -e /etc/majestic.yaml ] && cp /etc/majestic.yaml "$backup_dir/majestic.yaml.before" || true
[ -e /etc/majestic-gpio.yaml ] && cp /etc/majestic-gpio.yaml "$backup_dir/majestic-gpio.yaml.before" || true

cp "$REMOTE_TMP/gpio-motors" /usr/bin/gpio-motors
chmod 755 /usr/bin/gpio-motors
cp "$REMOTE_TMP/gpio-motors.conf" /etc/gpio-motors.conf

mkdir -p /etc/init.d
cp "$REMOTE_TMP/S99gpio-motors-home" /etc/init.d/S99gpio-motors-home
chmod 755 /etc/init.d/S99gpio-motors-home

cp "$REMOTE_TMP/majestic-gpio.yaml" /etc/majestic-gpio.yaml

if [ "$ENABLE_OPENIPC_GPIO_MOTORS" = "1" ]; then
  if command -v fw_setenv >/dev/null 2>&1; then
    fw_setenv gpio_motors 1 || true
  else
    echo "fw_setenv not found; skipping OpenIPC gpio_motors flag"
  fi
fi

set_yaml_key() {
  key="$1"
  value="$2"
  file="/etc/majestic.yaml"
  if [ ! -f "$file" ]; then
    return 0
  fi
  if grep -q "^[[:space:]]*$key:" "$file"; then
    sed -i "s|^[[:space:]]*$key:.*|  $key: $value|" "$file"
  fi
}

if [ "$APPLY_MAJESTIC_GPIO" = "1" ]; then
  set_yaml_key speakerPin 64
  set_yaml_key speakerPinInvert false
  set_yaml_key outputEnabled true
  set_yaml_key outputVolume 52
  set_yaml_key irCutSingleInvert true
  set_yaml_key lightMonitor true
  set_yaml_key lightSensorInvert true
  set_yaml_key monitorDelay 0
  set_yaml_key irCutPin1 2
  set_yaml_key irCutPin2 1
  set_yaml_key backlightPin 63
  set_yaml_key lightSensorPin 62
fi

rm -rf /tmp/gpio-motors.lock
/usr/bin/gpio-motors --status || true

if [ "$RUN_HOME" = "1" ]; then
  nohup sh -c '/usr/bin/gpio-motors --home; /usr/bin/gpio-motors --off' >/tmp/gpio-motors-home.log 2>&1 &
fi

rm -rf "$REMOTE_TMP"
echo "Backup saved on camera: $backup_dir"
echo "Config installed at: /etc/gpio-motors.conf"
echo "Binary installed at: /usr/bin/gpio-motors"
echo "Majestic GPIO reference installed at: /etc/majestic-gpio.yaml"
echo "OpenIPC gpio_motors flag requested: $ENABLE_OPENIPC_GPIO_MOTORS"
REMOTE_SCRIPT

echo "Done."
