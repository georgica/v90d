# Sourced by rcS (and by tools/test_confpack.sh). The flash keeps one file
# for the box: the settings ($CONF) and, when the owner has given one on the
# Dreamcast page, the VPN profile ($VPN). The profile rides behind the
# settings, every line of it after "#%vpn ": to a settings reader those are
# comments, so the file is still a settings file.

# the file for the flash, on standard output
conf_pack() {
    cat "$CONF"
    [ -s "$VPN" ] && sed 's/^/#%vpn /' "$VPN"
    return 0
}

# $CONF holds such a file: take the profile out of it into $VPN (or remove
# $VPN when it carries none)
conf_split() {
    if grep -q '^#%vpn ' "$CONF"; then
        ( umask 077; sed -n 's/^#%vpn //p' "$CONF" > "$VPN" )
        grep -v '^#%vpn ' "$CONF" > "$CONF.split"
        cat "$CONF.split" > "$CONF"
        rm -f "$CONF.split"
    else
        rm -f "$VPN"
    fi
}
