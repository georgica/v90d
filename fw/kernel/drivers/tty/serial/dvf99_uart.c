// SPDX-License-Identifier: GPL-2.0
/*
 * Serial port of the DSPG DVF99 SoC (a block of DSPG's own: not a 16550,
 * not a PL011, not a DesignWare UART), with console and earlycon.
 *
 * Written for the v90d project from refs/dvf99-core-facts.md section 3.
 */
#include <linux/clk.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/platform_data/dvf99-cmu.h>

#define DUART_CTL	0x00
#define  CTL_ENABLE	BIT(0)
#define DUART_CFG	0x04
#define  CFG_7BIT	(1 << 2)
#define  CFG_STOP2	BIT(4)
#define  CFG_ODD	BIT(5)
#define  CFG_PARITY	BIT(6)
#define  CFG_BREAK_DET	BIT(11)
#define DUART_INT_DIV	0x08
#define DUART_FRAC_DIV	0x0c
#define DUART_TX_WM	0x10
#define DUART_RX_WM	0x14
#define DUART_ISR	0x18
#define DUART_IER	0x1c
#define DUART_ICR	0x20
#define  INT_TX_OVER	BIT(0)
#define  INT_TX_WM	BIT(1)
#define  INT_RX_UNDER	BIT(8)
#define  INT_RX_OVER	BIT(9)
#define  INT_RX_WM	BIT(10)
#define  INT_RX_TIMEOUT	BIT(11)
#define  INT_BREAK	BIT(12)
#define  INT_CLEARABLE	(INT_TX_OVER | INT_RX_UNDER | INT_RX_OVER | INT_BREAK)
#define  INT_RX_ALL	(INT_RX_OVER | INT_RX_WM | INT_RX_TIMEOUT | INT_BREAK)
#define DUART_TX_LVL	0x24
#define DUART_RX_LVL	0x28
#define DUART_TX_DATA	0x2c
#define DUART_RX_DATA	0x30
#define  RX_PARITY_ERR	BIT(9)
#define  RX_FRAME_ERR	BIT(10)
#define DUART_STAT	0x34
#define  STAT_TX_EMPTY	BIT(0)
#define  STAT_TX_FULL	BIT(1)
#define  STAT_RX_EMPTY	BIT(8)

#define DVF99_FIFO	128
#define DVF99_NR	2
#define DVF99_NAME	"ttyS"
#define PORT_DVF99	251		/* a port type number nobody else uses */

struct dvf99_port {
	struct uart_port port;
	struct clk *clk;
	u32 ier;
};
#define to_dp(p) container_of(p, struct dvf99_port, port)

static inline u32 rd(struct uart_port *p, unsigned int reg)
{
	return readl(p->membase + reg);
}

static inline void wr(struct uart_port *p, unsigned int reg, u32 v)
{
	writel(v, p->membase + reg);
}

static void dvf99_set_ier(struct uart_port *p, u32 set, u32 clear)
{
	struct dvf99_port *dp = to_dp(p);

	dp->ier = (dp->ier | set) & ~clear;
	wr(p, DUART_IER, dp->ier);
}

static unsigned int dvf99_tx_empty(struct uart_port *p)
{
	return rd(p, DUART_STAT) & STAT_TX_EMPTY ? TIOCSER_TEMT : 0;
}

static void dvf99_set_mctrl(struct uart_port *p, unsigned int mctrl) { }

static unsigned int dvf99_get_mctrl(struct uart_port *p)
{
	return TIOCM_CAR | TIOCM_DSR | TIOCM_CTS;
}

static void dvf99_stop_tx(struct uart_port *p)
{
	dvf99_set_ier(p, 0, INT_TX_WM);
}

static void dvf99_tx_chars(struct uart_port *p)
{
	u8 ch;

	uart_port_tx_limited(p, ch, DVF99_FIFO / 2,
		!(rd(p, DUART_STAT) & STAT_TX_FULL),
		wr(p, DUART_TX_DATA, ch),
		({}));
	if (uart_circ_empty(&p->state->xmit) || uart_tx_stopped(p))
		dvf99_set_ier(p, 0, INT_TX_WM);
	else
		dvf99_set_ier(p, INT_TX_WM, 0);
}

static void dvf99_start_tx(struct uart_port *p)
{
	dvf99_tx_chars(p);
}

static void dvf99_stop_rx(struct uart_port *p)
{
	dvf99_set_ier(p, 0, INT_RX_ALL);
}

static void dvf99_break_ctl(struct uart_port *p, int ctl) { }

static void dvf99_rx_chars(struct uart_port *p, u32 isr)
{
	int max = 4 * DVF99_FIFO;

	if (isr & INT_BREAK) {
		p->icount.brk++;
		uart_handle_break(p);
	}
	if (isr & INT_RX_OVER) {
		p->icount.overrun++;
		tty_insert_flip_char(&p->state->port, 0, TTY_OVERRUN);
	}
	while (!(rd(p, DUART_STAT) & STAT_RX_EMPTY) && max--) {
		u32 d = rd(p, DUART_RX_DATA);
		u8 flag = TTY_NORMAL;

		p->icount.rx++;
		if (d & RX_FRAME_ERR) {
			p->icount.frame++;
			flag = TTY_FRAME;
		} else if (d & RX_PARITY_ERR) {
			p->icount.parity++;
			flag = TTY_PARITY;
		}
		if (uart_handle_sysrq_char(p, d & 0xff))
			continue;
		tty_insert_flip_char(&p->state->port, d & 0xff, flag);
	}
	tty_flip_buffer_push(&p->state->port);
}

static irqreturn_t dvf99_irq(int irq, void *dev_id)
{
	struct uart_port *p = dev_id;
	u32 isr;

	spin_lock(&p->lock);
	isr = rd(p, DUART_ISR) & to_dp(p)->ier;
	if (isr & INT_CLEARABLE)
		wr(p, DUART_ICR, isr & INT_CLEARABLE);
	if (isr & INT_RX_ALL)
		dvf99_rx_chars(p, isr);
	if (isr & INT_TX_WM)
		dvf99_tx_chars(p);
	spin_unlock(&p->lock);
	return isr ? IRQ_HANDLED : IRQ_NONE;
}

static int dvf99_startup(struct uart_port *p)
{
	unsigned long flags;
	int ret;

	ret = request_irq(p->irq, dvf99_irq, 0, "dvf99-uart", p);
	if (ret)
		return ret;
	spin_lock_irqsave(&p->lock, flags);
	wr(p, DUART_ICR, INT_CLEARABLE);
	wr(p, DUART_TX_WM, DVF99_FIFO / 2);
	wr(p, DUART_RX_WM, DVF99_FIFO / 2);
	to_dp(p)->ier = 0;
	dvf99_set_ier(p, INT_RX_ALL, 0);
	wr(p, DUART_CTL, CTL_ENABLE);
	spin_unlock_irqrestore(&p->lock, flags);
	return 0;
}

static void dvf99_shutdown(struct uart_port *p)
{
	unsigned long flags;

	spin_lock_irqsave(&p->lock, flags);
	to_dp(p)->ier = 0;
	wr(p, DUART_IER, 0);
	spin_unlock_irqrestore(&p->lock, flags);
	free_irq(p->irq, p);
}

static void dvf99_set_termios(struct uart_port *p, struct ktermios *termios,
			      const struct ktermios *old)
{
	unsigned long flags;
	unsigned int baud, div16;
	u32 cfg = CFG_BREAK_DET;
	int i;

	/* 8 or 7 data bits, no hardware flow control lines on this board */
	termios->c_cflag &= ~(CRTSCTS | CMSPAR);
	if ((termios->c_cflag & CSIZE) == CS7) {
		cfg |= CFG_7BIT;
	} else {
		termios->c_cflag &= ~CSIZE;
		termios->c_cflag |= CS8;
	}
	if (termios->c_cflag & CSTOPB)
		cfg |= CFG_STOP2;
	if (termios->c_cflag & PARENB) {
		cfg |= CFG_PARITY;
		if (termios->c_cflag & PARODD)
			cfg |= CFG_ODD;
	}
	baud = uart_get_baud_rate(p, termios, old, 300, p->uartclk / 16);
	div16 = p->uartclk / baud;

	spin_lock_irqsave(&p->lock, flags);
	/* let what is in the FIFO go out at the old speed */
	for (i = 0; i < 20000 && !(rd(p, DUART_STAT) & STAT_TX_EMPTY); i++)
		udelay(10);
	wr(p, DUART_CTL, 0);
	wr(p, DUART_CFG, cfg);
	wr(p, DUART_INT_DIV, div16 >> 4);
	wr(p, DUART_FRAC_DIV, div16 & 0xf);
	wr(p, DUART_ICR, INT_CLEARABLE);
	wr(p, DUART_CTL, CTL_ENABLE);

	p->read_status_mask = 0;
	p->ignore_status_mask = 0;
	uart_update_timeout(p, termios->c_cflag, baud);
	spin_unlock_irqrestore(&p->lock, flags);

	if (tty_termios_baud_rate(termios))
		tty_termios_encode_baud_rate(termios, baud, baud);
}

static const char *dvf99_type(struct uart_port *p)
{
	return "DVF99";
}

static void dvf99_release_port(struct uart_port *p) { }
static int dvf99_request_port(struct uart_port *p) { return 0; }

static void dvf99_config_port(struct uart_port *p, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		p->type = PORT_DVF99;
}

static int dvf99_verify_port(struct uart_port *p, struct serial_struct *ser)
{
	return -EINVAL;
}

#ifdef CONFIG_CONSOLE_POLL
static int dvf99_poll_get_char(struct uart_port *p)
{
	if (rd(p, DUART_STAT) & STAT_RX_EMPTY)
		return NO_POLL_CHAR;
	return rd(p, DUART_RX_DATA) & 0xff;
}
#endif

static void dvf99_putc(struct uart_port *p, unsigned char ch)
{
	int i;

	for (i = 0; i < 100000 && (readl(p->membase + DUART_STAT) & STAT_TX_FULL); i++)
		cpu_relax();
	writel(ch, p->membase + DUART_TX_DATA);
}

static const struct uart_ops dvf99_ops = {
	.tx_empty	= dvf99_tx_empty,
	.set_mctrl	= dvf99_set_mctrl,
	.get_mctrl	= dvf99_get_mctrl,
	.stop_tx	= dvf99_stop_tx,
	.start_tx	= dvf99_start_tx,
	.stop_rx	= dvf99_stop_rx,
	.break_ctl	= dvf99_break_ctl,
	.startup	= dvf99_startup,
	.shutdown	= dvf99_shutdown,
	.set_termios	= dvf99_set_termios,
	.type		= dvf99_type,
	.release_port	= dvf99_release_port,
	.request_port	= dvf99_request_port,
	.config_port	= dvf99_config_port,
	.verify_port	= dvf99_verify_port,
#ifdef CONFIG_CONSOLE_POLL
	.poll_get_char	= dvf99_poll_get_char,
	.poll_put_char	= dvf99_putc,
#endif
};

static struct dvf99_port *dvf99_ports[DVF99_NR];

#ifdef CONFIG_SERIAL_DVF99_CONSOLE
static void dvf99_console_write(struct console *co, const char *s, unsigned int n)
{
	struct uart_port *p = &dvf99_ports[co->index]->port;
	unsigned long flags;
	int locked = 1;

	if (oops_in_progress)
		locked = spin_trylock_irqsave(&p->lock, flags);
	else
		spin_lock_irqsave(&p->lock, flags);
	uart_console_write(p, s, n, dvf99_putc);
	if (locked)
		spin_unlock_irqrestore(&p->lock, flags);
}

static int dvf99_console_setup(struct console *co, char *options)
{
	int baud = 115200, bits = 8, parity = 'n', flow = 'n';

	if (co->index < 0 || co->index >= DVF99_NR || !dvf99_ports[co->index])
		return -ENODEV;
	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);
	return uart_set_options(&dvf99_ports[co->index]->port, co, baud, parity, bits, flow);
}

static struct uart_driver dvf99_driver;
static struct console dvf99_console = {
	.name	= DVF99_NAME,
	.write	= dvf99_console_write,
	.device	= uart_console_device,
	.setup	= dvf99_console_setup,
	.flags	= CON_PRINTBUFFER,
	.index	= -1,
	.data	= &dvf99_driver,
};
#define DVF99_CONSOLE (&dvf99_console)

static void dvf99_early_write(struct console *co, const char *s, unsigned int n)
{
	struct earlycon_device *dev = co->data;

	uart_console_write(&dev->port, s, n, dvf99_putc);
}

static int __init dvf99_early_setup(struct earlycon_device *dev, const char *opt)
{
	if (!dev->port.membase)
		return -ENODEV;
	dev->con->write = dvf99_early_write;
	return 0;
}
OF_EARLYCON_DECLARE(dvf99, "dspg,dvf99-uart", dvf99_early_setup);
#else
#define DVF99_CONSOLE NULL
#endif

static struct uart_driver dvf99_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= "dvf99-uart",
	.dev_name	= DVF99_NAME,
	.major		= TTY_MAJOR,
	.minor		= 64,
	.nr		= DVF99_NR,
	.cons		= DVF99_CONSOLE,
};

static int dvf99_probe(struct platform_device *pdev)
{
	struct dvf99_port *dp;
	struct resource *res;
	u32 gate;
	int line, irq, ret;

	line = of_alias_get_id(pdev->dev.of_node, "serial");
	if (line < 0)
		line = 0;
	if (line >= DVF99_NR || dvf99_ports[line])
		return -EBUSY;

	dp = devm_kzalloc(&pdev->dev, sizeof(*dp), GFP_KERNEL);
	if (!dp)
		return -ENOMEM;
	dp->port.membase = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(dp->port.membase))
		return PTR_ERR(dp->port.membase);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	dp->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(dp->clk))
		return PTR_ERR(dp->clk);

	/* the boot loader opens only the port it uses as its console */
	if (!of_property_read_u32(pdev->dev.of_node, "dspg,gate", &gate)) {
		dvf99_cmu_gate(gate, true);
		dvf99_cmu_reset(gate, false);
	}

	dp->port.dev = &pdev->dev;
	dp->port.mapbase = res->start;
	dp->port.irq = irq;
	dp->port.iotype = UPIO_MEM32;
	dp->port.uartclk = clk_get_rate(dp->clk);
	dp->port.fifosize = DVF99_FIFO;
	dp->port.ops = &dvf99_ops;
	dp->port.flags = UPF_BOOT_AUTOCONF;
	dp->port.line = line;
	dp->port.has_sysrq = IS_ENABLED(CONFIG_SERIAL_DVF99_CONSOLE);
	dvf99_ports[line] = dp;
	platform_set_drvdata(pdev, dp);

	ret = uart_add_one_port(&dvf99_driver, &dp->port);
	if (ret)
		dvf99_ports[line] = NULL;
	return ret;
}

static const struct of_device_id dvf99_of_match[] = {
	{ .compatible = "dspg,dvf99-uart" },
	{ }
};

static struct platform_driver dvf99_platform_driver = {
	.probe	= dvf99_probe,
	.driver	= {
		.name		= "dvf99-uart",
		.of_match_table	= dvf99_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init dvf99_uart_init(void)
{
	int ret = uart_register_driver(&dvf99_driver);

	if (ret)
		return ret;
	ret = platform_driver_register(&dvf99_platform_driver);
	if (ret)
		uart_unregister_driver(&dvf99_driver);
	return ret;
}
arch_initcall(dvf99_uart_init);
