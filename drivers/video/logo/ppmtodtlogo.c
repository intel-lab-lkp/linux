// SPDX-License-Identifier: GPL-2.0-only
/*
 * Convert a PPM image into a devicetree boot logo node, or into the binary
 * blob the "linux,boot-logo-clut224" binding reads from a reserved memory
 * region.
 *
 * Like pnmtologo, this tool does not quantize: the image must already use
 * at most 224 distinct colours. Reduce it first if needed, for instance:
 *
 *	magick logo.png -colors 224 logo.ppm
 *
 * The node is emitted under /chosen, where the binding expects it: a logo is
 * configuration handed over by firmware rather than a description of the
 * hardware.
 *
 * The devicetree output stores plain palette indices; the 32 entry offset
 * the frame buffer layer reserves for the console is applied by the kernel.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CLUT_COLORS	224
#define MAX_PIXELS	(32U * 1024 * 1024)
#define BLOB_MAGIC	0x4f474f4cU	/* "LOGO", little endian */
#define BYTES_PER_LINE	12

static const char *programname;
static const char *filename;
static const char *outputname;
static FILE *out;

enum output_type {
	OUTPUT_DTS,	/* complete overlay */
	OUTPUT_DTSI,	/* bare node, for inclusion */
	OUTPUT_BIN,	/* blob for a reserved memory region */
};

static enum output_type output_type = OUTPUT_DTS;

/* Placement options, emitted into the node */
static int opt_centered;
static char *opt_position;
static char *opt_offset;
static const char *opt_rotation;

struct color {
	unsigned char red;
	unsigned char green;
	unsigned char blue;
};

static unsigned int logo_width;
static unsigned int logo_height;
static unsigned char *logo_data;
static struct color logo_clut[MAX_CLUT_COLORS];
static unsigned int logo_clutsize;

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(1);
}

static void usage(void)
{
	die("Usage: %s [options] <filename>\n"
	    "\n"
	    "Convert a PPM image into a \"linux,boot-logo-clut224\" node.\n"
	    "The image must use at most %d distinct colours.\n"
	    "\n"
	    "    -o <output>     write to file instead of stdout\n"
	    "    -t <type>       dts (default), dtsi or bin\n"
	    "    -c              centre the logo (logo-centered)\n"
	    "    -p <x>,<y>      logo-position\n"
	    "    -f <dx>,<dy>    logo-offset\n"
	    "    -r <rotation>   logo-rotation: cw, ccw, ud or none\n"
	    "    -h              this help\n",
	    programname, MAX_CLUT_COLORS);
}

static unsigned int get_number(FILE *fp)
{
	int c;
	unsigned int val;

	/* Skip leading whitespace */
	do {
		c = fgetc(fp);
		if (c == EOF)
			die("%s: end of file\n", filename);
		if (c == '#') {
			/* Ignore comments 'till end of line */
			do {
				c = fgetc(fp);
				if (c == EOF)
					die("%s: end of file\n", filename);
			} while (c != '\n');
		}
	} while (isspace(c));

	if (!isdigit(c))
		die("%s: expected a number\n", filename);

	/* Parse decimal number */
	val = 0;
	while (isdigit(c)) {
		val = 10 * val + c - '0';
		c = fgetc(fp);
		if (c == EOF)
			break;
	}
	return val;
}

static unsigned char get_byte(FILE *fp)
{
	int c = fgetc(fp);

	if (c == EOF)
		die("%s: end of file\n", filename);
	return c;
}

static unsigned int find_clut_entry(struct color color)
{
	unsigned int i;

	for (i = 0; i < logo_clutsize; i++)
		if (logo_clut[i].red == color.red &&
		    logo_clut[i].green == color.green &&
		    logo_clut[i].blue == color.blue)
			return i;

	if (logo_clutsize == MAX_CLUT_COLORS)
		die("%s: more than %d colors, reduce the image first, e.g.\n"
		    "    magick %s -colors %d out.ppm\n",
		    filename, MAX_CLUT_COLORS, filename, MAX_CLUT_COLORS);

	logo_clut[logo_clutsize] = color;
	return logo_clutsize++;
}

static void read_image(void)
{
	unsigned int i, npixels, maxval;
	int magic, raw;
	FILE *fp;

	fp = fopen(filename, "rb");
	if (!fp)
		die("Cannot open file %s: %s\n", filename, strerror(errno));

	if (fgetc(fp) != 'P')
		die("%s is not a PPM file\n", filename);

	magic = fgetc(fp);
	switch (magic) {
	case '3':
		raw = 0;
		break;
	case '6':
		raw = 1;
		break;
	default:
		die("%s is not a PPM file (only P3 and P6 are supported)\n",
		    filename);
	}

	logo_width = get_number(fp);
	logo_height = get_number(fp);
	maxval = get_number(fp);
	if (maxval != 255)
		die("%s: maximum color value must be 255\n", filename);

	if (!logo_width || !logo_height)
		die("%s: zero sized image\n", filename);
	if ((unsigned long long)logo_width * logo_height > MAX_PIXELS)
		die("%s: image too large\n", filename);

	npixels = logo_width * logo_height;
	logo_data = malloc(npixels);
	if (!logo_data)
		die("%s\n", strerror(errno));

	for (i = 0; i < npixels; i++) {
		struct color color;

		if (raw) {
			color.red = get_byte(fp);
			color.green = get_byte(fp);
			color.blue = get_byte(fp);
		} else {
			color.red = get_number(fp);
			color.green = get_number(fp);
			color.blue = get_number(fp);
		}
		logo_data[i] = find_clut_entry(color);
	}

	fclose(fp);
}

static void write_bytes(const unsigned char *data, unsigned int len,
			const char *indent)
{
	unsigned int i;

	for (i = 0; i < len; i++) {
		if (i % BYTES_PER_LINE == 0)
			fprintf(out, "%s%s", i ? "\n" : "", indent);
		else
			fputc(' ', out);
		fprintf(out, "0x%02x", data[i]);
	}
}

static void write_placement(const char *indent)
{
	fprintf(out, "%s/* Placement. logo-centered and logo-position are exclusive;\n",
		indent);
	fprintf(out, "%s * logo-offset is added after either of the two. */\n",
		indent);

	fprintf(out, "%s%slogo-centered;\n", indent, opt_centered ? "" : "// ");
	fprintf(out, "%s%slogo-position = <%s>;\n", indent,
		opt_position ? "" : "// ", opt_position ? opt_position : "0 0");
	fprintf(out, "%s%slogo-offset = <%s>;\n", indent,
		opt_offset ? "" : "// ", opt_offset ? opt_offset : "0 0");
	fprintf(out, "%s%slogo-rotation = \"%s\";\t/* cw, ccw, ud, none */\n",
		indent, opt_rotation ? "" : "// ",
		opt_rotation ? opt_rotation : "ccw");
}

static void write_node(const char *indent)
{
	char subindent[16];

	snprintf(subindent, sizeof(subindent), "%s\t\t", indent);

	fprintf(out, "%scompatible = \"linux,boot-logo-clut224\";\n", indent);
	fprintf(out, "\n");
	write_placement(indent);
	fprintf(out, "\n");
	fprintf(out, "%swidth = <%u>;\n", indent, logo_width);
	fprintf(out, "%sheight = <%u>;\n", indent, logo_height);
	fprintf(out, "\n");
	fprintf(out, "%sclut = /bits/ 8 <", indent);
	write_bytes((const unsigned char *)logo_clut, logo_clutsize * 3,
		    subindent);
	fprintf(out, ">;\n");
	fprintf(out, "\n");
	fprintf(out, "%sdata = /bits/ 8 <", indent);
	write_bytes(logo_data, logo_width * logo_height, subindent);
	fprintf(out, ">;\n");
}

static void write_header_comment(void)
{
	fprintf(out, "/*\n");
	fprintf(out, " * Boot logo generated by ppmtodtlogo from %s\n",
		filename);
	fprintf(out, " * %ux%u pixels, %u colours.\n", logo_width, logo_height,
		logo_clutsize);
	fprintf(out, " */\n");
}

static void write_dts(void)
{
	fprintf(out, "/dts-v1/;\n/plugin/;\n\n");
	write_header_comment();
	fprintf(out, "\n");
	fprintf(out, "/ {\n");
	fprintf(out, "\tfragment@101 {\n");
	fprintf(out, "\t\ttarget-path = \"/chosen\";\n");
	fprintf(out, "\n");
	fprintf(out, "\t\t__overlay__ {\n");
	fprintf(out, "\t\t\tlogo {\n");
	write_node("\t\t\t\t");
	fprintf(out, "\t\t\t};\n");
	fprintf(out, "\t\t};\n");
	fprintf(out, "\t};\n");
	fprintf(out, "};\n");
}

static void write_dtsi(void)
{
	write_header_comment();
	fprintf(out, "\n");
	fprintf(out, "chosen {\n");
	fprintf(out, "\tlogo {\n");
	write_node("\t\t");
	fprintf(out, "\t};\n");
	fprintf(out, "};\n");
}

static void put_le32(unsigned int val)
{
	fputc(val & 0xff, out);
	fputc((val >> 8) & 0xff, out);
	fputc((val >> 16) & 0xff, out);
	fputc((val >> 24) & 0xff, out);
}

static void write_bin(void)
{
	put_le32(BLOB_MAGIC);
	put_le32(logo_width);
	put_le32(logo_height);
	put_le32(logo_clutsize);
	fwrite(logo_clut, 3, logo_clutsize, out);
	fwrite(logo_data, 1, logo_width * logo_height, out);
}

int main(int argc, char *argv[])
{
	int opt;
	char *p;

	programname = argv[0];

	while ((opt = getopt(argc, argv, "o:t:cp:f:r:h")) != -1) {
		switch (opt) {
		case 'o':
			outputname = optarg;
			break;
		case 't':
			if (!strcmp(optarg, "dts"))
				output_type = OUTPUT_DTS;
			else if (!strcmp(optarg, "dtsi"))
				output_type = OUTPUT_DTSI;
			else if (!strcmp(optarg, "bin"))
				output_type = OUTPUT_BIN;
			else
				usage();
			break;
		case 'c':
			opt_centered = 1;
			break;
		case 'p':
			opt_position = optarg;
			break;
		case 'f':
			opt_offset = optarg;
			break;
		case 'r':
			if (strcmp(optarg, "cw") && strcmp(optarg, "ccw") &&
			    strcmp(optarg, "ud") && strcmp(optarg, "none"))
				usage();
			opt_rotation = optarg;
			break;
		default:
			usage();
		}
	}
	if (optind != argc - 1)
		usage();
	filename = argv[optind];

	/* "10,20" and "10 20" are both accepted for -p and -f */
	for (p = opt_position; p && *p; p++)
		if (*p == ',')
			*p = ' ';
	for (p = opt_offset; p && *p; p++)
		if (*p == ',')
			*p = ' ';

	read_image();

	if (outputname) {
		out = fopen(outputname,
			    output_type == OUTPUT_BIN ? "wb" : "w");
		if (!out)
			die("Cannot create file %s: %s\n", outputname,
			    strerror(errno));
	} else {
		out = stdout;
	}

	switch (output_type) {
	case OUTPUT_DTS:
		write_dts();
		break;
	case OUTPUT_DTSI:
		write_dtsi();
		break;
	case OUTPUT_BIN:
		write_bin();
		break;
	}

	if (outputname)
		fclose(out);

	fprintf(stderr, "%s: %ux%u pixels, %u colours\n", filename, logo_width,
		logo_height, logo_clutsize);

	return 0;
}
