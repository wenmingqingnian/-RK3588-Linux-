#!/bin/bash
# Install persistent GPIO permissions for the QuarkPi-CA2 buzzer.
# Usage: sudo ./scripts/install_buzzer_gpio_permissions.sh

set -euo pipefail

TARGET_USER="quark"
GPIO_GROUP="gpio"
RULE_FILE="/etc/udev/rules.d/99-carteminal-gpio.rules"

if [ "$(id -u)" -ne 0 ]; then
    echo "error: run this script as root with sudo" >&2
    exit 1
fi

if ! id "$TARGET_USER" >/dev/null 2>&1; then
    echo "error: target user '$TARGET_USER' does not exist" >&2
    exit 1
fi

if ! getent group "$GPIO_GROUP" >/dev/null; then
    groupadd --system "$GPIO_GROUP"
fi

usermod -aG "$GPIO_GROUP" "$TARGET_USER"

printf '%s\n' \
    'SUBSYSTEM=="gpio", KERNEL=="gpiochip4", GROUP="gpio", MODE="0660"' \
    > "$RULE_FILE"

udevadm control --reload-rules
udevadm trigger --subsystem-match=gpio --action=change

echo "GPIO permissions installed for $TARGET_USER."
echo "Log out and back in, or reboot, before running CarTerminal."
