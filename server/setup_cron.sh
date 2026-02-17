#!/bin/bash
# Run on the Pi to set up cron jobs for the astronomy dashboard.
# Usage: bash ~/dev/m5-astro/server/setup_cron.sh

DIR="$HOME/dev/m5-astro/server"
VENV="$DIR/.venv/bin/python"

# Ensure venv exists
if [ ! -f "$VENV" ]; then
    echo "Setting up venv..."
    cd "$DIR"
    uv venv
    uv pip install -r requirements.txt
fi

# Build cron entries
UPDATE_JOB="14,44 * * * * cd $DIR && $VENV update_dashboard.py >> $DIR/cron.log 2>&1"
SERVER_JOB="@reboot cd $DIR && python3 $DIR/serve.py 8081 > $DIR/http.log 2>&1 &"

# Add to crontab (preserving existing entries, removing old versions of these jobs)
(crontab -l 2>/dev/null | grep -v 'm5-astro'; echo "$UPDATE_JOB"; echo "$SERVER_JOB") | crontab -

echo "Cron jobs installed:"
crontab -l | grep -E 'm5-astro|update_dashboard.*8081'
echo ""
echo "Starting file server now..."
pkill -f "serve.py 8081" 2>/dev/null || true
cd "$DIR" && python3 "$DIR/serve.py" 8081 > "$DIR/http.log" 2>&1 &
echo "Running initial update..."
cd "$DIR" && "$VENV" update_dashboard.py
echo "Done! Dashboard at http://$(hostname -I | awk '{print $1}'):8081/dashboard.json"
