# The Telnet BBS Guide's dialling directory (dialdirectory.xml in its
# archive) as the list of the bulletin board number (srv/bbs.h). Telnet
# boards only. A board's login name is its name in lower case letters and
# digits without the word BBS, the name 2600.network gives the same board;
# a second board of the same name is name2. A board's telephone number is
# 227 and its login name on the keys of a telephone, seven digits, zeros
# behind a short name; a board whose number another has takes the next one
# that is free. For busybox awk.
function attr(s, k,    i, r) {
    i = index(s, " " k "=\"")
    if (!i) return ""
    r = substr(s, i + length(k) + 3)
    return substr(r, 1, index(r, "\"") - 1)
}
function trim(s) { sub(/^ +/, "", s); sub(/ +$/, "", s); return s }
function keypad(s,    i, c, k, r) {
    r = ""
    for (i = 1; i <= length(s) && length(r) < 7; i++) {
        c = substr(s, i, 1)
        if (c ~ /[0-9]/) r = r c
        else { k = index("abcdefghijklmnopqrstuvwxyz", c); if (k) r = r substr("22233344455566677778889999", k, 1) }
    }
    while (length(r) < 7) r = r "0"
    return r
}
BEGIN { print "v90d-bbs 2" }
/<BBS / {
    if (toupper(attr($0, "protocol")) != "TELNET") next
    name = attr($0, "name"); host = trim(attr($0, "ip")); port = trim(attr($0, "port"))
    gsub(/&amp;/, "\\&", name); gsub(/[^ -~]/, "", name)
    name = trim(substr(trim(name), 1, 47))
    if (host !~ /^[A-Za-z0-9][A-Za-z0-9.-]*$/ || length(host) > 63) next
    if (port !~ /^[0-9]+$/ || port + 0 < 1 || port + 0 > 65535) next
    s = " " name " "
    gsub(/\]\[/, "ii", s)
    while (match(s, /[^A-Za-z0-9_][Bb][Bb][Ss][^A-Za-z0-9_]/)) s = substr(s, 1, RSTART) " " substr(s, RSTART + 4)
    s = tolower(s); gsub(/[^a-z0-9]/, "", s)
    login = substr(s, 1, 32)
    if (login == "") next
    n = ++seen[login]
    if (n > 1) { login = substr(login, 1, 32 - length(n)) n; seen[login] = 1 }
    num = keypad(login)
    while (num in taken) num = sprintf("%07d", (num + 1) % 10000000)
    taken[num] = 1
    print "bbs " login " 227" num " " host (port + 0 == 23 ? "" : ":" (port + 0)) " " name
}
