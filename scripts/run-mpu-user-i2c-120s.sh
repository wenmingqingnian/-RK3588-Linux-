#!/bin/sh
set -eu

EXPECTED_SHA="153ed69373967608654719074be67870f9af1706650f171a716675c4392ad9d0"

INIT="/etc/init.d/S99zz-carterminal"
APP="/usr/bin/CarTerminal"
CONFIG="/usr/share/carterminal/config/vehicle_devices.conf"
LAUNCHER="/usr/bin/carterminal-launch"
LOG="/userdata/carterminal/logs/carterminal.log"

CLIENT_ID="3-0068"
CLIENT="/sys/bus/i2c/devices/$CLIENT_ID"
DRIVER="/sys/bus/i2c/drivers/inv-mpu6050-i2c"

STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="/userdata/carterminal/ab/user-i2c-120s-$STAMP"
BACKUP_CONFIG="$RUN_DIR/vehicle_devices.conf.before"
MARKER="===USER_I2C_120S_$STAMP==="

RESTORE_DONE=0

extract_test_log()
{
    OUTPUT="$1"

    if grep -Fqx "$MARKER" "$LOG" 2>/dev/null; then
        awk -v marker="$MARKER" '
            $0 == marker {
                found = 1
                next
            }
            found {
                print
            }
        ' "$LOG" > "$OUTPUT"
    else
        cp -a "$LOG" "$OUTPUT"
    fi
}

restore_iio()
{
    echo
    echo "===== Restore IIO backend ====="

    "$INIT" stop >/dev/null 2>&1 || true
    sleep 1

    if [ -f "$BACKUP_CONFIG" ]; then
        cp -a "$BACKUP_CONFIG" "$CONFIG"
    fi

    if [ -d "$CLIENT" ] &&
       [ ! -L "$CLIENT/driver" ] &&
       [ -e "$DRIVER/bind" ]; then
        echo "$CLIENT_ID" > "$DRIVER/bind" 2>/dev/null || true
    fi

    sleep 2
    "$INIT" start >/dev/null 2>&1 || true
    sleep 12

    {
        echo "backend=$(grep '^mpu6050_backend=' "$CONFIG" || true)"
        echo "driver=$(readlink -f "$CLIENT/driver" 2>/dev/null || true)"

        PID="$(pidof CarTerminal | awk '{print $1}')"
        echo "pid=$PID"

        if [ -n "$PID" ]; then
            echo "running_sha=$(sha256sum "/proc/$PID/exe" | awk '{print $1}')"

            for FD in "/proc/$PID"/fd/*; do
                TARGET="$(readlink "$FD" 2>/dev/null || true)"
                case "$TARGET" in
                    /dev/iio:*|/dev/i2c-*)
                        echo "fd=${FD##*/} target=$TARGET"
                        ;;
                esac
            done
        fi

        for IIO in /sys/bus/iio/devices/iio:device*; do
            [ -f "$IIO/name" ] || continue
            [ "$(cat "$IIO/name")" = "mpu6500" ] || continue

            echo "iio=$IIO"
            echo "buffer_enable=$(cat "$IIO/buffer/enable" 2>/dev/null)"
            echo "trigger=$(cat "$IIO/trigger/current_trigger" 2>/dev/null)"
            echo "frequency=$(cat "$IIO/sampling_frequency" 2>/dev/null)"
        done

        grep 'AB_STATS backend=iio_buffer' "$LOG" |
        tail -n 1 || true
    } > "$RUN_DIR/restore-state.txt"

    cat "$RUN_DIR/restore-state.txt"
    RESTORE_DONE=1
}

cleanup()
{
    if [ "$RESTORE_DONE" -ne 1 ]; then
        restore_iio
    fi
}

trap cleanup EXIT INT TERM

mkdir -p "$RUN_DIR"

echo "===== Preconditions ====="

APP_SHA="$(sha256sum "$APP" | awk '{print $1}')"

echo "APP_SHA=$APP_SHA"
echo "RUN_DIR=$RUN_DIR"

[ "$APP_SHA" = "$EXPECTED_SHA" ] || {
    echo "ERROR: unexpected CarTerminal binary"
    exit 1
}

grep -q '^mpu6050_backend=iio_buffer$' "$CONFIG" || {
    echo "ERROR: initial backend is not iio_buffer"
    exit 1
}

grep -q '^export CARTERMINAL_MPU_AB_STATS=1$' "$LAUNCHER" || {
    echo "ERROR: A/B statistics are disabled"
    exit 1
}

[ -L "$CLIENT/driver" ] || {
    echo "ERROR: MPU kernel driver is not bound"
    exit 1
}

cp -a "$CONFIG" "$BACKUP_CONFIG"
cp -a "$LOG" "$RUN_DIR/carterminal-before.log" 2>/dev/null || true

echo
echo "===== Stop IIO application ====="

"$INIT" stop
sleep 2

if pidof CarTerminal >/dev/null 2>&1; then
    echo "ERROR: CarTerminal remains running"
    exit 1
fi

for IIO in /sys/bus/iio/devices/iio:device*; do
    [ -f "$IIO/name" ] || continue
    [ "$(cat "$IIO/name")" = "mpu6500" ] || continue

    ENABLE="$(cat "$IIO/buffer/enable" 2>/dev/null || echo unknown)"

    echo "IIO=$IIO"
    echo "BUFFER_ENABLE_AFTER_STOP=$ENABLE"

    [ "$ENABLE" = "0" ] || {
        echo "ERROR: IIO buffer remains enabled"
        exit 1
    }
done

echo
echo "===== Unbind kernel driver ====="

echo "$CLIENT_ID" > "$DRIVER/unbind"
sleep 1

[ ! -L "$CLIENT/driver" ] || {
    echo "ERROR: kernel driver remains bound"
    exit 1
}

echo "PASS: kernel driver unbound"

echo
echo "===== Select user_i2c backend ====="

sed -i \
    's/^mpu6050_backend=.*/mpu6050_backend=user_i2c/' \
    "$CONFIG"

grep '^mpu6050_backend=' "$CONFIG"

printf '%s\n' "$MARKER" >> "$LOG"

echo
echo "===== Start user_i2c application ====="

"$INIT" start
sleep 1

PID="$(pidof CarTerminal | awk '{print $1}')"

echo "PID=$PID"

[ -n "$PID" ] || {
    echo "ERROR: CarTerminal failed to start"
    exit 1
}

echo
echo "===== Wait for warm-up statistics ====="

WAIT_COUNT=0
START_STATS=""

while [ "$WAIT_COUNT" -lt 25 ]; do
    extract_test_log "$RUN_DIR/carterminal-user-i2c-current.log"

    START_STATS="$(
        grep 'AB_STATS backend=user_i2c' \
            "$RUN_DIR/carterminal-user-i2c-current.log" |
        tail -n 1 || true
    )"

    [ -n "$START_STATS" ] && break

    sleep 1
    WAIT_COUNT=$((WAIT_COUNT + 1))
done

[ -n "$START_STATS" ] || {
    echo "ERROR: no user_i2c AB_STATS after warm-up"
    grep -i 'mpu' "$RUN_DIR/carterminal-user-i2c-current.log" |
    tail -n 100 || true
    exit 1
}

printf '%s\n' "$START_STATS" > "$RUN_DIR/ab-stats-start.txt"
echo "START_STATS=$START_STATS"

OPEN_OK="$(
    printf '%s\n' "$START_STATS" |
    sed -n 's/.* open_ok=\([0-9][0-9]*\).*/\1/p'
)"

OPEN_ERR="$(
    printf '%s\n' "$START_STATS" |
    sed -n 's/.* open_err=\([0-9][0-9]*\).*/\1/p'
)"

SAMPLES="$(
    printf '%s\n' "$START_STATS" |
    sed -n 's/.* samples=\([0-9][0-9]*\).*/\1/p'
)"

READ_ERR="$(
    printf '%s\n' "$START_STATS" |
    sed -n 's/.* read_err=\([0-9][0-9]*\).*/\1/p'
)"

[ "${OPEN_OK:-0}" -ge 1 ] || {
    echo "ERROR: user_i2c source did not open"
    exit 1
}

[ "${OPEN_ERR:-999}" -eq 0 ] || {
    echo "ERROR: user_i2c open error"
    exit 1
}

[ "${SAMPLES:-0}" -gt 0 ] || {
    echo "ERROR: no user_i2c samples"
    exit 1
}

[ "${READ_ERR:-999}" -eq 0 ] || {
    echo "ERROR: user_i2c read errors detected"
    exit 1
}

FD_FOUND=0

for FD in "/proc/$PID"/fd/*; do
    TARGET="$(readlink "$FD" 2>/dev/null || true)"

    case "$TARGET" in
        /dev/i2c-3)
            echo "FD=${FD##*/} TARGET=$TARGET"
            FD_FOUND=1
            ;;
        /dev/iio:*)
            echo "ERROR: unexpected IIO FD: $TARGET"
            exit 1
            ;;
    esac
done

[ "$FD_FOUND" -eq 1 ] || {
    echo "ERROR: /dev/i2c-3 FD not found"
    exit 1
}

echo
echo "===== Capture start context ====="

{
    echo "date_utc=$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "pid=$PID"
    echo "running_sha=$APP_SHA"
    echo "backend=user_i2c"
    echo "client_registered=$([ -d "$CLIENT" ] && echo yes || echo no)"
    echo "driver_bound=$([ -L "$CLIENT/driver" ] && echo yes || echo no)"
    echo "clk_tck=$(getconf CLK_TCK)"
    echo "page_size=$(getconf PAGESIZE)"
    echo "uptime=$(cat /proc/uptime)"
    echo "loadavg=$(cat /proc/loadavg)"
} > "$RUN_DIR/start-context.txt"

grep -E 'inv_mpu|inv-mpu' /proc/interrupts \
    > "$RUN_DIR/interrupts-start.txt" || true

echo "sample,epoch,uptime_s,cpu_ticks,rss_pages,threads" \
    > "$RUN_DIR/process-samples.csv"

echo
echo "===== Begin 120-second observation ====="

SAMPLE_INDEX=0

while [ "$SAMPLE_INDEX" -lt 120 ]; do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "ERROR: CarTerminal exited during test"
        exit 1
    fi

    EPOCH="$(date +%s)"
    UPTIME_S="$(awk '{print $1}' /proc/uptime)"

    awk \
        -v sample="$SAMPLE_INDEX" \
        -v epoch="$EPOCH" \
        -v uptime="$UPTIME_S" \
        '{
            print sample "," epoch "," uptime "," \
                  ($14 + $15) "," $24 "," $20
        }' \
        "/proc/$PID/stat" \
        >> "$RUN_DIR/process-samples.csv"

    sleep 1
    SAMPLE_INDEX=$((SAMPLE_INDEX + 1))
done

echo "Completed samples=$SAMPLE_INDEX"

extract_test_log "$RUN_DIR/carterminal-user-i2c.log"

END_STATS="$(
    grep 'AB_STATS backend=user_i2c' \
        "$RUN_DIR/carterminal-user-i2c.log" |
    tail -n 1
)"

printf '%s\n' "$END_STATS" > "$RUN_DIR/ab-stats-end.txt"

echo
echo "END_STATS=$END_STATS"

{
    echo "date_utc=$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "pid=$PID"
    echo "backend=user_i2c"
    echo "client_registered=$([ -d "$CLIENT" ] && echo yes || echo no)"
    echo "driver_bound=$([ -L "$CLIENT/driver" ] && echo yes || echo no)"
    echo "uptime=$(cat /proc/uptime)"
    echo "loadavg=$(cat /proc/loadavg)"
} > "$RUN_DIR/end-context.txt"

grep -E 'inv_mpu|inv-mpu' /proc/interrupts \
    > "$RUN_DIR/interrupts-end.txt" || true

grep 'AB_STATS backend=user_i2c' \
    "$RUN_DIR/carterminal-user-i2c.log" \
    > "$RUN_DIR/ab-stats-all.txt"

grep -E '\[(WARN |ERROR)\]\[mpu6050\]' \
    "$RUN_DIR/carterminal-user-i2c.log" \
    > "$RUN_DIR/mpu-warnings-errors.txt" || true

{
    echo "debug_sample_lines=$(
        grep -c '\[DEBUG\].*mpu6050: ax=' \
            "$RUN_DIR/carterminal-user-i2c.log" 2>/dev/null ||
        true
    )"

    echo "warning_error_lines=$(
        wc -l < "$RUN_DIR/mpu-warnings-errors.txt"
    )"

    echo "process_sample_lines=$(
        tail -n +2 "$RUN_DIR/process-samples.csv" |
        wc -l
    )"
} > "$RUN_DIR/test-counts.txt"

echo
echo "===== Stop user_i2c application ====="

"$INIT" stop
sleep 2

extract_test_log "$RUN_DIR/carterminal-user-i2c-final.log"

grep 'AB_STATS backend=user_i2c' \
    "$RUN_DIR/carterminal-user-i2c-final.log" |
tail -n 1 \
    > "$RUN_DIR/ab-stats-final.txt" || true

restore_iio

(
    cd "$RUN_DIR"

    sha256sum \
        vehicle_devices.conf.before \
        carterminal-before.log \
        carterminal-user-i2c.log \
        carterminal-user-i2c-final.log \
        ab-stats-start.txt \
        ab-stats-end.txt \
        ab-stats-final.txt \
        ab-stats-all.txt \
        process-samples.csv \
        start-context.txt \
        end-context.txt \
        interrupts-start.txt \
        interrupts-end.txt \
        mpu-warnings-errors.txt \
        test-counts.txt \
        restore-state.txt \
        > SHA256SUMS
)

echo
echo "===== Test summary ====="
cat "$RUN_DIR/test-counts.txt"

echo "RUN_DIR=$RUN_DIR"
echo "PASS: user_i2c 120-second evidence captured and IIO restored"

trap - EXIT INT TERM
