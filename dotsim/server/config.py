"""Dot Sim's settings: the KODE_* environment variables, read once at import.

See deploy/dotsim.service for the values on the live server.
"""
import os
from pathlib import Path

ROOT = Path(__file__).parent
STATIC = Path(os.environ.get("KODE_STATIC", ROOT.parent / "web"))       # ours (dotsim/web), at /dotsim/
KSWEB = Path(os.environ.get("KODE_KSWEB", ROOT.parent.parent / "web"))   # kode-sim's page, at /
PORT = int(os.environ.get("KODE_PORT", "8000"))
BUILDS = Path(os.environ.get("KODE_BUILDS", "/opt/kode/builds"))         # <app>/kode_sim_app.elf
DATA = Path(os.environ.get("KODE_DATA", "/opt/kode/data"))               # <app>/: the app's saved /data
MAX_SESSIONS = int(os.environ.get("KODE_MAX_SESSIONS", "4"))
WORK = Path(os.environ.get("KODE_WORK", "/opt/kode/work"))               # the build's scratch folder
SIM = Path(os.environ.get("KODE_SIM", "/opt/kode/sim"))                  # dotsim/hostsim, with build.sh
SDK = Path(os.environ.get("KODE_SDK", "/opt/kode/src/kode-sdk"))
LOCAL_APPS = Path(os.environ.get("KODE_LOCAL_APPS", "/opt/kode/apps"))   # the assembled dotsim/apps/<id>/

EDIT_PREFIX = "g_"     # app ids of builds from the editor (cleaned up after GUEST_KEEP_S, not in the app list)
PRIVATE_APPS = set()   # app ids that only a logged-in user (Remote-User) may see and start
