#!/bin/sh
# Print "version=X.Y.Z" for the latest v90d release on GitHub, or nothing.
# Used by the daemon's daily update check (srv/update.c), which runs it only
# when the owner has turned "Check for updates" on. It reads the public
# releases API and pulls out the tag; no account, no identifying data beyond
# the request itself. Overridable in the daemon with -u (for tests).
repo="${V90D_REPO:-georgica/v90d}"
wget -q -T 20 -O - "https://api.github.com/repos/$repo/releases/latest" 2>/dev/null \
  | sed -n 's/.*"tag_name"[: ]*"[vV]\{0,1\}\([0-9][0-9.]*\)".*/version=\1/p' \
  | head -1
