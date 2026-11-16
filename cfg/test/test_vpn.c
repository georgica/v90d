/* the OpenVPN profile check of vpn.h */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "vpn.h"

static char msg[VPN_MSG_MAX];
static int chk(const char *t) { msg[0] = 0; return vpn_profile_check(t, strlen(t), msg, sizeof msg); }

static const char good[] =
    "# a client\r\nclient\ndev tun\nproto udp\n\nremote vpn.example.net 1194\nremote 192.0.2.5 443 tcp\n"
    "resolv-retry infinite\nnobind\npersist-key\npersist-tun\nremote-cert-tls server\ncipher AES-256-GCM\n"
    "data-ciphers AES-256-GCM:AES-128-GCM\nauth SHA256\nkey-direction 1\nverb 3\n; old style comment\n"
    "<ca>\n-----BEGIN CERTIFICATE-----\nMIIB+up/down==\n-----END CERTIFICATE-----\n</ca>\n"
    "<cert>\nabc\n</cert>\n<key>\nabc\n</key>\n<tls-auth>\n#\n# 2048 bit OpenVPN static key\n#\nabc\n</tls-auth>\n"
    "<auth-user-pass>\nplayer\nsecret\n</auth-user-pass>\ncompress\ncomp-lzo no\nroute 10.200.0.0 255.255.255.0\n";

int main(void)
{
    char r[40]; static char big[VPN_PROFILE_MAX + 200]; size_t i;

    CHECK_EQ_I(chk(good), 0);
    CHECK_EQ_I(vpn_profile_remote(good, strlen(good), r, sizeof r), 0); CHECK(strcmp(r, "vpn.example.net 1194") == 0);
    CHECK_EQ_I(vpn_profile_remote("client\n", 7, r, sizeof r), -1);
    CHECK_EQ_I(vpn_profile_remote("remote averyveryverylonghostname.example.org 1194\n", 50, r, 16), 0); CHECK_EQ_I(strlen(r), 15);

    /* what runs programs, names files, or takes the box's traffic away */
    CHECK_EQ_I(chk("client\nup /bin/sh\n"), -1); CHECK(strcmp(msg, "line 2: 'up' is not allowed in a profile here") == 0);
    CHECK_EQ_I(chk("script-security 3\n"), -1);
    CHECK_EQ_I(chk("plugin /lib/x.so\n"), -1);
    CHECK_EQ_I(chk("ca /etc/ca.crt\n"), -1);
    CHECK_EQ_I(chk("log /tmp/x\n"), -1);
    CHECK_EQ_I(chk("config /tmp/other\n"), -1);
    CHECK_EQ_I(chk("redirect-gateway def1\n"), -1);
    CHECK_EQ_I(chk("management 0.0.0.0 7505\n"), -1);
    CHECK_EQ_I(chk("daemon\n"), -1);
    CHECK_EQ_I(chk("auth-user-pass /tmp/pw\n"), -1); CHECK(strstr(msg, "line 1") != NULL);
    CHECK_EQ_I(chk("auth-user-pass\nremote a 1\n"), 0);
    CHECK_EQ_I(chk("--up x\n"), -1);
    CHECK_EQ_I(chk("  \tclient\n remote a 1\n"), 0);                 /* indented */
    CHECK_EQ_I(chk("  up x\n"), -1);
    CHECK_EQ_I(chk("UP x\n"), -1);
    CHECK_EQ_I(chk("setenv PATH /tmp\n"), -1);
    /* blocks: known ones only, closed, not nested */
    CHECK_EQ_I(chk("<ca>\nabc\n"), -1); CHECK(strcmp(msg, "line 1: <ca> is not closed") == 0);
    CHECK_EQ_I(chk("<up>\nabc\n</up>\n"), -1);
    CHECK_EQ_I(chk("<ca>\n<key>\n</key>\n</ca>\n"), -1);
    CHECK_EQ_I(chk("</ca>\n"), -1);
    CHECK_EQ_I(chk("<connection>\nremote a 1\n</connection>\n"), -1);
    /* a directive inside a block is text, not a directive */
    CHECK_EQ_I(chk("<ca>\nup /bin/sh\n</ca>\nremote a 1\n"), 0);
    /* octets, sizes */
    CHECK_EQ_I(chk("client\n\x01\n"), -1); CHECK(strcmp(msg, "line 2: not plain text") == 0);
    CHECK_EQ_I(vpn_profile_check("client\0up x\n", 12, msg, sizeof msg), -1);
    CHECK_EQ_I(chk(""), -1); CHECK(strcmp(msg, "the profile is empty") == 0);
    CHECK_EQ_I(chk("# nothing\n\n"), -1);
    CHECK_EQ_I(chk("client\n"), -1); CHECK(strcmp(msg, "the profile names no server (remote)") == 0);
    for (i = 0; i < sizeof big - 1; i++) big[i] = i % 60 == 59 ? '\n' : '#';
    big[sizeof big - 1] = 0;
    CHECK_EQ_I(chk(big), -1); CHECK(strstr(msg, "longer than") != NULL);
    /* the last line needs no newline */
    CHECK_EQ_I(chk("remote a 1\nclient"), 0);
    CHECK_EQ_I(chk("remote a 1\nup x"), -1);
    T_REPORT("test_vpn");
}
