/*
 * This file is part of the sigrok project.
 *
 * Copyright (C) 2010 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2012 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef LIBSIGROKDECODE_SIGROKDECODE_H
#define LIBSIGROKDECODE_SIGROKDECODE_H

#include <Python.h> /* First, so we avoid a _POSIX_C_SOURCE warning. */
#include <stdint.h>
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Status/error codes returned by libsigrokdecode functions.
 *
 * All possible return codes of libsigrokdecode functions must be listed here.
 * Functions should never return hardcoded numbers as status, but rather
 * use these #defines instead. All error codes are negative numbers.
 *
 * The error codes are globally unique in libsigrokdecode, i.e. if one of the
 * libsigrokdecode functions returns a "malloc error" it must be exactly the
 * same return value as used by all other functions to indicate "malloc error".
 * There must be no functions which indicate two different errors via the
 * same return code.
 *
 * Also, for compatibility reasons, no defined return codes are ever removed
 * or reused for different #defines later. You can only add new #defines and
 * return codes, but never remove or redefine existing ones.
 */
#define SRD_OK                 0 /**< No error */
#define SRD_ERR               -1 /**< Generic/unspecified error */
#define SRD_ERR_MALLOC        -2 /**< Malloc/calloc/realloc error */
#define SRD_ERR_ARG           -3 /**< Function argument error */
#define SRD_ERR_BUG           -4 /**< Errors hinting at internal bugs */
#define SRD_ERR_PYTHON        -5 /**< Python C API error */
#define SRD_ERR_DECODERS_DIR  -6 /**< Protocol decoder path invalid */

/* libsigrokdecode loglevels. */
#define SRD_LOG_NONE   0 /**< Output no messages at all. */
#define SRD_LOG_ERR    1 /**< Output error messages. */
#define SRD_LOG_WARN   2 /**< Output warnings. */
#define SRD_LOG_INFO   3 /**< Output informational messages. */
#define SRD_LOG_DBG    4 /**< Output debug messages. */
#define SRD_LOG_SPEW   5 /**< Output very noisy debug messages. */

enum {
	SRD_OUTPUT_ANN,
	SRD_OUTPUT_PROTO,
	SRD_OUTPUT_BINARY,
	/* When adding an output type, don't forget to...
	 * 	  - expose it to PDs in controller.c:PyInit_sigrokdecode()
	 * 	  - add a check in module_sigrokdecode.c:Decoder_put()
	 *    - add a debug string in type_decoder.c:OUTPUT_TYPES
	 */
};

#define SRD_MAX_NUM_PROBES   64

/* TODO: Documentation. */
struct srd_decoder {
	/** The decoder ID. Must be non-NULL and unique for all decoders. */
	char *id;

	/** The (short) decoder name. */
	char *name;

	/** The (long) decoder name. May be NULL. */
	char *longname;

	/** A (short, one-line) description of the decoder. */
	char *desc;

	/** The license of the decoder. Valid values: "gplv2+", "gplv3+". */
	char *license;

	/** TODO */
	GSList *inputformats;

	/** TODO */
	GSList *outputformats;

	/** Probes */
	GSList *probes;

	/** Optional probes */
	GSList *opt_probes;

	/* List of NULL-terminated char[], containing descriptions of the
	 * supported annotation output.
	 */
	GSList *annotations;

	/** Python module */
	PyObject *py_mod;

	/** sigrokdecode.Decoder class */
	PyObject *py_dec;
};

struct srd_probe {
	char *id;
	char *name;
	char *desc;
	int order;
};

struct srd_decoder_instance {
	struct srd_decoder *decoder;
	PyObject *py_instance;
	char *instance_id;
	GSList *pd_output;
	int dec_num_probes;
	int *dec_probemap;
	int data_num_probes;
	int data_unitsize;
	uint64_t data_samplerate;
	GSList *next_di;
};

struct srd_pd_output {
	int pdo_id;
	int output_type;
	struct srd_decoder_instance *di;
	char *proto_id;
};

struct srd_proto_data {
	uint64_t start_sample;
	uint64_t end_sample;
	struct srd_pd_output *pdo;
	int ann_format;
	void *data;
};

struct srd_pd_callback {
	int output_type;
	void (*callback)(struct srd_proto_data *);
};


/* custom python types */
typedef struct {
	PyObject_HEAD
} srd_Decoder;

typedef struct {
	PyObject_HEAD
	struct srd_decoder_instance *di;
	uint64_t start_samplenum;
	unsigned int itercnt;
	uint8_t *inbuf;
	uint64_t inbuflen;
	PyObject *sample;
} srd_logic;


/*--- controller.c ----------------------------------------------------------*/
int srd_init(void);
int srd_exit(void);
int set_modulepath(void);
int srd_instance_set_options(struct srd_decoder_instance *di,
		GHashTable *options);
int srd_instance_set_probes(struct srd_decoder_instance *di,
		GHashTable *probes);
struct srd_decoder_instance *srd_instance_new(const char *id,
		GHashTable *options);
int srd_instance_stack(struct srd_decoder_instance *di_from,
		struct srd_decoder_instance *di_to);
struct srd_decoder_instance *srd_instance_find_by_id(char *instance_id);
struct srd_decoder_instance *srd_instance_find_by_obj(GSList *stack,
		PyObject *obj);
int srd_instance_start(struct srd_decoder_instance *di, PyObject *args);
int srd_instance_decode(uint64_t start_samplenum,
		struct srd_decoder_instance *dec, uint8_t *inbuf, uint64_t inbuflen);
void srd_instance_free(struct srd_decoder_instance *di);
void srd_instance_free_all(GSList *stack);
int srd_session_start(int num_probes, int unitsize, uint64_t samplerate);
int srd_session_feed(uint64_t start_samplenum, uint8_t *inbuf, uint64_t inbuflen);
int pd_add(struct srd_decoder_instance *di, int output_type, char *output_id);
struct srd_decoder_instance *get_di_by_decobject(void *decobject);
typedef void (*srd_pd_output_callback_t)(struct srd_proto_data *pdata);
int srd_register_callback(int output_type, srd_pd_output_callback_t cb);
void *srd_find_callback(int output_type);

/*--- decoder.c -------------------------------------------------------------*/
GSList *srd_list_decoders(void);
struct srd_decoder *srd_get_decoder_by_id(const char *id);
int srd_load_decoder(const char *name, struct srd_decoder **dec);
int srd_unload_decoder(struct srd_decoder *dec);
int srd_load_all_decoders(void);
int srd_unload_all_decoders(void);
char *srd_decoder_doc(struct srd_decoder *dec);

/*--- exception.c -----------------------------------------------------------*/
void catch_exception(const char *format, ...);

/*--- util.c ----------------------------------------------------------------*/
int py_attr_as_str(PyObject *py_obj, const char *attr, char **outstr);
int py_dictitem_as_str(PyObject *py_obj, const char *key, char **outstr);
int py_str_as_str(PyObject *py_str, char **outstr);
int py_strlist_to_char(PyObject *py_strlist, char ***outstr);

/*--- log.c -----------------------------------------------------------------*/
typedef int (*srd_log_handler_t)(void *data, int loglevel, const char *format,
				 va_list args);
int srd_set_loglevel(int loglevel);
int srd_get_loglevel(void);
int srd_log_set_handler(srd_log_handler_t handler, void *data);
int srd_log_set_default_handler(void);
int srd_log_set_logdomain(const char *logdomain);
char *srd_log_get_logdomain(void);

#ifdef __cplusplus
}
#endif

#endif
