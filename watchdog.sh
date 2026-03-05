#!/bin/bash
# watchdog.sh - Enforces the 0.5s maximum execution time constraint

CMD="$@"
if [ -z "$CMD" ]; then
    echo "Usage: ./watchdog.sh <command>"
    exit 1
fi

$CMD &
PID=$!

# Loop for 5 iterations of 0.1s (total 0.5s)
for i in {1..5}; do
    sleep 0.1
    if ! kill -0 $PID 2>/dev/null; then
        # Process completed within 0.5s
        wait $PID
        exit $?
    fi
done

# If we reached here, 0.5s passed and process is still running
echo "=========================================================="
echo "WATCHDOG TIMEOUT ERROR: Command took longer than 0.5s!"
echo "Killing PID $PID..."
echo "=========================================================="

# Attempt to take screenshot (macOS specific)
TIMESTAMP=$(date +%s)
FILENAME="watchdog_timeout_error_${TIMESTAMP}.png"
screencapture "$FILENAME"
echo "Timeout screenshot saved as: $FILENAME"

kill -9 $PID
exit 124
