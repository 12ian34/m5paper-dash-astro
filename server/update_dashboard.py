"""
Fetches astronomy data for the M5Paper dashboard.
Run via cron every 30 minutes.

Data sources:
- Sun: simplified solar equations (no external dependency)
- Moon: simplified phase calculation (no external dependency)
- Planets: PyEphem for naked-eye planet positions from London
- Aurora: AuroraWatch UK API (Lancaster University)
- ISS: PyEphem with TLE from Celestrak
"""

import json
import math
import os
import xml.etree.ElementTree as ET
from datetime import datetime, timedelta, timezone

import ephem
import httpx
from dotenv import load_dotenv

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
load_dotenv(os.path.join(SCRIPT_DIR, "..", ".env"))

DASHBOARD_PATH = os.path.join(SCRIPT_DIR, "dashboard.json")
ISS_TLE_CACHE_PATH = os.path.join(SCRIPT_DIR, "iss_tle.json")

# London
LONDON_LAT = "51.5074"
LONDON_LON = "-0.1278"
LONDON_ELEV = 11  # metres above sea level


def uk_offset() -> int:
    """Return UK timezone offset: 0 for GMT, 1 for BST."""
    now = datetime.now(timezone.utc)
    month = now.month
    if 4 <= month <= 9:
        return 1
    elif month == 3 and now.day >= 25:
        return 1
    elif month == 10 and now.day < 25:
        return 1
    return 0


def get_observer(date=None) -> ephem.Observer:
    """Create an ephem Observer for London."""
    obs = ephem.Observer()
    obs.lat = LONDON_LAT
    obs.lon = LONDON_LON
    obs.elevation = LONDON_ELEV
    obs.pressure = 0  # disable atmospheric refraction for raw positions
    if date:
        obs.date = date
    else:
        obs.date = ephem.now()
    return obs


# ---- Moon ----


def moon_phase() -> dict:
    """
    Calculate current moon phase using a known new moon reference.
    Returns phase name, age in days, and illumination percentage.
    """
    ref = datetime(2000, 1, 6, 18, 14, 0, tzinfo=timezone.utc)
    now = datetime.now(timezone.utc)
    days_since = (now - ref).total_seconds() / 86400
    cycle = 29.53058867
    age = days_since % cycle
    fraction = age / cycle

    illumination = (1 - math.cos(2 * math.pi * fraction)) / 2

    if fraction < 0.0625:
        name = "New Moon"
    elif fraction < 0.1875:
        name = "Waxing Crescent"
    elif fraction < 0.3125:
        name = "First Quarter"
    elif fraction < 0.4375:
        name = "Waxing Gibbous"
    elif fraction < 0.5625:
        name = "Full Moon"
    elif fraction < 0.6875:
        name = "Waning Gibbous"
    elif fraction < 0.8125:
        name = "Last Quarter"
    elif fraction < 0.9375:
        name = "Waning Crescent"
    else:
        name = "New Moon"

    return {
        "name": name,
        "age_days": round(age, 1),
        "illumination_pct": round(illumination * 100, 1),
    }


# ---- Sun ----


def sun_times() -> dict:
    """
    Calculate sunrise/sunset for London using simplified solar equations.
    Returns times as HH:MM strings in local UK time (UTC or BST).
    """
    now = datetime.now(timezone.utc)
    lat = 51.5074
    lng = -0.1278

    n = now.timetuple().tm_yday
    decl = math.radians(-23.44 * math.cos(math.radians(360 / 365 * (n + 10))))
    lat_rad = math.radians(lat)
    cos_ha = -math.tan(lat_rad) * math.tan(decl)
    cos_ha = max(-1, min(1, cos_ha))
    ha = math.degrees(math.acos(cos_ha))

    solar_noon = 12.0 - lng / 15.0
    sunrise_h = solar_noon - ha / 15.0
    sunset_h = solar_noon + ha / 15.0

    offset = uk_offset()
    sunrise_h += offset
    sunset_h += offset

    def fmt(h: float) -> str:
        h = h % 24
        return f"{int(h):02d}:{int((h % 1) * 60):02d}"

    return {"sunrise": fmt(sunrise_h), "sunset": fmt(sunset_h)}


# ---- Planets ----


def azimuth_to_compass(az_deg: float) -> str:
    """Convert azimuth in degrees to 8-point compass direction."""
    dirs = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"]
    idx = round(az_deg / 45) % 8
    return dirs[idx]


def visible_planets() -> dict:
    """
    Calculate which naked-eye planets are visible from London tonight.
    Computes positions 1 hour after the next sunset for best viewing.
    Returns list of {name, dir} for planets above 5 degrees altitude.
    """
    obs = get_observer()
    sun = ephem.Sun()

    try:
        next_set = obs.next_setting(sun)
        view_time = ephem.Date(next_set + ephem.hour)
    except ephem.AlwaysUpError:
        return {"planets": [], "note": "No darkness"}
    except ephem.NeverUpError:
        view_time = ephem.now()

    obs.date = view_time

    bodies = [
        ("Mercury", ephem.Mercury()),
        ("Venus", ephem.Venus()),
        ("Mars", ephem.Mars()),
        ("Jupiter", ephem.Jupiter()),
        ("Saturn", ephem.Saturn()),
    ]

    visible = []
    for name, body in bodies:
        body.compute(obs)
        alt_deg = math.degrees(body.alt)
        az_deg = math.degrees(body.az)

        if alt_deg > 5:
            visible.append({
                "name": name,
                "dir": azimuth_to_compass(az_deg),
            })

    return {"planets": visible}


# ---- Aurora ----


def fetch_aurora() -> dict:
    """
    Fetch current AuroraWatch UK magnetometer status and activity.
    Uses the Lancaster University AuroraWatch API v0.2.

    Thresholds: green <50nT, yellow 50-100nT, amber 100-200nT, red >200nT.
    """
    status_map = {
        "green": "LOW",
        "yellow": "MINOR",
        "amber": "MODERATE",
        "red": "HIGH",
    }

    with httpx.Client(timeout=10.0) as client:
        # Current status (alert level)
        resp = client.get(
            "https://aurorawatch-api.lancs.ac.uk/0.2/status/current-status.xml"
        )
        resp.raise_for_status()
        root = ET.fromstring(resp.text)

        status_id = "unknown"
        for elem in root.iter():
            sid = elem.get("status_id")
            if sid:
                status_id = sid
                break

        level = status_map.get(status_id, status_id.upper())

        # Get actual nT reading from activity data
        nt = None
        try:
            resp2 = client.get(
                "https://aurorawatch-api.lancs.ac.uk/0.2/status/"
                "alerting-site-activity.xml"
            )
            resp2.raise_for_status()
            root2 = ET.fromstring(resp2.text)

            # Structure: <activity><value>49.6</value></activity>
            # Find the last activity element's value child
            last_val = None
            for activity in root2.findall("activity"):
                val_elem = activity.find("value")
                if val_elem is not None and val_elem.text:
                    try:
                        last_val = float(val_elem.text)
                    except (ValueError, TypeError):
                        pass

            if last_val is not None:
                nt = round(last_val)
        except Exception:
            pass

        return {"nt": nt, "level": level, "status_color": status_id}


# ---- ISS ----


def fetch_iss_tle() -> tuple[str, str, str]:
    """Fetch ISS TLE from Celestrak, cached for 24 hours."""
    if os.path.exists(ISS_TLE_CACHE_PATH):
        with open(ISS_TLE_CACHE_PATH) as f:
            cached = json.load(f)
        cached_time = datetime.fromisoformat(cached["fetched_at"])
        if datetime.now(timezone.utc) - cached_time < timedelta(hours=24):
            return cached["name"], cached["line1"], cached["line2"]

    with httpx.Client(timeout=10.0) as client:
        resp = client.get(
            "https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE"
        )
        resp.raise_for_status()

    lines = [ln.strip() for ln in resp.text.strip().split("\n") if ln.strip()]
    name, line1, line2 = lines[0], lines[1], lines[2]

    with open(ISS_TLE_CACHE_PATH, "w") as f:
        json.dump({
            "fetched_at": datetime.now(timezone.utc).isoformat(),
            "name": name,
            "line1": line1,
            "line2": line2,
        }, f)

    return name, line1, line2


def next_iss_pass() -> dict:
    """
    Calculate next visible ISS pass from London.
    A pass is 'visible' when: max altitude > 10 deg AND sun is below -6 deg
    (civil twilight — dark enough to see the sunlit ISS).
    """
    name, line1, line2 = fetch_iss_tle()
    iss = ephem.readtle(name, line1, line2)
    obs = get_observer()
    offset = uk_offset()

    for _ in range(50):
        try:
            info = obs.next_pass(iss)
        except Exception:
            break

        rise_time, rise_az, max_time, max_alt, set_time, set_az = info

        if max_alt is None or rise_time is None or set_time is None:
            obs.date = obs.date + ephem.hour
            continue

        max_alt_deg = math.degrees(float(max_alt))

        if max_alt_deg > 10:
            # Check sky darkness at time of max altitude
            obs_check = get_observer(max_time)
            sun = ephem.Sun()
            sun.compute(obs_check)
            sun_alt = math.degrees(float(sun.alt))

            if sun_alt < -6:
                rise_dt = (
                    ephem.Date(rise_time).datetime().replace(tzinfo=timezone.utc)
                )
                local_rise = rise_dt + timedelta(hours=offset)

                duration = (float(set_time) - float(rise_time)) * 24 * 60

                return {
                    "time": local_rise.strftime("%H:%M"),
                    "date": local_rise.strftime("%d %b"),
                    "max_alt": round(max_alt_deg),
                    "rise_dir": azimuth_to_compass(math.degrees(float(rise_az))),
                    "set_dir": azimuth_to_compass(math.degrees(float(set_az))),
                    "duration_min": round(duration),
                }

        # Advance past this pass
        if set_time:
            obs.date = ephem.Date(float(set_time) + 0.01)
        else:
            obs.date = obs.date + ephem.hour

    return {"error": "No visible pass soon"}


# ---- Main ----


def main():
    widgets: dict = {}

    widgets["sun"] = sun_times()
    widgets["moon"] = moon_phase()

    try:
        widgets["planets"] = visible_planets()
    except Exception as e:
        widgets["planets"] = {"error": str(e), "planets": []}

    try:
        widgets["aurora"] = fetch_aurora()
    except Exception as e:
        widgets["aurora"] = {"error": str(e)}

    try:
        widgets["iss"] = next_iss_pass()
    except Exception as e:
        widgets["iss"] = {"error": str(e)}

    dashboard = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "widgets": widgets,
    }

    with open(DASHBOARD_PATH, "w") as f:
        json.dump(dashboard, f, indent=2)

    print(f"[{dashboard['timestamp']}] Updated dashboard.json")


if __name__ == "__main__":
    main()
