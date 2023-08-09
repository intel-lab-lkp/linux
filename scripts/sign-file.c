/* Sign a module file using the given key.
 *
 * Copyright © 2014-2016 Red Hat, Inc. All Rights Reserved.
 * Copyright © 2015      Intel Corporation.
 * Copyright © 2016      Hewlett Packard Enterprise Development LP
 *
 * Authors: David Howells <dhowells@redhat.com>
 *          David Woodhouse <dwmw2@infradead.org>
 *          Juerg Haefliger <juerg.haefliger@hpe.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the licence, or (at your option) any later version.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <err.h>
#include <arpa/inet.h>
#include <openssl/opensslv.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/engine.h>

/*
 * OpenSSL 3.0 deprecates the OpenSSL's ENGINE API.
 *
 * Remove this if/when that API is no longer used
 */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

/*
 * Use CMS if we have openssl-1.0.0 or newer available - otherwise we have to
 * assume that it's not available and its header file is missing and that we
 * should use PKCS#7 instead.  Switching to the older PKCS#7 format restricts
 * the options we have on specifying the X.509 certificate we want.
 *
 * Further, older versions of OpenSSL don't support manually adding signers to
 * the PKCS#7 message so have to accept that we get a certificate included in
 * the signature message.  Nor do such older versions of OpenSSL support
 * signing with anything other than SHA1 - so we're stuck with that if such is
 * the case.
 */
#if defined(LIBRESSL_VERSION_NUMBER) || \
	OPENSSL_VERSION_NUMBER < 0x10000000L || \
	defined(OPENSSL_NO_CMS)
#define USE_PKCS7
#endif
#ifndef USE_PKCS7
#include <openssl/cms.h>
#else
#include <openssl/pkcs7.h>
#endif

struct module_signature {
	uint8_t		algo;		/* Public-key crypto algorithm [0] */
	uint8_t		hash;		/* Digest algorithm [0] */
	uint8_t		id_type;	/* Key identifier type [PKEY_ID_PKCS7] */
	uint8_t		signer_len;	/* Length of signer's name [0] */
	uint8_t		key_id_len;	/* Length of key identifier [0] */
	uint8_t		__pad[3];
	uint32_t	sig_len;	/* Length of signature data */
};

#define PKEY_ID_PKCS7 2

static const char magic_number[] = "~Module signature appended~\n";

static __attribute__((noreturn))
void print_usage(void)
{
	fprintf(stderr, "Usage: scripts/sign-file [OPTIONS]... [MODULE]...\n");
	fprintf(stderr, "Available options:\n");
	fprintf(stderr, "-h, --help             Print this help message and exit\n");

	fprintf(stderr, "\nOptional args:\n");
	fprintf(stderr, "-s, --rawsig <sig>     Raw signature\n");
	fprintf(stderr, "-p, --savesig          Save signature\n");
	fprintf(stderr, "-d, --signonly         Sign only\n");
#ifndef USE_PKCS7
	fprintf(stderr, "-k, --usekeyid         Use key ID\n");
#endif
	fprintf(stderr, "-b, --bulksign         Sign modules in bulk\n");
	fprintf(stderr, "-r, --replaceorig      Replace original\n");
	fprintf(stderr, "-t, --dest <dest>      Destination path ");
	fprintf(stderr, "(Exclusive with bulk option)\n");

	fprintf(stderr, "\nMandatory args:\n");
	fprintf(stderr, "-i, --privkey <key>    Private key\n");
	fprintf(stderr, "-a, --hashalgo <alg>   Hash algorithm\n");
	fprintf(stderr, "-x, --x509 <x509>      X509\n");

	fprintf(stderr, "\nExamples:\n");

	fprintf(stderr, "\n    Regular signing:\n");
	fprintf(stderr, "     scripts/sign-file -a sha512 -i certs/signing_key.pem ");
	fprintf(stderr, "-x certs/signing_key.x509 <module>\n");

	fprintf(stderr, "\n    Signing with destination path:\n");
	fprintf(stderr, "     scripts/sign-file -a sha512 -i certs/signing_key.pem ");
	fprintf(stderr, "-x certs/signing_key.x509 <module> -t <path>\n");

	fprintf(stderr, "\n    Signing modules in bulk:\n");
	fprintf(stderr, "     scripts/sign-file -a sha512 -i certs/signing_key.pem ");
	fprintf(stderr, "-x certs/signing_key.x509 -b <module1> <module2> ...\n");

	exit(2);
}

static void display_openssl_errors(int l)
{
	const char *file;
	char buf[120];
	int e, line;

	if (ERR_peek_error() == 0)
		return;
	fprintf(stderr, "At main.c:%d:\n", l);

	while ((e = ERR_get_error_line(&file, &line))) {
		ERR_error_string(e, buf);
		fprintf(stderr, "- SSL %s: %s:%d\n", buf, file, line);
	}
}

static void drain_openssl_errors(void)
{
	const char *file;
	int line;

	if (ERR_peek_error() == 0)
		return;
	while (ERR_get_error_line(&file, &line)) {}
}

#define ERR(cond, fmt, ...)				\
	do {						\
		bool __cond = (cond);			\
		display_openssl_errors(__LINE__);	\
		if (__cond) {				\
			errx(1, fmt, ## __VA_ARGS__);	\
		}					\
	} while(0)

static const char *key_pass;

static int pem_pw_cb(char *buf, int len, int w, void *v)
{
	int pwlen;

	if (!key_pass)
		return -1;

	pwlen = strlen(key_pass);
	if (pwlen >= len)
		return -1;

	strcpy(buf, key_pass);

	/* If it's wrong, don't keep trying it. */
	key_pass = NULL;

	return pwlen;
}

static EVP_PKEY *read_private_key(const char *private_key_name)
{
	EVP_PKEY *private_key;

	if (!strncmp(private_key_name, "pkcs11:", 7)) {
		ENGINE *e;

		ENGINE_load_builtin_engines();
		drain_openssl_errors();
		e = ENGINE_by_id("pkcs11");
		ERR(!e, "Load PKCS#11 ENGINE");
		if (ENGINE_init(e))
			drain_openssl_errors();
		else
			ERR(1, "ENGINE_init");
		if (key_pass)
			ERR(!ENGINE_ctrl_cmd_string(e, "PIN", key_pass, 0),
			    "Set PKCS#11 PIN");
		private_key = ENGINE_load_private_key(e, private_key_name,
						      NULL, NULL);
		ERR(!private_key, "%s", private_key_name);
	} else {
		BIO *b;

		b = BIO_new_file(private_key_name, "rb");
		ERR(!b, "%s", private_key_name);
		private_key = PEM_read_bio_PrivateKey(b, NULL, pem_pw_cb,
						      NULL);
		ERR(!private_key, "%s", private_key_name);
		BIO_free(b);
	}

	return private_key;
}

static X509 *read_x509(const char *x509_name)
{
	unsigned char buf[2];
	X509 *x509;
	BIO *b;
	int n;

	b = BIO_new_file(x509_name, "rb");
	ERR(!b, "%s", x509_name);

	/* Look at the first two bytes of the file to determine the encoding */
	n = BIO_read(b, buf, 2);
	if (n != 2) {
		if (BIO_should_retry(b)) {
			fprintf(stderr, "%s: Read wanted retry\n", x509_name);
			exit(1);
		}
		if (n >= 0) {
			fprintf(stderr, "%s: Short read\n", x509_name);
			exit(1);
		}
		ERR(1, "%s", x509_name);
	}

	ERR(BIO_reset(b) != 0, "%s", x509_name);

	if (buf[0] == 0x30 && buf[1] >= 0x81 && buf[1] <= 0x84)
		/* Assume raw DER encoded X.509 */
		x509 = d2i_X509_bio(b, NULL);
	else
		/* Assume PEM encoded X.509 */
		x509 = PEM_read_bio_X509(b, NULL, NULL, NULL);

	BIO_free(b);
	ERR(!x509, "%s", x509_name);

	return x509;
}

struct cmd_opts {
	char *raw_sig_name;
	char *hash_algo;
	char *dest_name;
	char *private_key_name;
	char *x509_name;
	char *module_name;
	bool save_sig;
	bool replace_orig;
	bool raw_sig;
	bool sign_only;
	bool bulk_sign;
#ifndef USE_PKCS7
	unsigned int use_keyid;
#endif
};

static void parse_args(int argc, char **argv, struct cmd_opts *opts)
{
	struct option cmd_options[] = {
		{"rawsig",	required_argument,  0,	's'},
		{"savesig",	no_argument,	    0,	'p'},
		{"signonly",	no_argument,	    0,	'd'},
#ifndef USE_PKCS7
		{"usekeyid",	no_argument,	    0,	'k'},
#endif
		{"help",	no_argument,	    0,	'h'},
		{"privkey",	required_argument,  0,	'i'},
		{"hashalgo",	required_argument,  0,	'a'},
		{"x509",	required_argument,  0,	'x'},
		{"dest",	required_argument,  0,	'd'},
		{"replaceorig",	required_argument,  0,	'r'},
		{0, 0, 0, 0}
	};

	int opt;
	int opt_index = 0;

	do {
#ifndef USE_PKCS7
		opt = getopt_long_only(argc, argv, "hpdbs:i:a:x:t:r:",
				cmd_options, &opt_index);
#else
		opt = getopt_long_only(argc, argv, "hpdkbs:i:a:x:t:r:",
				cmd_options, &opt_index);
#endif
		switch (opt) {
		case 's':
			opts->raw_sig = true;
			opts->raw_sig_name = optarg;
			break;

		case 'p':
			opts->save_sig = true;
			break;

		case 'd':
			opts->sign_only = true;
			opts->save_sig = true;
			break;

#ifndef USE_PKCS7
		case 'k':
			opts->use_keyid = CMS_USE_KEYID;
			break;
#endif

		case 'h':
			print_usage();
			break;

		case 'i':
			opts->private_key_name = optarg;
			break;

		case 'a':
			opts->hash_algo = optarg;
			break;

		case 'x':
			opts->x509_name = optarg;
			break;

		case 't':
			opts->dest_name = optarg;
			break;

		case 'r':
			opts->replace_orig = true;
			break;

		case 'b':
			opts->bulk_sign = true;
			break;

		case -1:
			break;

		default:
			print_usage();
			break;
		}
	} while (opt != -1);
}

static int sign_single_file(struct cmd_opts *opts)
{
	struct module_signature sig_info = { .id_type = PKEY_ID_PKCS7 };
	unsigned char buf[4096] = {};
	unsigned long module_size, sig_size;
	unsigned int use_signed_attrs;
	const EVP_MD *digest_algo;
	EVP_PKEY *private_key;
#ifndef USE_PKCS7
	CMS_ContentInfo *cms = NULL;
#else
	PKCS7 *pkcs7 = NULL;
#endif
	X509 *x509;
	BIO *bd, *bm;
	int n;

	key_pass = getenv("KBUILD_SIGN_PIN");

#ifndef USE_PKCS7
	use_signed_attrs = CMS_NOATTR;
#else
	use_signed_attrs = PKCS7_NOATTR;
#endif

#ifdef USE_PKCS7
	if (strcmp(hash_algo, "sha1") != 0) {
		fprintf(stderr, "sign-file: %s only supports SHA1 signing\n",
			OPENSSL_VERSION_TEXT);
		exit(3);
	}
#endif

	/* Open the module file */
	bm = BIO_new_file(opts->module_name, "rb");
	ERR(!bm, "%s", opts->module_name);

	if (!opts->raw_sig) {
		/* Read the private key and the X.509 cert the PKCS#7 message
		 * will point to.
		 */
		private_key = read_private_key(opts->private_key_name);
		x509 = read_x509(opts->x509_name);

		/* Digest the module data. */
		OpenSSL_add_all_digests();
		display_openssl_errors(__LINE__);
		digest_algo = EVP_get_digestbyname(opts->hash_algo);
		ERR(!digest_algo, "EVP_get_digestbyname");

#ifndef USE_PKCS7
		/* Load the signature message from the digest buffer. */
		cms = CMS_sign(NULL, NULL, NULL, NULL,
			       CMS_NOCERTS | CMS_PARTIAL | CMS_BINARY |
			       CMS_DETACHED | CMS_STREAM);
		ERR(!cms, "CMS_sign");

		ERR(!CMS_add1_signer(cms, x509, private_key, digest_algo,
				     CMS_NOCERTS | CMS_BINARY |
				     CMS_NOSMIMECAP | opts->use_keyid |
				     use_signed_attrs),
		    "CMS_add1_signer");
		ERR(CMS_final(cms, bm, NULL, CMS_NOCERTS | CMS_BINARY) < 0,
		    "CMS_final");

#else
		pkcs7 = PKCS7_sign(x509, private_key, NULL, bm,
				   PKCS7_NOCERTS | PKCS7_BINARY |
				   PKCS7_DETACHED | use_signed_attrs);
		ERR(!pkcs7, "PKCS7_sign");
#endif

		if (opts->save_sig) {
			char *sig_file_name;
			BIO *b;

			ERR(asprintf(&sig_file_name, "%s.p7s", opts->module_name) < 0,
			    "asprintf");
			b = BIO_new_file(sig_file_name, "wb");
			ERR(!b, "%s", sig_file_name);
#ifndef USE_PKCS7
			ERR(i2d_CMS_bio_stream(b, cms, NULL, 0) < 0,
			    "%s", sig_file_name);
#else
			ERR(i2d_PKCS7_bio(b, pkcs7) < 0,
			    "%s", sig_file_name);
#endif
			BIO_free(b);
		}

		if (opts->sign_only) {
			BIO_free(bm);
			return 0;
		}
	}

	/* Open the destination file now so that we can shovel the module data
	 * across as we read it.
	 */
	bd = BIO_new_file(opts->dest_name, "wb");
	ERR(!bd, "%s", opts->dest_name);

	/* Append the marker and the PKCS#7 message to the destination file */
	ERR(BIO_reset(bm) < 0, "%s", opts->module_name);
	while ((n = BIO_read(bm, buf, sizeof(buf))),
	       n > 0) {
		ERR(BIO_write(bd, buf, n) < 0, "%s", opts->dest_name);
	}
	BIO_free(bm);
	ERR(n < 0, "%s", opts->module_name);
	module_size = BIO_number_written(bd);

	if (!opts->raw_sig) {
#ifndef USE_PKCS7
		ERR(i2d_CMS_bio_stream(bd, cms, NULL, 0) < 0, "%s", opts->dest_name);
#else
		ERR(i2d_PKCS7_bio(bd, pkcs7) < 0, "%s", opts->dest_name);
#endif
	} else {
		BIO *b;

		/* Read the raw signature file and write the data to the
		 * destination file
		 */
		b = BIO_new_file(opts->raw_sig_name, "rb");
		ERR(!b, "%s", opts->raw_sig_name);
		while ((n = BIO_read(b, buf, sizeof(buf))), n > 0)
			ERR(BIO_write(bd, buf, n) < 0, "%s", opts->dest_name);
		BIO_free(b);
	}

	sig_size = BIO_number_written(bd) - module_size;
	sig_info.sig_len = htonl(sig_size);
	ERR(BIO_write(bd, &sig_info, sizeof(sig_info)) < 0, "%s", opts->dest_name);
	ERR(BIO_write(bd, magic_number, sizeof(magic_number) - 1) < 0, "%s", opts->dest_name);

	ERR(BIO_free(bd) < 0, "%s", opts->dest_name);

	/* Finally, if we're signing in place, replace the original. */
	if (opts->replace_orig)
		ERR(rename(opts->dest_name, opts->module_name) < 0, "%s", opts->dest_name);

	return 0;
}

int main(int argc, char **argv)
{
	int i;
	struct cmd_opts opts = {};

	parse_args(argc, argv, &opts);
	argc -= optind;
	argv += optind;

	if ((opts.bulk_sign && opts.dest_name) || (!opts.bulk_sign && argc != 1))
		print_usage();

	OpenSSL_add_all_algorithms();
	ERR_load_crypto_strings();
	ERR_clear_error();

	for (i = 0; i < argc; ++i) {
		opts.module_name = argv[i];

		if (!opts.bulk_sign && opts.dest_name && strcmp(argv[i], opts.dest_name)) {
			opts.replace_orig = false;
		} else {
			ERR(asprintf(&opts.dest_name, "%s.~signed~", opts.module_name) < 0,
				     "asprintf");
			if (!opts.replace_orig)
				opts.replace_orig = true;
		}

		if (sign_single_file(&opts)) {
			fprintf(stderr, "Failed to sign: %s module\n", opts.module_name);
			return -1;
		}
	}

	return 0;
}
