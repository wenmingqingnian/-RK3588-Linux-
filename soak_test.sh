#!/bin/bash
# 30-minute soak test for scenario C (file + GUI + RTSP)
# Monitors: FPS stability, memory growth, thread count, crashes

cd ~/RK3588S_CarTerminal

LOG_FILE=logs/soak_C_30min_$(date +%Y%m%d_%H%M%S).log
STATS_FILE=logs/soak_C_stats_$(date +%Y%m%d_%H%M%S).csv

echo "LOG_FILE=$LOG_FILE"
echo "STATS_FILE=$STATS_FILE"
echo "Duration: 30 minutes (1800 seconds)"
echo "Start time: $(date)"
echo "---"

# CSV header
echo "timestamp,elapsed_sec,rss_kb,threads,source_fps,gui_fps,rtsp_fps,gui_drops,rtsp_drops,gui_queue_max,rtsp_queue_max" > "$STATS_FILE"

# Start CarTerminal in background with 30-minute timeout
timeout -s INT -k 15s 1800s ./scripts/run_hdmi.sh > "$LOG_FILE" 2>&1 &
APP_PID=$!
echo "CarTerminal PID: $APP_PID"

# Monitor loop: sample every 60 seconds
START_TIME=$(date +%s)
while true; do
    # Check if process is still running
    if ! kill -0 $APP_PID 2>/dev/null; then
        echo "[$(date)] CarTerminal process exited"
        break
    fi

    CURRENT_TIME=$(date +%s)
    ELAPSED=$((CURRENT_TIME - START_TIME))
    if [ $ELAPSED -ge 1800 ]; then
        echo "[$(date)] 30 minutes reached, stopping"
        break
    fi

    # Sample memory (RSS in KB) and thread count
    RSS=$(cat /proc/$APP_PID/status 2>/dev/null | grep VmRSS | awk '{print $2}')
    THREADS=$(cat /proc/$APP_PID/status 2>/dev/null | grep Threads | awk '{print $2}')

    # Extract latest MEDIA_STATS from log
    LATEST_STATS=$(grep "MEDIA_STATS" "$LOG_FILE" | tail -n 4)
    SOURCE_FPS=$(echo "$LATEST_STATS" | grep "source frames" | sed -n 's/.*fps=\([0-9.]*\).*/\1/p' | tail -1)
    GUI_FPS=$(echo "$LATEST_STATS" | grep "gui frames" | sed -n 's/.*fps=\([0-9.]*\).*/\1/p' | tail -1)
    RTSP_FPS=$(echo "$LATEST_STATS" | grep "rtsp frames" | sed -n 's/.*fps=\([0-9.]*\).*/\1/p' | tail -1)
    GUI_DROPS=$(echo "$LATEST_STATS" | grep "gui frames" | sed -n 's/.*drops=\([0-9]*\).*/\1/p' | tail -1)
    RTSP_DROPS=$(echo "$LATEST_STATS" | grep "rtsp frames" | sed -n 's/.*drops=\([0-9]*\).*/\1/p' | tail -1)
    GUI_QMAX=$(echo "$LATEST_STATS" | grep "gui frames" | sed -n 's/.*max=\([0-9]*\).*/\1/p' | tail -1)
    RTSP_QMAX=$(echo "$LATEST_STATS" | grep "rtsp frames" | sed -n 's/.*max=\([0-9]*\).*/\1/p' | tail -1)

    echo "$(date +%Y-%m-%dT%H:%M:%S),$ELAPSED,${RSS:-N/A},${THREADS:-N/A},${SOURCE_FPS:-N/A},${GUI_FPS:-N/A},${RTSP_FPS:-N/A},${GUI_DROPS:-N/A},${RTSP_DROPS:-N/A},${GUI_QMAX:-N/A},${RTSP_QMAX:-N/A}" >> "$STATS_FILE"

    echo "[$(date)] elapsed=${ELAPSED}s rss=${RSS}KB threads=${THREADS} src=${SOURCE_FPS} gui=${GUI_FPS} rtsp=${RTSP_FPS}"

    sleep 60
done

# Stop CarTerminal gracefully
kill -INT $APP_PID 2>/dev/null
sleep 5
kill -KILL $APP_PID 2>/dev/null

echo ""
echo "=== Soak Test Complete ==="
echo "End time: $(date)"
echo "Total runtime: $(($(date +%s) - START_TIME)) seconds"
echo ""
echo "=== Memory/Thread/FPS Statistics (CSV) ==="
cat "$STATS_FILE"
echo ""
echo "=== Key Events ==="
echo "MEDIA_STATS count: $(grep -c 'MEDIA_STATS' "$LOG_FILE")"
echo "Error count: $(grep -cE 'ERROR|FATAL|crash' "$LOG_FILE")"
echo "Warning count: $(grep -c 'WARN' "$LOG_FILE")"
echo ""
echo "=== First and Last MEDIA_STATS ==="
echo "--- First ---"
grep "MEDIA_STATS" "$LOG_FILE" | head -n 4
echo "--- Last ---"
grep "MEDIA_STATS" "$LOG_FILE" | tail -n 4
echo ""
echo "=== Exit Code ==="
wait $APP_PID 2>/dev/null
EXIT_CODE=$?
echo "CarTerminal exit code: $EXIT_CODE"
echo ""
echo "Log file: $LOG_FILE"
echo "Stats file: $STATS_FILE"
