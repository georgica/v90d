/* Tests for the Firmware page's two renderers: what they show for each way
 * a box can have been started, that nothing from outside reaches the page
 * unescaped, and that the buffer bound holds. */
#include <string.h>
#include "test.h"
#include "firmware.h"
#include "page.h"

static char html[FIRMWARE_HTML_MAX + 64];
static int has(const char *s) { return strstr(html, s) != NULL; }

static const char LOADER[] = "version=0.1.0\nscheme=loader\nset=A\nboard=9610003725A known\ncore=0x060000\nstate=0x070000\n"
                             "unconfirmed=0\ninstalled=whole\nsettings=0x080000\nupgrade=0x2e0000\nimage=0x2e0200\n"
                             "image_name=v90d ht802 0.1.0\nimage_octets=2182398\nstock=B good\n";
static const char OLD[]    = "scheme=old\nset=-\nboard=9610003725A known\nold_tries=2\nsettings=0x060000\nupgrade=0x2e0000\n"
                             "image=0x2e0000\nimage_name=v90d ht802 0.1.0\nimage_octets=2182398\nstock=B good\n";
static const char NONE[]   = "scheme=none\nset=-\nboard=9610003725A known\n";

static void test_value(void)
{
    char v[8];
    CHECK(firmware_value(LOADER, "scheme", v, sizeof v) && strcmp(v, "loader") == 0);
    CHECK(firmware_value(LOADER, "set", v, sizeof v) && strcmp(v, "A") == 0);
    CHECK(firmware_value(LOADER, "stock", v, sizeof v) && strcmp(v, "B good") == 0);
    CHECK(firmware_value(LOADER, "image_name", v, sizeof v) && strcmp(v, "v90d ht") == 0);    /* cut to fit, terminated */
    CHECK(!firmware_value(LOADER, "sch", v, sizeof v) && v[0] == 0);
    CHECK(!firmware_value(LOADER, "tries", v, sizeof v));       /* not the tail of another name */
    CHECK(!firmware_value(LOADER, "nothere", v, sizeof v));
    CHECK(!firmware_value("", "scheme", v, sizeof v));
    CHECK(firmware_value("a=1", "a", v, sizeof v) && strcmp(v, "1") == 0);     /* no line end at the end */
    CHECK(!firmware_value("a", "a", v, sizeof v));
    CHECK(firmware_value("a=\nb=2\n", "b", v, sizeof v) && strcmp(v, "2") == 0);
}

static void test_pages(void)
{
    int n = firmware_render_html("0.1.0 (built today)", LOADER, "", 1, html, sizeof html);
    CHECK(n > 0 && n == (int)strlen(html) && n <= FIRMWARE_HTML_MAX);
    CHECK(has("<title>" PAGE_NAME " firmware</title>") && has("<b>Firmware</b>") && has("<a href=\"/system\">System</a>"));
    CHECK(has("<tr><th>Version</th><td>0.1.0 (built today)</td></tr>"));
    CHECK(has("by the v90d loader, from set A of the flash"));
    CHECK(has("v90d ht802 0.1.0, 2182398 octets"));
    CHECK(has("<tr><th>Starts not confirmed</th><td>0<small>"));
    CHECK(has("<tr><th>Stock firmware</th><td>in set B good</td></tr>"));
    CHECK(has("<tr><th>Board</th><td>9610003725A known</td></tr>"));
    CHECK(has("action=\"/firmware/upload\" enctype=\"multipart/form-data\"") && has("<input type=\"file\" name=\"image\" required>"));
    CHECK(has("href=\"/firmware/stock\""));
    CHECK(!has("Last upload") && !has("not complete"));
    CHECK(has("v90d firmware " V90D_VERSION "</footer>"));

    /* the bench scheme */
    n = firmware_render_html("x", OLD, "written: v90d ht802 0.1.0; the box restarts", 1, html, sizeof html);
    CHECK(n > 0 && has("the bench way") && has("<tr><th>Starts left</th><td>2<small>"));
    CHECK(has("<h2>Last upload</h2>\n<p>written: v90d ht802 0.1.0; the box restarts</p>"));
    CHECK(has("action=\"/firmware/upload\""));

    /* nothing to write to: no forms, and it says why */
    n = firmware_render_html("x", NONE, "", 1, html, sizeof html);
    CHECK(n > 0 && has("from memory") && !has("<form") && !has("/firmware/stock") && has("no place in the flash"));
    n = firmware_render_html("x", NULL, NULL, 1, html, sizeof html);
    CHECK(n > 0 && has("from memory") && !has("<form"));
    /* nobody to hand a post to */
    n = firmware_render_html("x", LOADER, "", 0, html, sizeof html);
    CHECK(n > 0 && has("by the v90d loader") && !has("<form") && !has("href=\"/firmware/stock\""));
    /* half an installation is said */
    n = firmware_render_html("x", "scheme=loader\nset=A\ninstalled=half\n", "", 1, html, sizeof html);
    CHECK(n > 0 && has("<tr><th>Installation</th><td>not complete") && has("no system image that checks"));

    n = firmware_render_stock(LOADER, html, sizeof html);
    CHECK(n > 0 && n == (int)strlen(html) && n <= FIRMWARE_HTML_MAX);
    CHECK(has("<form method=\"post\" action=\"/firmware/stock\">") && has("name=\"confirm\" value=\"stock\" required>"));
    CHECK(has("Four octets are written") && has("in set B good") && has("<a href=\"/firmware\">No, go back</a>"));
    n = firmware_render_stock(OLD, html, sizeof html);
    CHECK(n > 0 && has("The start count in the boot loader") && !has("Four octets"));
}

/* what comes from the flash and from the helper is text from outside */
static void test_hostile(void)
{
    static char where[FIRMWARE_TEXT_MAX], state[FIRMWARE_TEXT_MAX];
    int n, i;
    strcpy(where, "scheme=loader\nset=<b>\nimage_name=<script>alert(1)</script>\nimage_octets=\"&'\nstock=</td>\nboard=\x01\xff\n"
                  "unconfirmed=<em>\ninstalled=<u>\n");
    strcpy(state, "<img src=x onerror=1> & more");
    n = firmware_render_html("<v>", where, state, 1, html, sizeof html);
    CHECK(n > 0);
    CHECK(!has("<script>") && !has("<img") && !has("<b>\n") && !has("<em>") && !has("<u>") && !has("<v>"));
    CHECK(has("&lt;script&gt;alert(1)&lt;/script&gt;") && has("&lt;img src=x onerror=1&gt; &amp; more") && has("\\x01\\xff"));
    for (i = 0; i < n; i++) if ((unsigned char)html[i] > 0x7e) break;
    CHECK_EQ_I(i, n);                                   /* pure ASCII */
    n = firmware_render_stock(where, html, sizeof html);
    CHECK(n > 0 && !has("</td>\n<small>") && has("in set &lt;/td&gt;"));

    /* the longest texts still fit the bound the server reserves */
    memset(where, 0, sizeof where);
    strcpy(where, "scheme=loader\nset=A\nimage_name=");
    memset(where + strlen(where), '<', 150);
    strcat(where, "\nimage_octets=<<<<<<<<<<<<<<<\nstock=");
    memset(where + strlen(where), '<', 150);
    strcat(where, "\nboard="); memset(where + strlen(where), '<', 150);
    strcat(where, "\nunconfirmed="); memset(where + strlen(where), '<', 150);
    strcat(where, "\ninstalled=half\n");
    memset(state, '&', sizeof state - 1); state[sizeof state - 1] = 0;
    n = firmware_render_html("&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&", where, state, 1, html, sizeof html);
    CHECK(n > 0 && n <= FIRMWARE_HTML_MAX);
    printf("  largest firmware page: %d of %d octets\n", n, FIRMWARE_HTML_MAX);
    n = firmware_render_stock(where, html, sizeof html);
    CHECK(n > 0 && n <= FIRMWARE_HTML_MAX);
    /* a buffer that is too small: -1, never a write past it */
    memset(html, 'Z', sizeof html);
    CHECK_EQ_I(firmware_render_html("x", LOADER, "", 1, html, 2000), -1);
    CHECK(html[1999] == 0 && html[2000] == 'Z');
    CHECK_EQ_I(firmware_render_stock(LOADER, html, 100), -1);
}

int main(void)
{
    test_value();
    test_pages();
    test_hostile();
    T_REPORT("test_firmware");
}
