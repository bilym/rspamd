/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"
#include "printf.h"
#include "message.h"
#include "util.h"
#include "content_type.h"

static gdouble total_time = 0;
static gint total_parsed = 0;
static gint total_valid = 0;
static gint total_type = 0;
static gint total_subtype = 0;
static gint total_charset = 0;
static gint total_attrs = 0;
static gboolean verbose = 1;

#define MODE_NORMAL 0
#define MODE_GMIME 1
#define MODE_COMPARE 2

static void
rspamd_process_file (const gchar *fname, gint mode)
{
	rspamd_mempool_t *pool;
	GIOChannel *f;
	GError *err = NULL;
	GString *buf;
	struct rspamd_content_type *ct;
	gdouble t1, t2;
	GMimeContentType *gct;
	rspamd_ftok_t t;

	f = g_io_channel_new_file (fname, "r", &err);

	if (!f) {
		rspamd_fprintf (stderr, "cannot open %s: %e\n", fname, err);
		g_error_free (err);

		return;
	}

	g_io_channel_set_encoding (f, NULL, NULL);
	buf = g_string_sized_new (8192);
	pool = rspamd_mempool_new (rspamd_mempool_suggest_size (), "test");

	while (g_io_channel_read_line_string (f, buf, NULL, &err)
			== G_IO_STATUS_NORMAL) {

		while (buf->len > 0 && g_ascii_isspace (buf->str[buf->len - 1])) {
			buf->len --;
		}

		if (mode == MODE_NORMAL) {
			t1 = rspamd_get_virtual_ticks ();
			ct = rspamd_content_type_parse (buf->str, buf->len, pool);
			t2 = rspamd_get_virtual_ticks ();
		}
		else if (mode == MODE_GMIME) {
			t1 = rspamd_get_virtual_ticks ();
			gct = g_mime_content_type_new_from_string (buf->str);
			t2 = rspamd_get_virtual_ticks ();
		}
		else {
			t1 = rspamd_get_virtual_ticks ();
			ct = rspamd_content_type_parse (buf->str, buf->len, pool);
			gct = g_mime_content_type_new_from_string (buf->str);
			t2 = rspamd_get_virtual_ticks ();
		}

		total_time += t2 - t1;
		total_parsed ++;

		if (mode == MODE_NORMAL) {

			if (ct) {
				total_valid ++;

				if (ct->type.len > 0) {
					total_type ++;
				}
				if (ct->subtype.len > 0) {
					total_subtype ++;
				}
				if (ct->charset.len > 0) {
					total_charset ++;
				}
				if (ct->attrs) {
					total_attrs ++;
				}
			}
		}
		else if (mode == MODE_GMIME) {
			if (gct) {
				total_valid ++;

				if (g_mime_content_type_get_media_type (gct)) {
					total_type ++;
				}
				if (g_mime_content_type_get_media_subtype (gct)) {
					total_subtype ++;
				}
				if (g_mime_content_type_get_parameter (gct, "charset")) {
					total_charset ++;
				}
				if (g_mime_content_type_get_params (gct)) {
					total_attrs ++;
				}

				g_object_unref (gct);
			}
		}
		else {
			if (gct && ct) {
				total_valid ++;

				if (g_mime_content_type_get_media_type (gct) && ct->type.len) {
					t.begin = g_mime_content_type_get_media_type (gct);
					t.len = strlen (t.begin);

					if (rspamd_ftok_casecmp (&ct->type, &t) == 0) {
						total_type ++;
					}
					else if (verbose) {
						rspamd_fprintf (stderr, "type: '%*s'(rspamd) '%s'gmime\n",
								(gint)ct->type.len, ct->type.begin,
								t.begin);
					}
				}
				if (g_mime_content_type_get_media_subtype (gct) && ct->subtype.len) {
					t.begin = g_mime_content_type_get_media_subtype (gct);
					t.len = strlen (t.begin);

					if (rspamd_ftok_casecmp (&ct->subtype, &t) == 0) {
						total_subtype ++;
					}
					else if (verbose) {
						rspamd_fprintf (stderr, "subtype: '%*s'(rspamd) '%s'gmime\n",
								(gint)ct->subtype.len, ct->subtype.begin,
								t.begin);
					}
				}
				if (g_mime_content_type_get_parameter (gct, "charset") && ct->charset.len) {
					t.begin = g_mime_content_type_get_parameter (gct, "charset");
					t.len = strlen (t.begin);

					if (rspamd_ftok_casecmp (&ct->charset, &t) == 0) {
						total_charset ++;
					}
					else if (verbose) {
						rspamd_fprintf (stderr, "charset: '%*s'(rspamd) '%s'gmime\n",
								(gint)ct->charset.len, ct->charset.begin,
								t.begin);
					}
				}
			}
			else if (verbose) {
				rspamd_fprintf (stderr, "cannot parse: %v, %d(rspamd), %d(gmime)\n",
						buf, ct ? 1 : 0, gct ? 1 : 0);
			}

			if (gct) {
				g_object_unref (gct);
			}
		}
	}

	if (err) {
		rspamd_fprintf (stderr, "cannot read %s: %e\n", fname, err);
		g_error_free (err);
	}

	g_io_channel_unref (f);
	g_string_free (buf, TRUE);
	rspamd_mempool_delete (pool);
}

int
main (int argc, char **argv)
{
	gint i, start = 1, mode = MODE_NORMAL;

	g_mime_init (0);

	if (argc > 2 && *argv[1] == '-') {
		start = 2;

		if (argv[1][1] == 'g') {
			mode = MODE_GMIME;
		}
		else if (argv[1][1] == 'c') {
			mode = MODE_COMPARE;
		}
	}

	for (i = start; i < argc; i ++) {
		if (argv[i]) {
			rspamd_process_file (argv[i], mode);
		}
	}

	if (mode != MODE_COMPARE) {
		rspamd_printf ("Parsed %d received headers in %.3f seconds\n"
				"Total valid (has type): %d\n"
				"Total known type: %d\n"
				"Total known subtype: %d\n"
				"Total known charset: %d\n"
				"Total has attrs: %d\n",
				total_parsed, total_time,
				total_valid, total_type,
				total_subtype, total_charset,
				total_attrs);
	}
	else {
		rspamd_printf ("Parsed %d received headers in %.3f seconds\n"
				"Total valid (parsed by both): %d\n"
				"Total same type: %d\n"
				"Total same subtype: %d\n"
				"Total same charset: %d\n",
				total_parsed, total_time,
				total_valid, total_type,
				total_subtype, total_charset);
	}

	g_mime_shutdown ();

	return 0;
}
