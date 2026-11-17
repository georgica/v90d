# When may this start be called a good one? Sourced by rcS; tested on the
# host by tools/test_startgood.sh.
#
# A start nobody confirms is counted, and after three the box goes back to
# the stock firmware. On a board a unit has run (`v90nv where` says
# tried=yes) the daemon being alive is enough. On a board that was only read
# about, the network port must also have received something: without a
# network nobody can reach the pages, and with them the way back.
#   start_good WHEREFILE INTERFACE     0: confirm it, 1: not yet
start_good() {
    grep -q '^tried=yes$' "$1" 2>/dev/null && return 0
    _n=$(cat "${SYSNET:-/sys/class/net}/$2/statistics/rx_packets" 2>/dev/null)
    [ "${_n:-0}" -gt 0 ] 2>/dev/null
}
