/* Where the daemon gets its telephone line from.
 *
 * A line delivers, every 10 ms, 80 received G.711 codewords and the hook
 * state, and takes 80 codewords to send. The exchange is the daemon's only
 * clock: line_exchange() blocks until the next block is due.
 *
 * Back ends:
 *   fd:N     an open stream descriptor (a socket pair from a test program,
 *            a pipe to a helper). Wire format, strictly alternating:
 *            to us:    1 octet hook (0 on hook, 1 off hook) + 80 codewords
 *            from us:  80 codewords, the answer to that block
 *   unix:PATH  the same over a UNIX stream socket we connect to
 *   seq:PATH   the dev: protocol below over a UNIX packet socket
 *            (SOCK_SEQPACKET) we connect to: one message per block or
 *            command, as one read or write of the device would carry. The
 *            line helper of boxes whose telephony driver is not ours
 *            serves it (fw/obi/obiline, the OBi200).
 *
 *   dev:PATH   the same over a character device: the SLIC driver of the box
 *            (fw/drv). One read gives the 81 octets of the next block and
 *            blocks until it is due; one write takes 80 codewords. A write
 *            of one octet sets the law (line_set_law) or the receive format
 *            (line_set_linear). With linear receive the codec hands over
 *            16-bit samples that never passed a G.711 encoder: one read
 *            then gives 161 octets, the hook and 80 little-endian 16-bit
 *            values on the scale of the G.711 tables, and the length of what
 *            a read returns tells the two formats apart (line_read2). What
 *            we send stays 80 codewords: the driver puts their exact G.711
 *            levels on the line (fw/drv/pump.h).
 *
 * What the driver must guarantee beyond the contract: every codeword we hand
 * over is transmitted, in order, exactly once (V.90 does not survive a
 * slipped or repeated sample), and the delay from our output to its echo in
 * our input is constant during a call. */
#ifndef V90D_SRV_LINE_H
#define V90D_SRV_LINE_H
#include <stdint.h>

#define LINE_BLOCK 80

typedef struct { int fd; int dev; } line_t;

/* 0, or -1 with errno; spec as above */
int  line_open(line_t *l, const char *spec);
/* Send tx (the answer to the previous block), then wait for the next block.
 * 1 = got one; 0 = the other side closed the line; -1 = error (errno). The
 * first call sends nothing. */
int  line_exchange(line_t *l, const uint8_t tx[LINE_BLOCK], int have_tx, uint8_t rx[LINE_BLOCK], int *offhook);
/* The two halves, for an owner of several lines who waits for them with
 * poll() on l->fd: take the block that is due (same returns), then hand
 * over the answer to it (0, or -1 with errno; a closed line is 0 too, the
 * next line_read tells). */
int  line_read(line_t *l, uint8_t rx[LINE_BLOCK], int *offhook);
int  line_write(line_t *l, const uint8_t tx[LINE_BLOCK]);
/* line_read for a line that may give linear samples: *linear = 0 and rx
 * filled, or *linear = 1 and lin filled. (line_read on such a block fails
 * with EPROTO.) */
int  line_read2(line_t *l, uint8_t rx[LINE_BLOCK], int16_t lin[LINE_BLOCK], int *linear, int *offhook);
/* Ask the box's driver for linear (1) or companded (0) receive; it changes
 * over within some 10 ms, and blocks of the new format follow. 0, or -1 with
 * errno (ENOTSUP: not a line that can). */
int  line_set_linear(line_t *l, int on);
/* A linear sample as the codec gives it, put on the modem's scale (the
 * 16-bit scale of the G.711 tables, 0 dBm0 = 15770 RMS). Measured on the
 * unit (HARDWARE.md, "Linear PCM and the gain knobs"): the Si3217x hands
 * out the same signal 10.9 dB lower in linear format than its compander
 * would encode it, a factor of 2/7 in amplitude. Times 7/2, rounded away
 * from zero, limited to +/-32767. line_read2 applies it. */
int16_t line_lin_sample(int16_t codec);
/* Which G.711 law the codewords are in (0 mu-law, 1 A-law). The box's driver
 * has to know to set its codec; it is told in one octet, 'u' or 'a', a write
 * the driver tells from a block by its length. Other back ends carry
 * codewords as they are and are not told. 0, or -1 with errno. */
int  line_set_law(line_t *l, int alaw);
void line_close(line_t *l);

#endif
