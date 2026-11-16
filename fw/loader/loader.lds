/* v90d loader: one flat image at LD_LINK (set by the Makefile), entry first */
OUTPUT_ARCH(arm)
ENTRY(_start)
SECTIONS
{
	. = LD_LINK;
	.text : { KEEP(*(.text.start)) *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	. = ALIGN(4);
	__image_end = .;
	.bss (NOLOAD) : { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __bss_end = .; }
	. = ALIGN(8);
	. += 0x2000;
	__stack_top = .;
	/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) *(.ARM.attributes) }
}
