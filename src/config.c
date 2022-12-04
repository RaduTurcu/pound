/*
 * Pound - the reverse-proxy load-balancer
 * Copyright (C) 2002-2010 Apsis GmbH
 * Copyright (C) 2018-2022 Sergey Poznyakoff
 *
 * Pound is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Pound is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pound.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pound.h"
#include "extern.h"
#include <openssl/x509v3.h>
#include <assert.h>

char *progname;

/*
 * Scanner
 */

/* Token types: */
enum
  {
    T__BASE = 256,
    T_IDENT = T__BASE, /* Identifier */
    T_NUMBER,          /* Decimal number */
    T_STRING,          /* Quoted string */
    T_LITERAL,         /* Unquoted literal */
    T__END,
    T_ERROR = T__END,  /* Erroneous or malformed token */
  };

typedef unsigned TOKENMASK;

#define T_BIT(t) ((TOKENMASK)1<<((t)-T__BASE))
#define T_MASK_ISSET(m,t) ((m) & T_BIT(t))
#define T_ANY 0 /* any token, inlcuding newline */
/* Unquoted character sequence */
#define T_UNQ (T_BIT (T_IDENT) | T_BIT (T_NUMBER) | T_BIT(T_LITERAL))

/* Locations in the source file */
struct locus_point
{
  char const *filename;
  int line;
  int col;
};

struct locus_range
{
  struct locus_point beg, end;
};

/* Token structure */
struct token
{
  int type;
  char *str;
  struct locus_range locus;
};

/*
 * Return a static string describing the given type.
 * Note, that in addition to the types defined above, this function returns
 * meaningful description for all possible ASCII characters.
 */
static char const *
token_type_str (unsigned type)
{
  static char buf[6];
  switch (type)
    {
    case T_IDENT:
      return "identifier";

    case T_STRING:
      return "quoted string";

    case T_NUMBER:
      return "number";

    case T_LITERAL:
      return "literal";

    case '\n':
      return "end of line";

    case '\t':
      return "'\\t'";

    case '\\':
      return "'\\'";

    case '\"':
      return "'\"'";
    }

  if (isprint (type))
    {
      buf[0] = buf[2] = '\'';
      buf[1] = type;
      buf[3] = 0;
    }
  else if (iscntrl (type))
    {
      buf[0] = '^';
      buf[1] = type ^ 0100;
      buf[2] = 0;
    }
  else
    {
      buf[5] = 0;
      buf[4] = (type & 7) + '0';
      type >>= 3;
      buf[3] = (type & 7) + '0';
      type >>= 3;
      buf[2] = (type & 7) + '0';
      buf[1] = '0';
      buf[0] = '\\';
    }
  return buf;
}

static size_t
token_mask_str (TOKENMASK mask, char *buf, size_t size)
{
  unsigned i = 0;
  char *q = buf, *end = buf + size - 1;

  for (i = T__BASE; i < T__END; i++)
    {
      if (mask & T_BIT (i))
	{
	  char const *s;

	  mask &= ~T_BIT (i);
	  if (q > buf)
	    {
	      if (mask)
		{
		  if (end - q <= 2)
		    break;
		  *q++ = ',';
		  *q++ = ' ';
		}
	      else
		{
		  if (end - q <= 4)
		    break;
		  strcpy (q, " or ");
		  q += 4;
		}
	    }
	  s = token_type_str (i);
	  while (*s && q < end)
	    {
	      *q++ = *s++;
	    }
	}
    }
  *q = 0;
  return q - buf;
}

/*
 * Buffer size for token buffer used as input to token_mask_str.  This takes
 * into account only T_.* types above, as returned by token_type_str.
 *
 * Be sure to update this constant if you change anything above.
 */
#define MAX_TOKEN_BUF_SIZE 45


struct kwtab
{
  char const *name;
  int tok;
};

static int
kw_to_tok (struct kwtab *kwt, char const *name, int ci, int *retval)
{
  for (; kwt->name; kwt++)
    if ((ci ? strcasecmp : strcmp) (kwt->name, name) == 0)
      {
	*retval = kwt->tok;
	return 0;
      }
  return -1;
}

/* Input stream */
struct input
{
  struct input *prev;             /* Previous input in stack. */

  FILE *file;                     /* Input file. */
  ino_t ino;
  dev_t devno;

  struct locus_point locus;       /* Current location */
  int prev_col;                   /* Last column in the previous line. */
  struct token token;             /* Current token. */
  int ready;                      /* Token already parsed and put back */

  /* Input buffer: */
  struct stringbuf buf;
};

void *
mem2nrealloc (void *p, size_t *pn, size_t s)
{
  size_t n = *pn;
  char *newp;

  if (!p)
    {
      if (!n)
	{
	  /* The approximate size to use for initial small allocation
	     requests, when the invoking code  specifies an old size of zero.
	     64 bytes is the largest "small" request for the
	     GNU C library malloc.  */
	  enum { DEFAULT_MXFAST = 64 };

	  n = DEFAULT_MXFAST / s;
	  n += !n;
	}
    }
  else
    {
      /* Set N = ceil (1.5 * N) so that progress is made if N == 1.
	 Check for overflow, so that N * S stays in size_t range.
	 The check is slightly conservative, but an exact check isn't
	 worth the trouble.  */
      if ((size_t) -1 / 3 * 2 / s <= n)
	{
	  errno = ENOMEM;
	  return NULL;
	}
      n += (n + 1) / 2;
    }

  newp = realloc(p, n * s);
  if (!newp)
    return NULL;
  *pn = n;
  return newp;
}

void
xnomem (void)
{
  logmsg (LOG_CRIT, "out of memory");
  exit (1);
}

void *
xmalloc (size_t s)
{
  void *p = malloc (s);
  if (p == NULL)
    xnomem ();
  return p;
}

void *
xcalloc (size_t nmemb, size_t size)
{
  void *p = calloc (nmemb, size);
  if (!p)
    xnomem ();
  return p;
}

void *
xrealloc (void *p, size_t s)
{
  if ((p = realloc (p, s)) == NULL)
    xnomem ();
  return p;
}

void *
x2nrealloc (void *p, size_t *pn, size_t s)
{
  if ((p = mem2nrealloc (p, pn, s)) == NULL)
    xnomem ();
  return p;
}

char *
xstrdup (char const *s)
{
  char *p = strdup (s);
  if (!p)
    xnomem ();
  return p;
}

char *
xstrndup (const char *s, size_t n)
{
  char *p = strndup (s, n);
  if (!p)
    xnomem ();
  return p;
}

void
stringbuf_init (struct stringbuf *sb)
{
  memset (sb, 0, sizeof (*sb));
}

void
stringbuf_reset (struct stringbuf *sb)
{
  sb->len = 0;
}

void
stringbuf_free (struct stringbuf *sb)
{
  free (sb->base);
}

void
stringbuf_add_char (struct stringbuf *sb, int c)
{
  if (sb->len == sb->size)
    sb->base = x2nrealloc (sb->base, &sb->size, 1);
  sb->base[sb->len++] = c;
}

void
stringbuf_add (struct stringbuf *sb, char const *str, size_t len)
{
  while (sb->len + len > sb->size)
    sb->base = x2nrealloc (sb->base, &sb->size, 1);
  memcpy (sb->base + sb->len, str, len);
  sb->len += len;
}

void
stringbuf_add_string (struct stringbuf *sb, char const *str)
{
  int c;

  while ((c = *str++) != 0)
    stringbuf_add_char (sb, c);
}

char *
stringbuf_finish (struct stringbuf *sb)
{
  stringbuf_add_char (sb, 0);
  return sb->base;
}

void
stringbuf_vprintf (struct stringbuf *sb, char const *fmt, va_list ap)
{
  for (;;)
    {
      size_t bufsize = sb->size - sb->len;
      ssize_t n;
      va_list aq;

      va_copy (aq, ap);
      n = vsnprintf (sb->base + sb->len, bufsize, fmt, aq);
      va_end (aq);

      if (n < 0 || n >= bufsize || !memchr (sb->base + sb->len, '\0', n + 1))
	sb->base = x2nrealloc (sb->base, &sb->size, 1);
      else
	{
	  sb->len += n;
	  break;
	}
    }
}

void
stringbuf_printf (struct stringbuf *sb, char const *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  stringbuf_vprintf (sb, fmt, ap);
  va_end (ap);
}

static void
stringbuf_format_locus_point (struct stringbuf *sb, struct locus_point const *loc)
{
  stringbuf_printf (sb, "%s:%d", loc->filename, loc->line);
  if (loc->col)
    stringbuf_printf (sb, ".%d", loc->col);
}

static int
same_file (struct locus_point const *a, struct locus_point const *b)
{
  return a->filename == b->filename
	 || (a->filename && b->filename && strcmp (a->filename, b->filename) == 0);
}

static void
stringbuf_format_locus_range (struct stringbuf *sb, struct locus_range const *range)
{
  stringbuf_format_locus_point (sb, &range->beg);
  if (range->end.filename)
    {
      if (!same_file (&range->beg, &range->end))
	{
	  stringbuf_add_char (sb, '-');
	  stringbuf_format_locus_point (sb, &range->end);
	}
      else if (range->beg.line != range->end.line)
	{
	  stringbuf_add_char (sb, '-');
	  stringbuf_printf (sb, "%d", range->end.line);
	  if (range->end.col)
	    stringbuf_printf (sb, ".%d", range->end.col);
	}
      else if (range->beg.col && range->beg.col != range->end.col)
	{
	  stringbuf_add_char (sb, '-');
	  stringbuf_printf (sb, "%d", range->end.col);
	}
    }
}

static void
vconf_error_at_locus_range (struct locus_range const *loc, char const *fmt, va_list ap)
{
  struct stringbuf sb;

  stringbuf_init (&sb);
  if (loc)
    {
      stringbuf_format_locus_range (&sb, loc);
      stringbuf_add_string (&sb, ": ");
    }
  stringbuf_vprintf (&sb, fmt, ap);
  logmsg (LOG_ERR, "%s", sb.base);
  stringbuf_free (&sb);
}

static void
conf_error_at_locus_range (struct locus_range const *loc, char const *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  vconf_error_at_locus_range (loc, fmt, ap);
  va_end (ap);
}

static void
vconf_error_at_locus_point (struct locus_point const *loc, char const *fmt, va_list ap)
{
  struct stringbuf sb;

  stringbuf_init (&sb);
  if (loc)
    {
      stringbuf_format_locus_point (&sb, loc);
      stringbuf_add_string (&sb, ": ");
    }
  stringbuf_vprintf (&sb, fmt, ap);
  logmsg (LOG_ERR, "%s", sb.base);
  stringbuf_free (&sb);
}

static void
conf_error_at_locus_point (struct locus_point const *loc, char const *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  vconf_error_at_locus_point (loc, fmt, ap);
  va_end (ap);
}

static void
regcomp_error_at_locus_range (struct locus_range const *loc, int rc, regex_t *rx,
			      char const *expr)
{
  char errbuf[512];
  regerror (rc, rx, errbuf, sizeof (errbuf));
  conf_error_at_locus_range (loc, "%s", errbuf);
  if (expr)
    conf_error_at_locus_range (loc, "regular expression: %s", expr);
}

static void
openssl_error_at_locus_range (struct locus_range const *loc, char const *msg)
{
  unsigned long n = ERR_get_error ();
  conf_error_at_locus_range (loc, "%s: %s", msg, ERR_error_string (n, NULL));

  if ((n = ERR_get_error ()) != 0)
    {
      do
	{
	  conf_error_at_locus_range (loc, "%s", ERR_error_string (n, NULL));
	}
      while ((n = ERR_get_error ()) != 0);
    }
}

struct name_list
{
  struct name_list *next;
  char name[1];
};

static struct name_list *name_list;

static char const *
name_alloc (char const *name)
{
  struct name_list *np;
  np = xmalloc (sizeof (*np) + strlen (name));
  strcpy (np->name, name);
  np->next = name_list;
  name_list = np;
  return np->name;
}

static void
name_list_free (void)
{
  while (name_list)
    {
      struct name_list *next = name_list->next;
      free (name_list);
      name_list = next;
    }
}

/*
 * Input scanner.
 */
static struct input *
input_close (struct input *input)
{
  struct input *prev = NULL;
  if (input)
    {
      prev = input->prev;
      fclose (input->file);
      stringbuf_free (&input->buf);
      free (input);
    }
  return prev;
}

static struct input *
input_open (char const *filename, struct stat *st)
{
  struct input *input;

  input = xmalloc (sizeof (*input));
  memset (input, 0, sizeof (*input));
  if ((input->file = fopen (filename, "r")) == 0)
    {
      logmsg (LOG_ERR, "can't open %s: %s", filename, strerror (errno));
      free (input);
      return NULL;
    }
  input->ino = st->st_ino;
  input->devno = st->st_dev;
  input->locus.filename = name_alloc (filename);
  input->locus.line = 1;
  input->locus.col = 0;
  return input;
}

static inline int
input_getc (struct input *input)
{
  int c = fgetc (input->file);
  if (c == '\n')
    {
      input->locus.line++;
      input->prev_col = input->locus.col;
      input->locus.col = 0;
    }
  else if (c == '\t')//FIXME
    input->locus.col += 8;
  else if (c != EOF)
    input->locus.col++;
  return c;
}

static void
input_ungetc (struct input *input, int c)
{
  if (c != EOF)
    {
      ungetc (c, input->file);
      if (c == '\n')
	{
	  input->locus.line--;
	  input->locus.col = input->prev_col;
	}
      else
	input->locus.col--;
    }
}

#define is_ident_start(c) (isalpha (c) || c == '_')
#define is_ident_cont(c) (is_ident_start (c) || isdigit (c))

int
input_gettkn (struct input *input, struct token **tok)
{
  int c;

  if (input->ready)
    {
      input->ready = 0;
      *tok = &input->token;
      return input->token.type;
    }

  stringbuf_reset (&input->buf);
  for (;;)
    {
      c = input_getc (input);

      if (c == EOF)
	{
	  input->token.type = c;
	  break;
	}

      if (c == '#')
	{
	  while ((c = input_getc (input)) != '\n')
	    if (c == EOF)
	      {
		input->token.type = c;
		goto end;
	      }
	  /* return newline */
	}

      if (c == '\n')
	{
	  input->token.locus.beg = input->locus;
	  input->token.locus.beg.line--;
	  input->token.locus.beg.col = input->prev_col;
	  input->token.type = c;
	  break;
	}

      if (isspace (c))
	continue;

      input->token.locus.beg = input->locus;
      if (c == '"')
	{
	  while ((c = input_getc (input)) != '"')
	    {
	      if (c == '\\')
		{
		  c = input_getc (input);
		  if (!(c == EOF || c == '"' || c == '\\'))
		    {
		      conf_error_at_locus_point (&input->locus,
						 "unrecognized escape character");
		    }
		}
	      if (c == EOF)
		{
		  conf_error_at_locus_point (&input->locus,
					     "end of file in quoted string");
		  input->token.type = T_ERROR;
		  goto end;
		}
	      if (c == '\n')
		{
		  conf_error_at_locus_point (&input->locus,
					     "end of line in quoted string");
		  input->token.type = T_ERROR;
		  goto end;
		}
	      stringbuf_add_char (&input->buf, c);
	    }
	  stringbuf_add_char (&input->buf, 0);
	  input->token.type = T_STRING;
	  input->token.str = input->buf.base;
	  break;
	}

      if (is_ident_start (c))
	{
	  do
	    {
	      stringbuf_add_char (&input->buf, c);
	    }
	  while ((c = input_getc (input)) != EOF && is_ident_cont (c));
	  if (c == EOF || isspace (c))
	    {
	      input_ungetc (input, c);
	      stringbuf_add_char (&input->buf, 0);
	      input->token.type = T_IDENT;
	      input->token.str = input->buf.base;
	      break;
	    }
	  /* It is a literal */
	}

      if (isdigit (c))
	input->token.type = T_NUMBER;
      else
	input->token.type = T_LITERAL;

      do
	{
	  stringbuf_add_char (&input->buf, c);
	  if (!isdigit (c))
	    input->token.type = T_LITERAL;
	}
      while ((c = input_getc (input)) != EOF && !isspace (c));

      input_ungetc (input, c);
      stringbuf_add_char (&input->buf, 0);
      input->token.str = input->buf.base;
      break;
    }
 end:
  input->token.locus.end = input->locus;
  *tok = &input->token;
  return input->token.type;
}

static void
input_putback (struct input *input)
{
  assert (input->ready == 0);
  input->ready = 1;
}


struct input *cur_input;

static struct locus_range *
last_token_locus_range (void)
{
  if (cur_input)
    return &cur_input->token.locus;
  else
    return NULL;
}

static struct locus_point *
current_locus_point (void)
{
  if (cur_input)
    return &cur_input->locus;
  else
    return NULL;
}

#define conf_error(fmt, ...) \
  conf_error_at_locus_range (last_token_locus_range (), fmt, __VA_ARGS__)

#define conf_regcomp_error(rc, rx, expr) \
  regcomp_error_at_locus_range (last_token_locus_range (), rc, rx, expr)

#define conf_openssl_error(msg) \
  openssl_error_at_locus_range (last_token_locus_range (), msg)

static int
push_input (const char *filename)
{
  struct stat st;
  struct input *input;

  if (stat (filename, &st))
    {
      conf_error ("can't stat %s: %s", filename, strerror (errno));
      return -1;
    }

  for (input = cur_input; input; input = input->prev)
    {
      if (input->ino == st.st_ino && input->devno == st.st_dev)
	{
	  if (input->prev)
	    {
	      conf_error ("%s already included", filename);
	      conf_error_at_locus_point (&input->prev->locus, "here is the place of original inclusion");
	    }
	  else
	    {
	      conf_error ("%s already included (at top level)", filename);
	    }
	  return -1;
	}
    }

  if ((input = input_open (filename, &st)) == NULL)
    return -1;

  input->prev = cur_input;
  cur_input = input;

  return 0;
}

static void
pop_input (void)
{
  cur_input = input_close (cur_input);
}

static int
gettkn (struct token **tok)
{
  int t;

  while (cur_input && (t = input_gettkn (cur_input, tok)) == EOF)
    pop_input ();
  return t;
}

static struct token *
gettkn_expect_mask (int expect)
{
  struct token *tok;
  int type = gettkn (&tok);

  if (type == EOF)
    {
      conf_error ("%s", "unexpected end of file");
      tok = NULL;
    }
  else if (type == T_ERROR)
    {
      /* error message already issued */
      tok = NULL;
    }
  else if (expect == 0)
    /* any token is accepted */;
  else if (!T_MASK_ISSET (expect, type))
    {
      char buf[MAX_TOKEN_BUF_SIZE];
      token_mask_str (expect, buf, sizeof (buf));
      conf_error ("expected %s, but found %s", buf, token_type_str (tok->type));
      tok = NULL;
    }
  return tok;
}

static struct token *
gettkn_any (void)
{
  return gettkn_expect_mask (T_ANY);
}

static struct token *
gettkn_expect (int type)
{
  return gettkn_expect_mask (T_BIT (type));
}

static void
putback_tkn (void)
{
  input_putback (cur_input);
}

enum
  {
    PARSER_OK,
    PARSER_OK_NONL,
    PARSER_FAIL,
    PARSER_END
  };

typedef int (*PARSER) (void *, void *);

typedef struct parser_table
{
  char *name;
  PARSER parser;
  void *data;
  size_t off;
} PARSER_TABLE;

static PARSER_TABLE *
parser_find (PARSER_TABLE *tab, char const *name)
{
  for (; tab->name; tab++)
    if (strcasecmp (tab->name, name) == 0)
      return tab;
  return NULL;
}

static int
parser_loop (PARSER_TABLE *ptab, void *call_data, void *section_data, struct locus_range *retrange)
{
  struct token *tok;
  size_t i;
  int res = PARSER_OK;

  if (retrange)
    {
      retrange->beg = last_token_locus_range ()->beg;
    }

  for (;;)
    {
      int type = gettkn (&tok);

      if (type == EOF)
	{
	  if (retrange)
	    {
	      conf_error_at_locus_point (&retrange->beg, "unexpected end of file");
	      return PARSER_FAIL;
	    }
	  goto end;
	}
      else if (type == T_ERROR)
	return PARSER_FAIL;

      if (retrange)
	{
	  retrange->end = last_token_locus_range ()->end;
	}

      if (tok->type == T_IDENT)
	{
	  PARSER_TABLE *ent = parser_find (ptab, tok->str);
	  if (ent)
	    {
	      void *data = ent->data ? ent->data : call_data;
	      switch (ent->parser ((char*)data + ent->off, section_data))
		{
		case PARSER_OK:
		  type = gettkn (&tok);
		  if (type == T_ERROR)
		    return PARSER_FAIL;
		  if (type != '\n' && type != EOF)
		    {
		      conf_error ("unexpected %s", token_type_str (type));
		      return PARSER_FAIL;
		    }
		  break;

		case PARSER_OK_NONL:
		  continue;

		case PARSER_FAIL:
		  return PARSER_FAIL;

		case PARSER_END:
		  goto end;
		}
	    }
	  else
	    {
	      conf_error_at_locus_range (&tok->locus, "unrecognized keyword");
	      return PARSER_FAIL;
	    }
	}
      else if (tok->type == '\n')
	continue;
      else
	conf_error_at_locus_range (&tok->locus, "syntax error");
    }
 end:
  return PARSER_OK;
}

typedef struct
{
  int log_level;
  int facility;
  unsigned clnt_to;
  unsigned be_to;
  unsigned ws_to;
  unsigned be_connto;
  unsigned ignore_case;
} POUND_DEFAULTS;

static int
parse_include (void *call_data, void *section_data)
{
  struct token *tok = gettkn_expect (T_STRING);
  if (!tok)
    return PARSER_FAIL;
  if (push_input (tok->str))
    return PARSER_FAIL;
  return PARSER_OK_NONL;
}

static int
parse_end (void *call_data, void *section_data)
{
  return PARSER_END;
}

static int
int_set_one (void *call_data, void *section_data)
{
  *(int*)call_data = 1;
  return PARSER_OK;
}

static int
assign_string (void *call_data, void *section_data)
{
  char *s;
  struct token *tok = gettkn_expect (T_STRING);
  if (!tok)
    return PARSER_FAIL;
  s = xstrdup (tok->str);
  *(char**)call_data = s;
  return PARSER_OK;
}

static int
assign_string_from_file (void *call_data, void *section_data)
{
  struct stat st;
  char *s;
  FILE *fp;
  struct token *tok = gettkn_expect (T_STRING);
  if (!tok)
    return PARSER_FAIL;
  if (stat (tok->str, &st))
    {
      conf_error ("can't stat %s: %s", tok->str, strerror (errno));
      return PARSER_FAIL;
    }
  // FIXME: Check st_size bounds.
  s = xmalloc (st.st_size + 1);
  if ((fp = fopen (tok->str, "r")) == NULL)
    {
      conf_error ("can't open %s: %s", tok->str, strerror (errno));
      return PARSER_FAIL;
    }
  if (fread (s, st.st_size, 1, fp) != 1)
    {
      conf_error ("%s: read error: %s", tok->str, strerror (errno));
      return PARSER_FAIL;
    }
  s[st.st_size] = 0;
  fclose (fp);
  *(char**)call_data = s;
  return PARSER_OK;
}

static int
assign_bool (void *call_data, void *section_data)
{
  char *s;
  struct token *tok = gettkn_expect_mask (T_UNQ);

  if (!tok)
    return PARSER_FAIL;

  if (strcmp (tok->str, "1") == 0 ||
      strcmp (tok->str, "yes") == 0 ||
      strcmp (tok->str, "true") == 0 ||
      strcmp (tok->str, "on") == 0)
    *(int *)call_data = 1;
  else if (strcmp (tok->str, "0") == 0 ||
	   strcmp (tok->str, "no") == 0 ||
	   strcmp (tok->str, "false") == 0 ||
	   strcmp (tok->str, "off") == 0)
    *(int *)call_data = 0;
  else
    {
      conf_error ("%s", "not a boolean value");
      conf_error ("valid booleans are: %s for true value, and %s for false value",
		  "1, yes, true, on",
		  "0, no, false, off");
      return PARSER_FAIL;
    }
  return PARSER_OK;
}

static int
assign_unsigned (void *call_data, void *section_data)
{
  unsigned long n;
  char *p;
  struct token *tok = gettkn_expect (T_NUMBER);

  if (!tok)
    return PARSER_FAIL;

  errno = 0;
  n = strtoul (tok->str, &p, 10);
  if (errno || *p || n > UINT_MAX)
    {
      conf_error ("%s", "bad unsigned number");
      return PARSER_FAIL;
    }
  *(unsigned *)call_data = n;
  return 0;
}

static int
assign_int (void *call_data, void *section_data)
{
  long n;
  char *p;
  struct token *tok = gettkn_expect (T_NUMBER);

  if (!tok)
    return PARSER_FAIL;

  errno = 0;
  n = strtol (tok->str, &p, 10);
  if (errno || *p || n < INT_MIN || n > INT_MAX)
    {
      conf_error ("%s", "bad integer number");
      return PARSER_FAIL;
    }
  *(int *)call_data = n;
  return 0;
}

static int
assign_int_range (int *dst, int min, int max)
{
  int n;
  int rc;

  if ((rc = assign_int (&n, NULL)) != PARSER_OK)
    return rc;

  if (!(min <= n && n <= max))
    {
      conf_error ("value out of allowed range (%d..%d)", min, max);
      return PARSER_FAIL;
    }
  *dst = n;
  return PARSER_OK;
}

static int
assign_LONG (void *call_data, void *section_data)
{
  LONG n;
  char *p;
  struct token *tok = gettkn_expect (T_NUMBER);

  if (!tok)
    return PARSER_FAIL;

  errno = 0;
  n = STRTOL (tok->str, &p, 10);
  if (errno || *p)
    {
      conf_error ("%s", "bad long number");
      return PARSER_FAIL;
    }
  *(LONG *)call_data = n;
  return 0;
}

#define assign_timeout assign_unsigned

static struct kwtab facility_table[] = {
  { "auth", LOG_AUTH },
#ifdef  LOG_AUTHPRIV
  { "authpriv", LOG_AUTHPRIV },
#endif
  { "cron", LOG_CRON },
  { "daemon", LOG_DAEMON },
#ifdef  LOG_FTP
  { "ftp", LOG_FTP },
#endif
  { "kern", LOG_KERN },
  { "lpr", LOG_LPR },
  { "mail", LOG_MAIL },
  { "news", LOG_NEWS },
  { "syslog", LOG_SYSLOG },
  { "user", LOG_USER },
  { "uucp", LOG_UUCP },
  { "local0", LOG_LOCAL0 },
  { "local1", LOG_LOCAL1 },
  { "local2", LOG_LOCAL2 },
  { "local3", LOG_LOCAL3 },
  { "local4", LOG_LOCAL4 },
  { "local5", LOG_LOCAL5 },
  { "local6", LOG_LOCAL6 },
  { "local7", LOG_LOCAL7 },
  { NULL }
};

static int
assign_log_facility (void *call_data, void *section_data)
{
  struct token *tok = gettkn_expect_mask (T_UNQ);
  int n;

  if (strcmp (tok->str, "-") == 0)
    n = -1;
  else if (kw_to_tok (facility_table, tok->str, 1, &n) != 0)
    {
      conf_error ("%s", "unknown log facility name");
      return PARSER_FAIL;
    }
  *(int*)call_data = n;

  return PARSER_OK;
}

static int
assign_log_level (void *call_data, void *section_data)
{
  unsigned n;
  int ret = assign_unsigned (&n, NULL);
  if (ret == PARSER_OK)
    {
      if (n >= INT_MAX)
	{
	  conf_error ("%s", "log level out of allowed range");
	  ret = PARSER_FAIL;
	}
      else
	{
	  *(int*)call_data = n;
	}
    }
  return ret;
}

/*
 * The ai_flags in the struct addrinfo is not used, unless in hints.
 * Therefore it is reused to mark which parts of address have been
 * initialized.
 */
#define ADDRINFO_SET_ADDRESS(addr) ((addr)->ai_flags = AI_NUMERICHOST)
#define ADDRINFO_HAS_ADDRESS(addr) ((addr)->ai_flags & AI_NUMERICHOST)
#define ADDRINFO_SET_PORT(addr) ((addr)->ai_flags |= AI_NUMERICSERV)
#define ADDRINFO_HAS_PORT(addr) ((addr)->ai_flags & AI_NUMERICSERV)

static int
assign_address_internal (struct addrinfo *addr, struct token *tok)
{
  if (!tok)
    return PARSER_FAIL;

  if (tok->type != T_IDENT && tok->type != T_LITERAL && tok->type != T_STRING)
    {
      conf_error_at_locus_range (&tok->locus,
				 "expected hostname or IP address, but found %s",
				 token_type_str (tok->type));
      return PARSER_FAIL;
    }
  if (get_host (tok->str, addr, PF_UNSPEC))
    {
      /* if we can't resolve it assume this is a UNIX domain socket */
      struct sockaddr_un *sun;
      size_t len = strlen (tok->str);
      if (len > UNIX_PATH_MAX)
	{
	  conf_error_at_locus_range (&tok->locus,
				     "%s", "UNIX path name too long");
	  return PARSER_FAIL;
	}

      len += offsetof (struct sockaddr_un, sun_path) + 1;
      sun = xmalloc (len);
      sun->sun_family = AF_UNIX;
      strcpy (sun->sun_path, tok->str);

      addr->ai_socktype = SOCK_STREAM;
      addr->ai_family = AF_UNIX;
      addr->ai_protocol = 0;
      addr->ai_addr = (struct sockaddr *) sun;
      addr->ai_addrlen = len;
    }
  ADDRINFO_SET_ADDRESS (addr);
  return PARSER_OK;
}

static int
assign_address (void *call_data, void *section_data)
{
  struct addrinfo *addr = call_data;

  if (ADDRINFO_HAS_ADDRESS (addr))
    {
      conf_error ("%s", "Duplicate Address statement");
      return PARSER_FAIL;
    }

  return assign_address_internal (call_data, gettkn_any ());
}

static int
assign_port_internal (struct addrinfo *addr, struct token *tok)
{
  struct addrinfo hints, *res;
  int rc;

  if (!tok)
    return PARSER_FAIL;

  if (tok->type != T_IDENT && tok->type != T_NUMBER)
    {
      conf_error_at_locus_range (&tok->locus,
				 "expected port number or service name, but found %s",
				 token_type_str (tok->type));
      return PARSER_FAIL;
    }

  if (!(addr->ai_family == AF_INET || addr->ai_family == AF_INET6))
    {
      conf_error_at_locus_range (&tok->locus, "Port is not applicable to this address family");
      return PARSER_FAIL;
    }

  memset (&hints, 0, sizeof(hints));
  hints.ai_flags = feature_is_set (FEATURE_DNS) ? 0 : AI_NUMERICHOST;
  hints.ai_family = addr->ai_family;
  hints.ai_socktype = addr->ai_socktype;
  hints.ai_protocol = addr->ai_protocol;
  rc = getaddrinfo (NULL, tok->str, &hints, &res);
  if (rc != 0)
    {
      conf_error_at_locus_range (&tok->locus,
				 "bad port number: %s", gai_strerror (rc));
      return PARSER_FAIL;
    }

  switch (addr->ai_family)
    {
    case AF_INET:
      ((struct sockaddr_in *)addr->ai_addr)->sin_port =
	((struct sockaddr_in *)res->ai_addr)->sin_port;
      break;

    case AF_INET6:
      ((struct sockaddr_in6 *)addr->ai_addr)->sin6_port =
	((struct sockaddr_in6 *)res->ai_addr)->sin6_port;
      break;

    default:
      conf_error_at_locus_range (&tok->locus, "%s",
				 "Port is supported only for INET/INET6 back-ends");
      return PARSER_FAIL;
    }
  freeaddrinfo (res);
  ADDRINFO_SET_PORT (addr);

  return PARSER_OK;
}

static int
assign_port (void *call_data, void *section_data)
{
  struct addrinfo *addr = call_data;

  if (ADDRINFO_HAS_PORT (addr))
    {
      conf_error ("%s", "Duplicate port statement");
      return PARSER_FAIL;
    }
  if (!(ADDRINFO_HAS_ADDRESS (addr)))
    {
      conf_error ("%s", "Address statement should precede Port");
      return PARSER_FAIL;
    }

  return assign_port_internal (call_data, gettkn_any ());
}

static int
parse_ECDHCurve (void *call_data, void *section_data)
{
  struct token *tok = gettkn_expect (T_STRING);
  if (!tok)
    return PARSER_FAIL;
#if OPENSSL_VERSION_MAJOR < 3 && !defined OPENSSL_NO_ECDH
  if (set_ECDHCurve (tok->str) == 0)
    {
      conf_error ("%s", "ECDHCurve: invalid curve name");
      return PARSER_FAIL;
    }
#else
  conf_error ("%s", "statement ignored");
#endif
  return PARSER_OK;
}

static int
parse_SSLEngine (void *call_data, void *section_data)
{
  struct token *tok = gettkn_expect (T_STRING);
  if (!tok)
    return PARSER_FAIL;
#if HAVE_OPENSSL_ENGINE_H && OPENSSL_VERSION_MAJOR < 3
  ENGINE *e;

  if (!(e = ENGINE_by_id (tok->str)))
    {
      conf_error ("%s", "unrecognized engine");
      return PARSER_FAIL;
    }

  if (!ENGINE_init (e))
    {
      ENGINE_free (e);
      conf_error ("%s", "could not init engine");
      return PARSER_FAIL;
    }

  if (!ENGINE_set_default (e, ENGINE_METHOD_ALL))
    {
      ENGINE_free (e);
      conf_error ("%s", "could not set all defaults");
    }

  ENGINE_finish (e);
  ENGINE_free (e);
#else
  conf_error ("%s", "statement ignored");
#endif

  return PARSER_OK;
}

/*
 * basic hashing function, based on fmv
 */
static unsigned long
t_hash (const TABNODE * e)
{
  unsigned long res;
  char *k;

  k = e->key;
  res = 2166136261;
  while (*k)
    res = ((res ^ *k++) * 16777619) & 0xFFFFFFFF;
  return res;
}

#if OPENSSL_VERSION_NUMBER < 0x10100000L
# if OPENSSL_VERSION_NUMBER >= 0x10000000L
static
IMPLEMENT_LHASH_HASH_FN (t, TABNODE)
# else
static
IMPLEMENT_LHASH_HASH_FN (t_hash, const TABNODE *)
# endif
#endif
static int
t_cmp (const TABNODE * d1, const TABNODE * d2)
{
  return strcmp (d1->key, d2->key);
}

#if OPENSSL_VERSION_NUMBER < 0x10100000L
# if OPENSSL_VERSION_NUMBER >= 0x10000000L
static
IMPLEMENT_LHASH_COMP_FN (t, TABNODE)
# else
static
IMPLEMENT_LHASH_COMP_FN (t_cmp, const TABNODE *)
# endif
#endif

struct token_ent
{
  struct token tok;
  SLIST_ENTRY (token_ent) next;
};

typedef SLIST_HEAD (,token_ent) TOKEN_HEAD;

static int
assign_token_list (void *call_data, void *section_data)
{
  TOKEN_HEAD *head = call_data;
  struct token_ent *ent;
  struct token *tok = gettkn_expect (T_STRING);

  if (!tok)
    return PARSER_FAIL;

  XZALLOC (ent);
  ent->tok = *tok;
  ent->tok.str = xstrdup (ent->tok.str);
  SLIST_PUSH (head, ent, next);

  return PARSER_OK;
}

static int
backend_parse_haport (void *call_data, void *section_data)
{
  BACKEND *be = call_data;
  struct token *tok, saved_tok;
  int rc;
  char *s;

  if (ADDRINFO_HAS_ADDRESS (&be->ha_addr))
    {
      conf_error ("%s", "Duplicate HAport statement");
      return PARSER_FAIL;
    }

  if ((tok = gettkn_any ()) == NULL)
    return PARSER_FAIL;

  saved_tok = *tok;
  saved_tok.str = strdup (tok->str);

  if ((tok = gettkn_any ()) == NULL)
    return PARSER_FAIL;

  if (tok->type == '\n')
    {
      be->ha_addr = be->addr;
      tok = &saved_tok;
      putback_tkn ();
    }
  else if (assign_address_internal (&be->ha_addr, &saved_tok))
    return PARSER_FAIL;

  if (assign_port_internal (&be->ha_addr, tok))
    return PARSER_FAIL;

  ADDRINFO_SET_ADDRESS (&be->ha_addr);

  free (saved_tok.str);
  return PARSER_OK;
}

static int
backend_parse_https (void *call_data, void *section_data)
{
  BACKEND *be = call_data;
  struct stringbuf sb;

  if ((be->ctx = SSL_CTX_new (SSLv23_client_method ())) == NULL)
    {
      conf_openssl_error ("SSL_CTX_new");
      return PARSER_FAIL;
    }

  SSL_CTX_set_app_data (be->ctx, be);
  SSL_CTX_set_verify (be->ctx, SSL_VERIFY_NONE, NULL);
  SSL_CTX_set_mode (be->ctx, SSL_MODE_AUTO_RETRY);
#ifdef SSL_MODE_SEND_FALLBACK_SCSV
  SSL_CTX_set_mode (be->ctx, SSL_MODE_SEND_FALLBACK_SCSV);
#endif
  SSL_CTX_set_options (be->ctx, SSL_OP_ALL);
#ifdef  SSL_OP_NO_COMPRESSION
  SSL_CTX_set_options (be->ctx, SSL_OP_NO_COMPRESSION);
#endif
  SSL_CTX_clear_options (be->ctx,
			 SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION);
  SSL_CTX_clear_options (be->ctx, SSL_OP_LEGACY_SERVER_CONNECT);

  stringbuf_init (&sb);
  stringbuf_printf (&sb, "%d-Pound-%ld", getpid (), random ());
  SSL_CTX_set_session_id_context (be->ctx, (unsigned char *) sb.base, sb.len);
  stringbuf_free (&sb);

  POUND_SSL_CTX_init (be->ctx);

  return PARSER_OK;
}

static int
backend_parse_cert (void *call_data, void *section_data)
{
  BACKEND *be = call_data;
  struct token *tok;

  if (be->ctx == NULL)
    {
      conf_error ("%s", "HTTPS must be used before this statement");
      return PARSER_FAIL;
    }

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return PARSER_FAIL;

  if (SSL_CTX_use_certificate_chain_file (be->ctx, tok->str) != 1)
    {
      conf_openssl_error ("SSL_CTX_use_certificate_chain_file");
      return PARSER_FAIL;
    }

  if (SSL_CTX_use_PrivateKey_file (be->ctx, tok->str, SSL_FILETYPE_PEM) != 1)
    {
      conf_openssl_error ("SSL_CTX_use_PrivateKey_file");
      return PARSER_FAIL;
    }

  if (SSL_CTX_check_private_key (be->ctx) != 1)
    {
      conf_openssl_error ("SSL_CTX_check_private_key failed");
      return PARSER_FAIL;
    }

  return PARSER_OK;
}

static int
backend_assign_ciphers (void *call_data, void *section_data)
{
  BACKEND *be = call_data;
  struct token *tok;

  if (be->ctx == NULL)
    {
      conf_error ("%s", "HTTPS must be used before this statement");
      return PARSER_FAIL;
    }

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return PARSER_FAIL;

  SSL_CTX_set_cipher_list (be->ctx, tok->str);
  return PARSER_OK;
}

static int
backend_assign_priority (void *call_data, void *section_data)
{
  return assign_int_range (call_data, 0, 9);
}

static int
set_proto_opt (int *opt)
{
  struct token *tok;
  int n;

  static struct kwtab kwtab[] = {
    { "SSLv2", SSL_OP_NO_SSLv2 },
    { "SSLv3", SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 },
#ifdef SSL_OP_NO_TLSv1
    { "TLSv1", SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 },
#endif
#ifdef SSL_OP_NO_TLSv1_1
    { "TLSv1_1", SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
		 SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 },
#endif
#ifdef SSL_OP_NO_TLSv1_2
    { "TLSv1_2", SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
		 SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 |
		 SSL_OP_NO_TLSv1_2 },
#endif
    { NULL }
  };

  if ((tok = gettkn_expect (T_IDENT)) == NULL)
    return PARSER_FAIL;

  if (kw_to_tok (kwtab, tok->str, 0, &n))
    {
      conf_error ("%s", "unrecognized protocol name");
      return PARSER_FAIL;
    }

  *opt |= n;

  return PARSER_OK;
}

static int
disable_proto (void *call_data, void *section_data)
{
  SSL_CTX *ctx = call_data;
  struct token *tok;
  int n = 0;

  if (ctx == NULL)
    {
      conf_error ("%s", "HTTPS must be used before this statement");
      return PARSER_FAIL;
    }

  if (set_proto_opt (&n) != PARSER_OK)
    return PARSER_FAIL;

  SSL_CTX_set_options (ctx, n);

  return PARSER_OK;
}

static PARSER_TABLE backend_parsetab[] = {
  {
    .name = "End",
    .parser = parse_end
  },
  {
    .name = "Address",
    .parser = assign_address,
    .off = offsetof (BACKEND, addr)
  },
  {
    .name = "Port",
    .parser = assign_port,
    .off = offsetof (BACKEND, addr)
  },
  {
    .name = "Priority",
    .parser = backend_assign_priority,
    .off = offsetof (BACKEND, priority)
  },
  {
    .name = "TimeOut",
    .parser = assign_timeout,
    .off = offsetof (BACKEND, to)
  },
  {
    .name = "WSTimeOut",
    .parser = assign_timeout,
    .off = offsetof (BACKEND, ws_to)
  },
  {
    .name = "ConnTO",
    .parser = assign_timeout,
    .off = offsetof (BACKEND, conn_to)
  },
  {
    .name = "HAport",
    .parser = backend_parse_haport
  },
  {
    .name = "HTTPS",
    .parser = backend_parse_https
  },
  {
    .name = "Cert",
    .parser = backend_parse_cert
  },
  {
    .name = "Ciphers",
    .parser = backend_assign_ciphers
  },
  {
    .name = "Disable",
    .parser = disable_proto,
    .off = offsetof (BACKEND, ctx)
  },
  { NULL }
};

static PARSER_TABLE emergency_parsetab[] = {
  { "End", parse_end },
  { "Address", assign_address, NULL, offsetof (BACKEND, addr) },
  { "Port", assign_port, NULL, offsetof (BACKEND, addr) },
  { "TimeOut", assign_timeout, NULL, offsetof (BACKEND, to) },
  { "WSTimeOut", assign_timeout, NULL, offsetof (BACKEND, ws_to) },
  { "ConnTO", assign_timeout, NULL, offsetof (BACKEND, conn_to) },
  { "HTTPS", backend_parse_https },
  { "Cert", backend_parse_cert },
  { "Ciphers", backend_assign_ciphers },
  { "Disable", disable_proto, NULL, offsetof (BACKEND, ctx) },
  { NULL }
};

static int
check_addrinfo (struct addrinfo const *addr, struct locus_range const *range, char const *name)
{
  if (ADDRINFO_HAS_ADDRESS (addr))
    {
      if (!ADDRINFO_HAS_PORT (addr) &&
	  (addr->ai_family == AF_INET || addr->ai_family == AF_INET6))
	{
	  conf_error_at_locus_range (range, "%s missing Port declaration", name);
	  return PARSER_FAIL;
	}
    }
  else
    {
      conf_error_at_locus_range (range, "%s missing Address declaration", name);
      return PARSER_FAIL;
    }
  return PARSER_OK;
}

static BACKEND *
parse_backend_internal (PARSER_TABLE *table, POUND_DEFAULTS *dfl)
{
  BACKEND *be;
  struct locus_range range;

  XZALLOC (be);
  be->be_type = BE_BACKEND;
  be->addr.ai_socktype = SOCK_STREAM;
  be->to = dfl->be_to;
  be->conn_to = dfl->be_connto;
  be->ws_to = dfl->ws_to;
  be->alive = 1;
  memset (&be->addr, 0, sizeof (be->addr));
  be->priority = 5;
  memset (&be->ha_addr, 0, sizeof (be->ha_addr));
  be->url = NULL;
  pthread_mutex_init (&be->mut, NULL);

  if (parser_loop (table, be, dfl, &range))
    return NULL;

  if (check_addrinfo (&be->addr, &range, "Backend") != PARSER_OK)
    return NULL;

  return be;
}

static int
parse_backend (void *call_data, void *section_data)
{
  BACKEND_HEAD *head = call_data;
  BACKEND *be;

  be = parse_backend_internal (backend_parsetab, section_data);
  if (!be)
    return PARSER_FAIL;

  SLIST_PUSH (head, be, next);

  return PARSER_OK;
}

static int
parse_emergency (void *call_data, void *section_data)
{
  BACKEND **res_ptr = call_data;
  BACKEND *be;
  POUND_DEFAULTS dfl = *(POUND_DEFAULTS*)section_data;

  dfl.be_to = 120;
  dfl.be_connto = 120;
  dfl.ws_to = 120;

  be = parse_backend_internal (emergency_parsetab, &dfl);
  if (!be)
    return PARSER_FAIL;

  *res_ptr = be;

  return PARSER_OK;
}


struct service_ext
{
  SERVICE svc;
  TOKEN_HEAD url;
  int ignore_case;
};

static int
assign_matcher (void *call_data, void *section_data)
{
  MATCHER *m;
  MATCHER_HEAD *head = call_data;
  int rc;

  struct token *tok = gettkn_expect (T_STRING);
  if (!tok)
    return PARSER_FAIL;

  XZALLOC (m);
  rc = regcomp (&m->pat, tok->str, REG_ICASE | REG_NEWLINE | REG_EXTENDED);
  if (rc)
    {
      conf_regcomp_error (rc, &m->pat, NULL);
      return PARSER_FAIL;
    }
  SLIST_PUSH (head, m, next);

  return PARSER_OK;
}

static int
assign_redirect (void *call_data, void *section_data)
{
  BACKEND_HEAD *head = call_data;
  struct token *tok;
  int code = 302;
  BACKEND *be;
  regmatch_t matches[5];

  if ((tok = gettkn_any ()) == NULL)
    return PARSER_FAIL;

  if (tok->type == T_NUMBER)
    {
      int n = atoi (tok->str);
      if (n == 301 || n == 302 || n == 307)
	code = n;
      else
	{
	  conf_error ("%s", "invalid status code");
	  return PARSER_FAIL;
	}

      if ((tok = gettkn_any ()) == NULL)
	return PARSER_FAIL;
    }

  if (tok->type != T_STRING)
    {
      conf_error ("expected %s, but found %s", token_type_str (T_STRING), token_type_str (tok->type));
      return PARSER_FAIL;
    }

  XZALLOC (be);
  be->be_type = BE_REDIRECT;
  be->redir_code = code;
  be->priority = 1;
  be->alive = 1;
  pthread_mutex_init (&be->mut, NULL);
  be->url = xstrdup (tok->str);

  if (regexec (&LOCATION, be->url, 4, matches, 0))
    {
      conf_error ("%s", "Redirect bad URL");
      return PARSER_FAIL;
    }

  if ((be->redir_req = matches[3].rm_eo - matches[3].rm_so) == 1)
    /* the path is a single '/', so remove it */
    be->url[matches[3].rm_so] = '\0';

  SLIST_PUSH (head, be, next);

  return PARSER_OK;
}

struct service_session
{
  int type;
  char *id;
  unsigned ttl;
};

static int
session_type_parser (void *call_data, void *section_data)
{
  struct service_session *sp = call_data;
  struct token *tok;
  int n;

  static struct kwtab kwtab[] = {
    { "IP", SESS_IP },
    { "COOKIE", SESS_COOKIE },
    { "URL", SESS_URL },
    { "PARM", SESS_PARM },
    { "BASIC", SESS_BASIC },
    { "HEADER", SESS_HEADER },
    { NULL }
  };

  if ((tok = gettkn_expect (T_IDENT)) == NULL)
    return PARSER_FAIL;

  if (kw_to_tok (kwtab, tok->str, 1, &n))
    {
      conf_error ("%s", "Unknown Session type");
      return PARSER_FAIL;
    }
  sp->type = n;

  return PARSER_OK;
}

static PARSER_TABLE session_parsetab[] = {
  { "End", parse_end },
  { "Type", session_type_parser },
  { "TTL", assign_timeout, NULL, offsetof (struct service_session, ttl) },
  { "ID", assign_string, NULL, offsetof (struct service_session, id) },
  { NULL }
};

static int
xregcomp (regex_t *rx, char const *expr, int flags)
{
  int rc;

  rc = regcomp (rx, expr, flags);
  if (rc)
    {
      conf_regcomp_error (rc, rx, expr);
      return PARSER_FAIL;
    }
  return PARSER_OK;
}

static int
parse_session (void *call_data, void *section_data)
{
  SERVICE *svc = call_data;
  struct service_session sess;
  struct stringbuf sb;
  int rc;
  struct locus_range range;

  memset (&sess, 0, sizeof (sess));
  if (parser_loop (session_parsetab, &sess, section_data, &range))
    return PARSER_FAIL;

  if (sess.type == SESS_NONE)
    {
      conf_error_at_locus_range (&range, "Session type not defined");
      return PARSER_FAIL;
    }

  if (sess.ttl == 0)
    {
      conf_error_at_locus_range (&range, "Session TTL not defined");
      return PARSER_FAIL;
    }

  if ((sess.type == SESS_COOKIE || sess.type == SESS_URL
       || sess.type == SESS_HEADER) && sess.id == NULL)
    {
      conf_error ("%s", "Session ID not defined");
      return PARSER_FAIL;
    }

  stringbuf_init (&sb);
  switch (sess.type)
    {
    case SESS_COOKIE:
      stringbuf_printf (&sb, "Cookie[^:]*:.*[ \t]%s=", sess.id);
      if (xregcomp (&svc->sess_start, sb.base, REG_ICASE | REG_NEWLINE | REG_EXTENDED) != PARSER_OK)
	return PARSER_FAIL;

      if (xregcomp (&svc->sess_pat, "([^;]*)", REG_ICASE | REG_NEWLINE | REG_EXTENDED) != PARSER_OK)
	return PARSER_FAIL;
      break;

    case SESS_URL:
      stringbuf_printf (&sb, "[?&]%s=", sess.id);
      if (xregcomp (&svc->sess_start, sb.base, REG_ICASE | REG_NEWLINE | REG_EXTENDED) != PARSER_OK)
	return PARSER_FAIL;
      if (xregcomp (&svc->sess_pat, "([^&;#]*)", REG_ICASE | REG_NEWLINE | REG_EXTENDED) != PARSER_OK)
	return PARSER_FAIL;
      break;

    case SESS_PARM:
      if (xregcomp (&svc->sess_start, ";", REG_ICASE | REG_NEWLINE | REG_EXTENDED) != PARSER_OK)
	return PARSER_FAIL;
      if (xregcomp (&svc->sess_pat, "([^?]*)", REG_ICASE | REG_NEWLINE | REG_EXTENDED) != PARSER_OK)
	return PARSER_FAIL;
      break;

    case SESS_BASIC:
      if (xregcomp (&svc->sess_start, "Authorization:[ \t]*Basic[ \t]*",
		    REG_ICASE | REG_NEWLINE | REG_EXTENDED) != PARSER_OK)
	return PARSER_FAIL;
      if (xregcomp (&svc->sess_pat, "([^ \t]*)",
		    REG_ICASE | REG_NEWLINE | REG_EXTENDED) != PARSER_OK)
	return PARSER_FAIL;
      break;

    case SESS_HEADER:
      stringbuf_printf (&sb, "%s:[ \t]*", sess.id);
      if (xregcomp (&svc->sess_start, sb.base,
		    REG_ICASE | REG_NEWLINE | REG_EXTENDED) != PARSER_OK)
	return PARSER_FAIL;
      if (xregcomp (&svc->sess_pat, "([^ \t]*)",
		    REG_ICASE | REG_NEWLINE | REG_EXTENDED) != PARSER_OK)
	return PARSER_FAIL;
      break;
    }

  svc->sess_ttl = sess.ttl;
  svc->sess_type = sess.type;

  free (sess.id);

  return PARSER_OK;
}

static PARSER_TABLE service_parsetab[] = {
  { "End", parse_end },
  { "URL", assign_token_list, NULL, offsetof (struct service_ext, url) },
  { "IgnoreCase", assign_bool, NULL, offsetof (struct service_ext, ignore_case) },
  { "HeadRequire", assign_matcher, NULL, offsetof (SERVICE, req_head) },
  { "HeadDeny", assign_matcher, NULL, offsetof (SERVICE, deny_head) },
  { "Disabled", assign_bool, NULL, offsetof (SERVICE, disabled) },
  { "Redirect", assign_redirect, NULL, offsetof (SERVICE, backends) },
  { "Backend", parse_backend, NULL, offsetof (SERVICE, backends) },
  { "Emergency", parse_emergency, NULL, offsetof (SERVICE, emergency) },
  { "Session", parse_session },
  { NULL }
};

static int
parse_service (void *call_data, void *section_data)
{
  SERVICE_HEAD *head = call_data;
  POUND_DEFAULTS *dfl = section_data;
  struct token *tok;
  SERVICE *svc;
  struct service_ext ext;
  struct locus_range range;

  memset (&ext, 0, sizeof (ext));
  SLIST_INIT (&ext.url);
  /*
   * No use to do SLIST_INIT (&ext.svc.url), since no keywords in
   * service_parsetab attempt to modify it. Its counterpart in svc
   * will be initialized after it is allocated (see below).
   */
  SLIST_INIT (&ext.svc.req_head);
  SLIST_INIT (&ext.svc.deny_head);
  SLIST_INIT (&ext.svc.backends);

  tok = gettkn_any ();

  if (!tok)
    return PARSER_FAIL;

  ext.svc.sess_type = SESS_NONE;
  pthread_mutex_init (&ext.svc.mut, NULL);

  if (tok->type == T_STRING)
    {
      if (strlen (tok->str) > sizeof (ext.svc.name) - 1)
	{
	  conf_error ("%s", "service name too long: truncated");
	}
      strncpy (ext.svc.name, tok->str, sizeof (ext.svc.name) - 1);
    }
  else
    putback_tkn ();

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
  if ((ext.svc.sessions = lh_TABNODE_new (t_hash, t_cmp)) == NULL)
#elif OPENSSL_VERSION_NUMBER >= 0x10000000L
  if ((ext.svc.sessions = LHM_lh_new (TABNODE, t)) == NULL)
#else
  if ((ext.svc.sessions =
	 lh_new (LHASH_HASH_FN (t_hash), LHASH_COMP_FN (t_cmp))) == NULL)
#endif
    {
      conf_error ("%s", "lh_new failed");
      return -1;
    }

  ext.ignore_case = dfl->ignore_case;
  if (parser_loop (service_parsetab, &ext, section_data, &range))
    return PARSER_FAIL;
  else
    {
      BACKEND *be;
      int flags = REG_NEWLINE | REG_EXTENDED | (ext.ignore_case ? REG_ICASE : 0);

      XZALLOC (svc);
      *svc = ext.svc;
      SLIST_INIT (&svc->url);
      SLIST_COPY (&svc->req_head, &ext.svc.req_head);
      SLIST_COPY (&svc->deny_head, &ext.svc.deny_head);
      SLIST_COPY (&svc->backends, &ext.svc.backends);

      if ((be = SLIST_FIRST (&svc->backends)) == NULL)
	{
	  conf_error_at_locus_range (&range, "warning: no backends defined");
	}
      else
	{
	  SLIST_FOREACH (be, &svc->backends, next)
	    {
	      if (!be->disabled)
		svc->tot_pri += be->priority;
	      svc->abs_pri += be->priority;
	    }
	}

      while (!SLIST_EMPTY (&ext.url))
	{
	  MATCHER *m;
	  int rc;
	  struct token_ent *te = SLIST_FIRST (&ext.url);
	  SLIST_SHIFT (&ext.url, next);

	  XZALLOC (m);
	  rc = regcomp (&m->pat, te->tok.str, flags);
	  if (rc)
	    {
	      regcomp_error_at_locus_range (&te->tok.locus, rc, &m->pat, NULL);
	      return PARSER_FAIL;
	    }

	  SLIST_PUSH (&svc->url, m, next);

	  free (te->tok.str);
	  free (te);
	}

      SLIST_PUSH (head, svc, next);
    }
  return PARSER_OK;
}

static int
parse_acme (void *call_data, void *section_data)
{
  SERVICE_HEAD *head = call_data;
  POUND_DEFAULTS *dfl = section_data;
  SERVICE *svc;
  MATCHER *m;
  BACKEND *be;
  struct token *tok;
  struct stat st;
  size_t len;
  int rc;
  static char re_acme[] = "^/\\.well-known/acme-challenge/(.+)";
  static char suf_acme[] = "/$1";
  static size_t suf_acme_size = sizeof (suf_acme) - 1;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return PARSER_FAIL;

  if (stat (tok->str, &st))
    {
      conf_error ("can't stat %s: %s", tok->str, strerror (errno));
      return PARSER_FAIL;
    }
  if (!S_ISDIR (st.st_mode))
    {
      conf_error ("%s is not a directory: %s", tok->str, strerror (errno));
      return PARSER_FAIL;
    }

  /* Create service */
  XZALLOC (svc);
  SLIST_INIT (&svc->url);
  SLIST_INIT (&svc->req_head);
  SLIST_INIT (&svc->deny_head);
  SLIST_INIT (&svc->backends);

  /* Create a URL matcher */
  XZALLOC (m);
  rc = regcomp (&m->pat, re_acme, REG_EXTENDED);
  if (rc)
    {
      conf_regcomp_error (rc, &m->pat, NULL);
      return PARSER_FAIL;
    }
  SLIST_PUSH (&svc->url, m, next);

  svc->sess_type = SESS_NONE;
  pthread_mutex_init (&svc->mut, NULL);

  svc->tot_pri = 1;
  svc->abs_pri = 1;

  /* Create ACME backend */
  XZALLOC (be);
  be->be_type = BE_ACME;
  be->priority = 1;
  be->alive = 1;
  pthread_mutex_init (&be->mut, NULL);

  len = strlen (tok->str);
  if (tok->str[len-1] == '/')
    len--;

  be->url = xmalloc (len + suf_acme_size + 1);
  memcpy (be->url, tok->str, len);
  strcpy (be->url + len, suf_acme);

  /* Register backend in service */
  SLIST_PUSH (&svc->backends, be, next);

  /* Register service in the listener */
  SLIST_PUSH (head, svc, next);

  return PARSER_OK;
}


static char *xhttp[] = {
  "^(GET|POST|HEAD) ([^ ]+) HTTP/1.[01]$",
  "^(GET|POST|HEAD|PUT|PATCH|DELETE) ([^ ]+) HTTP/1.[01]$",
  "^(GET|POST|HEAD|PUT|PATCH|DELETE|LOCK|UNLOCK|PROPFIND|PROPPATCH|SEARCH|MKCOL|MOVE|COPY|OPTIONS|TRACE|MKACTIVITY|CHECKOUT|MERGE|REPORT) ([^ ]+) HTTP/1.[01]$",
  "^(GET|POST|HEAD|PUT|PATCH|DELETE|LOCK|UNLOCK|PROPFIND|PROPPATCH|SEARCH|MKCOL|MOVE|COPY|OPTIONS|TRACE|MKACTIVITY|CHECKOUT|MERGE|REPORT|SUBSCRIBE|UNSUBSCRIBE|BPROPPATCH|POLL|BMOVE|BCOPY|BDELETE|BPROPFIND|NOTIFY|CONNECT) ([^ ]+) HTTP/1.[01]$",
  "^(GET|POST|HEAD|PUT|PATCH|DELETE|LOCK|UNLOCK|PROPFIND|PROPPATCH|SEARCH|MKCOL|MOVE|COPY|OPTIONS|TRACE|MKACTIVITY|CHECKOUT|MERGE|REPORT|SUBSCRIBE|UNSUBSCRIBE|BPROPPATCH|POLL|BMOVE|BCOPY|BDELETE|BPROPFIND|NOTIFY|CONNECT|RPC_IN_DATA|RPC_OUT_DATA) ([^ ]+) HTTP/1.[01]$",
};

static int
listener_parse_xhttp (void *call_data, void *section_data)
{
  regex_t *rx = call_data;
  unsigned n;
  int rc;

  if ((rc = assign_unsigned (&n, NULL)) != PARSER_OK)
    return rc;
  if (n >= sizeof (xhttp) / sizeof (xhttp[0]))
    {
      conf_error ("%s", "argument out of allowed range");
      return PARSER_FAIL;
    }
  regfree (rx);
  if (xregcomp (rx, xhttp[n], REG_ICASE | REG_NEWLINE | REG_EXTENDED) != PARSER_OK)
    return PARSER_FAIL;
  return PARSER_OK;
}

static int
listener_parse_checkurl (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  POUND_DEFAULTS *dfl = section_data;
  struct token *tok;
  int rc;

  if (lst->has_pat)
    {
      conf_error ("%s", "CheckURL multiple pattern");
      return PARSER_FAIL;
    }

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return PARSER_FAIL;

  rc = regcomp (&lst->url_pat, tok->str,
		REG_NEWLINE | REG_EXTENDED |
		(dfl->ignore_case ? REG_ICASE : 0));
  if (rc)
    {
      conf_regcomp_error (rc, &lst->url_pat, NULL);
      return PARSER_FAIL;
    }
  lst->has_pat = 1;

  return PARSER_OK;
}

static int
read_fd (int fd)
{
  struct msghdr msg;
  struct iovec iov[1];
  char base[1];
  union
  {
    struct cmsghdr cm;
    char control[CMSG_SPACE (sizeof (int))];
  } control_un;
  struct cmsghdr *cmptr;

  msg.msg_control = control_un.control;
  msg.msg_controllen = sizeof (control_un.control);

  msg.msg_name = NULL;
  msg.msg_namelen = 0;

  iov[0].iov_base = base;
  iov[0].iov_len = sizeof (base);

  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  if (recvmsg (fd, &msg, 0) > 0)
    {
      if ((cmptr = CMSG_FIRSTHDR (&msg)) != NULL
	  && cmptr->cmsg_len == CMSG_LEN (sizeof (int))
	  && cmptr->cmsg_level == SOL_SOCKET
	  && cmptr->cmsg_type == SCM_RIGHTS)
	return *((int*) CMSG_DATA (cmptr));
    }
  return -1;
}

static int
listener_parse_socket_from (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  struct sockaddr_storage ss;
  socklen_t sslen = sizeof (ss);
  struct token *tok;
  struct addrinfo addr;
  int sfd, fd;

  if (ADDRINFO_HAS_ADDRESS (&lst->addr))
    {
      conf_error ("%s", "Duplicate Address or SocketFrom statement");
      return PARSER_FAIL;
    }

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return PARSER_FAIL;
  memset (&addr, 0, sizeof (addr));
  if (assign_address_internal (&addr, tok) != PARSER_OK)
    return PARSER_FAIL;

  if ((sfd = socket (PF_UNIX, SOCK_STREAM, 0)) < 0)
    {
      conf_error ("socket: %s", strerror (errno));
      return PARSER_FAIL;
    }

  if (connect (sfd, addr.ai_addr, addr.ai_addrlen) < 0)
    {
      conf_error ("connect %s: %s",
		  ((struct sockaddr_un*)addr.ai_addr)->sun_path,
		  strerror (errno));
      return PARSER_FAIL;
    }

  fd = read_fd (sfd);

  if (fd == -1)
    {
      conf_error ("can't get socket: %s", strerror (errno));
      return PARSER_FAIL;
    }

  if (getsockname (fd, (struct sockaddr*) &ss, &sslen) == -1)
    {
      conf_error ("can't get socket address: %s", strerror (errno));
      return PARSER_FAIL;
    }

  free (lst->addr.ai_addr);
  lst->addr.ai_addr = xmalloc (sslen);
  memcpy (lst->addr.ai_addr, &ss, sslen);
  lst->addr.ai_addrlen = sslen;
  lst->addr.ai_family = ss.ss_family;
  ADDRINFO_SET_ADDRESS (&lst->addr);
  ADDRINFO_SET_PORT (&lst->addr);

  {
    struct stringbuf sb;
    char tmp[MAX_ADDR_BUFSIZE];

    stringbuf_init (&sb);
    stringbuf_format_locus_range (&sb, &tok->locus);
    stringbuf_add_string (&sb, ": obtained address ");
    stringbuf_add_string (&sb, addr2str (tmp, sizeof (tmp), &lst->addr, 0));
    logmsg (LOG_DEBUG, "%s", stringbuf_finish (&sb));
    stringbuf_free (&sb);
  }

  lst->sock = fd;

  return PARSER_OK;
}

static int
parse_rewritelocation (void *call_data, void *section_data)
{
  return assign_int_range (call_data, 0, 2);
}

static int
append_string_line (void *call_data, void *section_data)
{
  char **dst = call_data;
  char *s = *dst;
  size_t len = s ? strlen (s) : 0;
  struct token *tok;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return PARSER_FAIL;

  s = xrealloc (s, len + strlen (tok->str) + 3);
  if (len == 0)
    strcpy (s, tok->str);
  else
    {
      strcpy (s + len, "\r\n");
      strcpy (s + len + 2, tok->str);
    }
  *dst = s;

  return PARSER_OK;
}

static PARSER_TABLE http_parsetab[] = {
  { "End", parse_end },
  { "Address", assign_address, NULL, offsetof (LISTENER, addr) },
  { "Port", assign_port, NULL, offsetof (LISTENER, addr) },
  { "SocketFrom", listener_parse_socket_from },
  { "xHTTP", listener_parse_xhttp, NULL, offsetof (LISTENER, verb) },
  { "Client", assign_timeout, NULL, offsetof (LISTENER, to) },
  { "CheckURL", listener_parse_checkurl },
  { "Err404", assign_string_from_file, NULL, offsetof (LISTENER, err404) },
  { "Err413", assign_string_from_file, NULL, offsetof (LISTENER, err413) },
  { "Err414", assign_string_from_file, NULL, offsetof (LISTENER, err414) },
  { "Err500", assign_string_from_file, NULL, offsetof (LISTENER, err500) },
  { "Err501", assign_string_from_file, NULL, offsetof (LISTENER, err501) },
  { "Err503", assign_string_from_file, NULL, offsetof (LISTENER, err503) },
  { "MaxRequest", assign_LONG, NULL, offsetof (LISTENER, max_req) },
  { "HeadRemove", assign_matcher, NULL, offsetof (LISTENER, head_off) },
  { "RewriteLocation", parse_rewritelocation, NULL, offsetof (LISTENER, rewr_loc) },
  { "RewriteDestination", assign_bool, NULL, offsetof (LISTENER, rewr_dest) },
  { "LogLevel", assign_int, NULL, offsetof (LISTENER, log_level) },
  { "AddHeader", append_string_line, NULL, offsetof (LISTENER, add_head) },
  { "Service", parse_service, NULL, offsetof (LISTENER, services) },
  { "ACME", parse_acme, NULL, offsetof (LISTENER, services) },
  { NULL }
};

static LISTENER *
listener_alloc (POUND_DEFAULTS *dfl)
{
  LISTENER *lst;

  XZALLOC (lst);

  lst->sock = -1;
  lst->to = dfl->clnt_to;
  lst->rewr_loc = 1;
  lst->err404 = "Not Found.";
  lst->err413 = "Request too large.";
  lst->err414 = "Request URI is too long.";
  lst->err500 = "An internal server error occurred. Please try again later.";
  lst->err501 = "This method may not be used.";
  lst->err503 = "The service is not available. Please try again later.";
  lst->log_level = dfl->log_level;
  if (xregcomp (&lst->verb, xhttp[0], REG_ICASE | REG_NEWLINE | REG_EXTENDED) != PARSER_OK)
    {
      free (lst);
      return NULL;
    }
  SLIST_INIT (&lst->head_off);
  SLIST_INIT (&lst->services);
  SLIST_INIT (&lst->ctx_head);
  return lst;
}

static int
parse_listen_http (void *call_data, void *section_data)
{
  LISTENER *lst;
  LISTENER_HEAD *list_head = call_data;
  POUND_DEFAULTS *dfl = section_data;
  struct locus_range range;

  if ((lst = listener_alloc (dfl)) == NULL)
    return PARSER_FAIL;

  if (parser_loop (http_parsetab, lst, section_data, &range))
    return PARSER_FAIL;

  if (check_addrinfo (&lst->addr, &range, "ListenHTTP") != PARSER_OK)
    return PARSER_FAIL;

  SLIST_PUSH (list_head, lst, next);
  return PARSER_OK;
}

static int
is_class (int c, char *cls)
{
  int k;

  if (*cls == 0)
    return 0;
  if (c == *cls)
    return 1;
  cls++;
  while ((k = *cls++) != 0)
    {
      if (k == '-' && cls[0] != 0)
	{
	  if (cls[-2] <= c && c <= cls[0])
	    return 1;
	  cls++;
	}
      else if (c == k)
	return 1;
    }
  return 0;
}

static char *
extract_cn (char const *str, size_t *plen)
{
  while (*str)
    {
      if ((str[0] == 'c' || str[0] == 'C') && (str[1] == 'n' || str[1] == 'N') && str[2] == '=')
	{
	  size_t i;
	  str += 3;
	  for (i = 0; str[i] && is_class (str[i], "-*.A-Za-z0-9"); i++)
	    ;
	  if (str[i] == 0)
	    {
	      *plen = i;
	      return (char*) str;
	    }
	  str += i;
	}
      str++;
    }
  return NULL;
}

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
# define general_name_string(n) \
	(unsigned char*) \
	xstrndup ((char*)ASN1_STRING_get0_data (n->d.dNSName),	\
		 ASN1_STRING_length (n->d.dNSName) + 1)
#else
# define general_name_string(n) \
	(unsigned char*) \
	xstrndup ((char*)ASN1_STRING_data(n->d.dNSName),	\
		 ASN1_STRING_length (n->d.dNSName) + 1)
#endif

unsigned char **
get_subjectaltnames (X509 * x509, unsigned int *count)
{
  unsigned int local_count;
  unsigned char **result;
  STACK_OF (GENERAL_NAME) * san_stack =
    (STACK_OF (GENERAL_NAME) *) X509_get_ext_d2i (x509, NID_subject_alt_name,
						  NULL, NULL);
  unsigned char *temp[sk_GENERAL_NAME_num (san_stack)];
  GENERAL_NAME *name;
  int i;

  local_count = 0;
  result = NULL;
  name = NULL;
  *count = 0;
  if (san_stack == NULL)
    return NULL;
  while (sk_GENERAL_NAME_num (san_stack) > 0)
    {
      name = sk_GENERAL_NAME_pop (san_stack);
      switch (name->type)
	{
	case GEN_DNS:
	  temp[local_count++] = general_name_string (name);
	  break;

	default:
	  logmsg (LOG_INFO, "unsupported subjectAltName type encountered: %i",
		  name->type);
	}
      GENERAL_NAME_free (name);
    }

  result = xcalloc (local_count, sizeof (unsigned char *));
  for (i = 0; i < local_count; i++)
    result[i] = temp[i];
  *count = local_count;

  sk_GENERAL_NAME_pop_free (san_stack, GENERAL_NAME_free);

  return result;
}

static int
https_parse_cert (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  struct token *tok;
  POUND_CTX *pc;

  if (lst->has_other)
    {
      conf_error ("%s", "Cert directives MUST precede other SSL-specific directives");
      return PARSER_FAIL;
    }

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return PARSER_FAIL;

  XZALLOC (pc);

  if ((pc->ctx = SSL_CTX_new (SSLv23_server_method ())) == NULL)
    {
      conf_openssl_error ("SSL_CTX_new");
      return PARSER_FAIL;
    }

  if (SSL_CTX_use_certificate_chain_file (pc->ctx, tok->str) != 1)
    {
      conf_openssl_error ("SSL_CTX_use_certificate_chain_file");
      return PARSER_FAIL;
    }
  if (SSL_CTX_use_PrivateKey_file (pc->ctx, tok->str, SSL_FILETYPE_PEM) != 1)
    {
      conf_openssl_error ("SSL_CTX_use_PrivateKey_file");
      return PARSER_FAIL;
    }

  if (SSL_CTX_check_private_key (pc->ctx) != 1)
    {
      conf_openssl_error ("SSL_CTX_check_private_key");
      return PARSER_FAIL;
    }

#ifdef SSL_CTRL_SET_TLSEXT_SERVERNAME_CB
  {
    /* we have support for SNI */
    FILE *fcert;
    char server_name[MAXBUF];
    X509 *x509;
    char *cnp;
    size_t cnl;

    if ((fcert = fopen (tok->str, "r")) == NULL)
      {
	conf_error ("%s", "ListenHTTPS: could not open certificate file");
	return PARSER_FAIL;
      }

    x509 = PEM_read_X509 (fcert, NULL, NULL, NULL);
    fclose (fcert);

    if (!x509)
      {
	conf_error ("%s", "could not get certificate subject");
	return PARSER_FAIL;
      }

    memset (server_name, '\0', MAXBUF);
    X509_NAME_oneline (X509_get_subject_name (x509), server_name,
		       sizeof (server_name) - 1);
    pc->subjectAltNameCount = 0;
    pc->subjectAltNames = NULL;
    pc->subjectAltNames = get_subjectaltnames (x509, &pc->subjectAltNameCount);
    X509_free (x509);

    if ((cnp = extract_cn (server_name, &cnl)) == NULL)
      {
	conf_error ("no CN in certificate subject name (%s)\n", server_name);
	return PARSER_FAIL;
      }
    pc->server_name = xmalloc (cnl + 1);
    memcpy (pc->server_name, cnp, cnl);
    pc->server_name[cnl] = 0;
  }
#else
  if (res->ctx)
    conf_error ("%s", "multiple certificates not supported");
#endif
  SLIST_PUSH (&lst->ctx_head, pc, next);

  return PARSER_OK;
}

static int
verify_OK (int pre_ok, X509_STORE_CTX * ctx)
{
  return 1;
}

#ifdef SSL_CTRL_SET_TLSEXT_SERVERNAME_CB
static int
SNI_server_name (SSL *ssl, int *dummy, POUND_CTX_HEAD *ctx_head)
{
  const char *server_name;
  POUND_CTX *pc;

  if ((server_name = SSL_get_servername (ssl, TLSEXT_NAMETYPE_host_name)) == NULL)
    return SSL_TLSEXT_ERR_NOACK;

  /* logmsg(LOG_DEBUG, "Received SSL SNI Header for servername %s", servername); */

  SSL_set_SSL_CTX (ssl, NULL);
  SLIST_FOREACH (pc, ctx_head, next)
    {
      if (fnmatch (pc->server_name, server_name, 0) == 0)
	{
	  /* logmsg(LOG_DEBUG, "Found cert for %s", servername); */
	  SSL_set_SSL_CTX (ssl, pc->ctx);
	  return SSL_TLSEXT_ERR_OK;
	}
      else if (pc->subjectAltNameCount > 0 && pc->subjectAltNames != NULL)
	{
	  int i;

	  for (i = 0; i < pc->subjectAltNameCount; i++)
	    {
	      if (fnmatch ((char *) pc->subjectAltNames[i], server_name, 0) ==
		  0)
		{
		  SSL_set_SSL_CTX (ssl, pc->ctx);
		  return SSL_TLSEXT_ERR_OK;
		}
	    }
	}
    }

  /* logmsg(LOG_DEBUG, "No match for %s, default used", server_name); */
  SSL_set_SSL_CTX (ssl, SLIST_FIRST (ctx_head)->ctx);
  return SSL_TLSEXT_ERR_OK;
}
#endif

static int
https_parse_client_cert (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  int depth;
  POUND_CTX *pc;

  if (SLIST_EMPTY (&lst->ctx_head))
    {
      conf_error ("%s", "ClientCert may only be used after Cert");
      return PARSER_FAIL;
    }
  lst->has_other = 1;

  if (assign_int_range (&lst->clnt_check, 0, 3) != PARSER_OK)
    return PARSER_FAIL;

  if (lst->clnt_check > 0 && assign_int (&depth, NULL) != PARSER_OK)
    return PARSER_FAIL;

  switch (lst->clnt_check)
    {
    case 0:
      /* don't ask */
      SLIST_FOREACH (pc, &lst->ctx_head, next)
	SSL_CTX_set_verify (pc->ctx, SSL_VERIFY_NONE, NULL);
      break;

    case 1:
      /* ask but OK if no client certificate */
      SLIST_FOREACH (pc, &lst->ctx_head, next)
	{
	  SSL_CTX_set_verify (pc->ctx,
			      SSL_VERIFY_PEER |
			      SSL_VERIFY_CLIENT_ONCE, NULL);
	  SSL_CTX_set_verify_depth (pc->ctx, depth);
	}
      break;

    case 2:
      /* ask and fail if no client certificate */
      SLIST_FOREACH (pc, &lst->ctx_head, next)
	{
	  SSL_CTX_set_verify (pc->ctx,
			      SSL_VERIFY_PEER |
			      SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
	  SSL_CTX_set_verify_depth (pc->ctx, depth);
	}
      break;

    case 3:
      /* ask but do not verify client certificate */
      SLIST_FOREACH (pc, &lst->ctx_head, next)
	{
	  SSL_CTX_set_verify (pc->ctx,
			      SSL_VERIFY_PEER |
			      SSL_VERIFY_CLIENT_ONCE, verify_OK);
	  SSL_CTX_set_verify_depth (pc->ctx, depth);
	}
      break;
    }
  return PARSER_OK;
}

static int
https_parse_disable (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  return set_proto_opt (&lst->ssl_op_enable);
}

static int
https_parse_ciphers (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  struct token *tok;
  POUND_CTX *pc;

  if (SLIST_EMPTY (&lst->ctx_head))
    {
      conf_error ("%s", "Ciphers may only be used after Cert");
      return PARSER_FAIL;
    }
  lst->has_other = 1;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return PARSER_FAIL;

  SLIST_FOREACH (pc, &lst->ctx_head, next)
    SSL_CTX_set_cipher_list (pc->ctx, tok->str);

  return PARSER_OK;
}

static int
https_parse_honor_cipher_order (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  int bv;

  if (assign_bool (&bv, NULL) != PARSER_OK)
    return PARSER_FAIL;

  if (bv)
    {
      lst->ssl_op_enable |= SSL_OP_CIPHER_SERVER_PREFERENCE;
      lst->ssl_op_disable &= ~SSL_OP_CIPHER_SERVER_PREFERENCE;
    }
  else
    {
      lst->ssl_op_disable |= SSL_OP_CIPHER_SERVER_PREFERENCE;
      lst->ssl_op_enable &= ~SSL_OP_CIPHER_SERVER_PREFERENCE;
    }

  return PARSER_OK;
}

static int
https_parse_allow_client_renegotiation (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;

  if (assign_int_range (&lst->allow_client_reneg, 0, 2) != PARSER_OK)
    return PARSER_FAIL;

  if (lst->allow_client_reneg == 2)
    {
      lst->ssl_op_enable |= SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION;
      lst->ssl_op_disable &= ~SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION;
    }
  else
    {
      lst->ssl_op_disable |= SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION;
      lst->ssl_op_enable &= ~SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION;
    }

  return PARSER_OK;
}

static int
https_parse_calist (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  STACK_OF (X509_NAME) *cert_names;
  struct token *tok;
  POUND_CTX *pc;

  if (SLIST_EMPTY (&lst->ctx_head))
    {
      conf_error ("%s", "CAList may only be used after Cert");
      return PARSER_FAIL;
    }
  lst->has_other = 1;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return PARSER_FAIL;

  if ((cert_names = SSL_load_client_CA_file (tok->str)) == NULL)
    {
      conf_openssl_error ("SSL_load_client_CA_file");
      return PARSER_FAIL;
    }

  SLIST_FOREACH (pc, &lst->ctx_head, next)
    SSL_CTX_set_client_CA_list (pc->ctx, cert_names);

  return PARSER_OK;
}

static int
https_parse_verifylist (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  struct token *tok;
  POUND_CTX *pc;

  if (SLIST_EMPTY (&lst->ctx_head))
    {
      conf_error ("%s", "VerifyList may only be used after Cert");
      return PARSER_FAIL;
    }
  lst->has_other = 1;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return PARSER_FAIL;

  SLIST_FOREACH (pc, &lst->ctx_head, next)
    if (SSL_CTX_load_verify_locations (pc->ctx, tok->str, NULL) != 1)
      {
	conf_openssl_error ("SSL_CTX_load_verify_locations");
	return PARSER_FAIL;
      }

  return PARSER_OK;
}

static int
https_parse_crlist (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  struct token *tok;
  X509_STORE *store;
  X509_LOOKUP *lookup;
  POUND_CTX *pc;

  if (SLIST_EMPTY (&lst->ctx_head))
    {
      conf_error ("%s", "CRlist may only be used after Cert");
      return PARSER_FAIL;
    }
  lst->has_other = 1;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return PARSER_FAIL;

  SLIST_FOREACH (pc, &lst->ctx_head, next)
    {
      store = SSL_CTX_get_cert_store (pc->ctx);
      if ((lookup = X509_STORE_add_lookup (store, X509_LOOKUP_file ())) == NULL)
	{
	  conf_openssl_error ("X509_STORE_add_lookup");
	  return PARSER_FAIL;
	}

      if (X509_load_crl_file (lookup, tok->str, X509_FILETYPE_PEM) != 1)
	{
	  conf_openssl_error ("X509_load_crl_file failed");
	  return PARSER_FAIL;
	}

      X509_STORE_set_flags (store, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
    }

  return PARSER_OK;
}

static int
https_parse_nohttps11 (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  return assign_int_range (&lst->noHTTPS11, 0, 2);
}

static PARSER_TABLE https_parsetab[] = {
  { "End", parse_end },
  { "Address", assign_address, NULL, offsetof (LISTENER, addr) },
  { "Port", assign_port, NULL, offsetof (LISTENER, addr) },
  { "SocketFrom", listener_parse_socket_from },
  { "xHTTP", listener_parse_xhttp, NULL, offsetof (LISTENER, verb) },
  { "Client", assign_timeout, NULL, offsetof (LISTENER, to) },
  { "CheckURL", listener_parse_checkurl },
  { "Err404", assign_string_from_file, NULL, offsetof (LISTENER, err404) },
  { "Err413", assign_string_from_file, NULL, offsetof (LISTENER, err413) },
  { "Err414", assign_string_from_file, NULL, offsetof (LISTENER, err414) },
  { "Err500", assign_string_from_file, NULL, offsetof (LISTENER, err500) },
  { "Err501", assign_string_from_file, NULL, offsetof (LISTENER, err501) },
  { "Err503", assign_string_from_file, NULL, offsetof (LISTENER, err503) },
  { "MaxRequest", assign_LONG, NULL, offsetof (LISTENER, max_req) },
  { "HeadRemove", assign_matcher, NULL, offsetof (LISTENER, head_off) },
  { "RewriteLocation", parse_rewritelocation, NULL, offsetof (LISTENER, rewr_loc) },
  { "RewriteDestination", assign_bool, NULL, offsetof (LISTENER, rewr_dest) },
  { "LogLevel", assign_int, NULL, offsetof (LISTENER, log_level) },
  { "AddHeader", append_string_line, NULL, offsetof (LISTENER, add_head) },
  { "Service", parse_service, NULL, offsetof (LISTENER, services) },
  { "Cert", https_parse_cert },
  { "ClientCert", https_parse_client_cert },
  { "Disable", https_parse_disable },
  { "Ciphers", https_parse_ciphers },
  { "SSLHonorCipherOrder", https_parse_honor_cipher_order },
  { "SSLAllowClientRenegotiation", https_parse_allow_client_renegotiation },
  { "CAlist", https_parse_calist },
  { "VerifyList", https_parse_verifylist },
  { "CRLlist", https_parse_crlist },
  { "NoHTTPS11", https_parse_nohttps11 },
  { NULL }
};

static int
parse_listen_https (void *call_data, void *section_data)
{
  LISTENER *lst;
  LISTENER_HEAD *list_head = call_data;
  POUND_DEFAULTS *dfl = section_data;
  struct locus_range range;
  POUND_CTX *pc;
  struct stringbuf sb;

  if ((lst = listener_alloc (dfl)) == NULL)
    return PARSER_FAIL;

  lst->ssl_op_enable = SSL_OP_ALL;
#ifdef  SSL_OP_NO_COMPRESSION
  lst->ssl_op_enable |= SSL_OP_NO_COMPRESSION;
#endif
  lst->ssl_op_disable =
    SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION | SSL_OP_LEGACY_SERVER_CONNECT |
    SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS;

  if (parser_loop (https_parsetab, lst, section_data, &range))
    return PARSER_FAIL;

  if (check_addrinfo (&lst->addr, &range, "ListenHTTPS") != PARSER_OK)
    return PARSER_FAIL;

  if (SLIST_EMPTY (&lst->ctx_head))
    {
      conf_error_at_locus_range (&range, "Cert statement is missing");
      return PARSER_FAIL;
    }

#ifdef SSL_CTRL_SET_TLSEXT_SERVERNAME_CB
  if (!SLIST_EMPTY (&lst->ctx_head))
    {
      SSL_CTX *ctx = SLIST_FIRST (&lst->ctx_head)->ctx;
      if (!SSL_CTX_set_tlsext_servername_callback (ctx, SNI_server_name)
	  || !SSL_CTX_set_tlsext_servername_arg (ctx, &lst->ctx_head))
	{
	  conf_openssl_error ("can't set SNI callback");
	  return PARSER_FAIL;
	}
    }
#endif

  stringbuf_init (&sb);
  SLIST_FOREACH (pc, &lst->ctx_head, next)
    {
      SSL_CTX_set_app_data (pc->ctx, lst);
      SSL_CTX_set_mode (pc->ctx, SSL_MODE_AUTO_RETRY);
      SSL_CTX_set_options (pc->ctx, lst->ssl_op_enable);
      SSL_CTX_clear_options (pc->ctx, lst->ssl_op_disable);
      stringbuf_reset (&sb);
      stringbuf_printf (&sb, "%d-Pound-%ld", getpid (), random ());
      SSL_CTX_set_session_id_context (pc->ctx, (unsigned char *) sb.base,
				      sb.len);
      POUND_SSL_CTX_init (pc->ctx);
      SSL_CTX_set_info_callback (pc->ctx, SSLINFO_callback);
    }
  stringbuf_free (&sb);

  SLIST_PUSH (list_head, lst, next);
  return PARSER_OK;
}

static PARSER_TABLE top_level_parsetab[] = {
  { "Include", parse_include },
  { "User", assign_string, &user },
  { "Group", assign_string, &group },
  { "RootJail", assign_string, &root_jail },
  { "Daemon", assign_bool, &daemonize },
  { "Supervisor", assign_bool, &enable_supervisor },
  { "Threads", assign_unsigned, &numthreads },
  { "Grace", assign_timeout, &grace },
  { "LogFacility", assign_log_facility, NULL, offsetof (POUND_DEFAULTS, facility) },
  { "LogLevel", assign_log_level, NULL, offsetof (POUND_DEFAULTS, log_level) },
  { "Alive", assign_timeout, &alive_to },
  { "Client", assign_timeout, NULL, offsetof (POUND_DEFAULTS, clnt_to) },
  { "TimeOut", assign_timeout, NULL, offsetof (POUND_DEFAULTS, be_to) },
  { "WSTimeOut", assign_timeout, NULL, offsetof (POUND_DEFAULTS, ws_to) },
  { "ConnTO", assign_timeout, NULL, offsetof (POUND_DEFAULTS, be_connto) },
  { "IgnoreCase", assign_bool, NULL, offsetof (POUND_DEFAULTS, ignore_case) },
  { "ECDHCurve", parse_ECDHCurve },
  { "SSLEngine", parse_SSLEngine },
  { "Control", assign_string, &ctrl_name },
  { "Anonymise", int_set_one, &anonymise },
  { "Anonymize", int_set_one, &anonymise },
  { "Service", parse_service, &services },
  { "ListenHTTP", parse_listen_http, &listeners },
  { "ListenHTTPS", parse_listen_https, &listeners },
  { NULL }
};

int
parse_config_file (char const *file)
{
  int res = -1;
  POUND_DEFAULTS pound_defaults = {
    .facility = LOG_DAEMON,
    .log_level = 1,
    .clnt_to = 10,
    .be_to = 15,
    .ws_to = 600,
    .be_connto = 15,
    .ignore_case = 0
  };

  if (push_input (file) == 0)
    {
      res = parser_loop (top_level_parsetab, &pound_defaults, &pound_defaults, NULL);
      if (res == 0)
	{
	  if (cur_input)
	    exit (1);
	  log_facility = pound_defaults.facility;
	}
    }
  return res;
}

enum {
	F_OFF,
	F_ON,
	F_DFL
};

struct pound_feature
{
  char *name;
  char *descr;
  int enabled;
  void (*setfn) (int, char const *);
};

static struct pound_feature feature[] = {
  [FEATURE_DNS] = {
    .name = "dns",
    .descr = "resolve host names found in configuration file (default)",
    .enabled = F_ON
  },
  { NULL }
};

int
feature_is_set (int f)
{
  return feature[f].enabled;
}

static int
feature_set (char const *name)
{
  int i, enabled = F_ON;
  size_t len;
  char *val;

  if ((val = strchr (name, '=')) != NULL)
    {
      len = val - name;
      val++;
    }
  else
    len = strlen (name);

  if (val == NULL && strncmp (name, "no-", 3) == 0)
    {
      name += 3;
      len -= 3;
      enabled = F_OFF;
    }

  if (*name)
    {
      for (i = 0; feature[i].name; i++)
	{
	  if (strlen (feature[i].name) == len &&
	      memcmp (feature[i].name, name, len) == 0)
	    {
	      if (feature[i].setfn)
		feature[i].setfn (enabled, val);
	      else if (val)
		break;
	      feature[i].enabled = enabled;
	      return 0;
	    }
	}
    }
  return -1;
}

enum string_value_type
  {
    STRING_CONSTANT,
    STRING_INT,
    STRING_VARIABLE,
    STRING_FUNCTION,
    STRING_PRINTER
  };

struct string_value
{
  char const *kw;
  enum string_value_type type;
  union
  {
    char *s_const;
    char **s_var;
    int s_int;
    char const *(*s_func) (void);
    void (*s_print) (FILE *);
  } data;
};

#define VALUE_COLUMN 28

static void
print_string_values (struct string_value *values, FILE *fp)
{
  struct string_value *p;
  char const *val;

  for (p = values; p->kw; p++)
    {
      int n = fprintf (fp, "%s:", p->kw);
      if (n < VALUE_COLUMN)
	fprintf (fp, "%*s", VALUE_COLUMN-n, "");

      switch (p->type)
	{
	case STRING_CONSTANT:
	  val = p->data.s_const;
	  break;

	case STRING_INT:
	  fprintf (fp, "%d\n", p->data.s_int);
	  continue;

	case STRING_VARIABLE:
	  val = *p->data.s_var;
	  break;

	case STRING_FUNCTION:
	  val = p->data.s_func ();
	  break;

	case STRING_PRINTER:
	  p->data.s_print (fp);
	  fputc ('\n', fp);
	  continue;
	}

      fprintf (fp, "%s\n", val);
    }
}

static char const *
supervisor_status (void)
{
#if SUPERVISOR
  return "enabled";
#else
  return "disabled";
#endif
}

struct string_value pound_settings[] = {
  { "Configuration file",  STRING_CONSTANT, { .s_const = POUND_CONF } },
  { "PID file",   STRING_CONSTANT,  { .s_const = POUND_PID } },
  { "Supervisor", STRING_FUNCTION, { .s_func = supervisor_status } },
  { "Buffer size",STRING_INT, { .s_int = MAXBUF } },
#if OPENSSL_VERSION_MAJOR < 3
  { "DH bits",         STRING_INT, { .s_int = DH_LEN } },
  { "RSA regeneration interval", STRING_INT, { .s_int = T_RSA_KEYS } },
#endif
  { NULL }
};

static int copyright_year = 2022;
void
print_version (void)
{
  printf ("%s (%s) %s\n", progname, PACKAGE_NAME, PACKAGE_VERSION);
  printf ("Copyright (C) 2002-2010 Apsis GmbH\n");
  printf ("Copyright (C) 2018-%d Sergey Poznyakoff\n", copyright_year);
  printf ("\
License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>\n\
This is free software: you are free to change and redistribute it.\n\
There is NO WARRANTY, to the extent permitted by law.\n\
");
  printf ("\nBuilt-in defaults:\n\n");
  print_string_values (pound_settings, stdout);
}

void
print_help (void)
{
  int i;

  printf ("usage: %s [-Vchv] [-W [no-]FEATURE] [-f FILE] [-p FILE]\n", progname);
  printf ("HTTP/HTTPS reverse-proxy and load-balancer\n");
  printf ("\nOptions are:\n\n");
  printf ("   -c               check configuration file syntax and exit\n");
  printf ("   -f FILE          read configuration from FILE\n");
  printf ("                    (default: %s)\n", POUND_CONF);
  printf ("   -p FILE          write PID to FILE\n");
  printf ("                    (default: %s)\n", POUND_PID);
  printf ("   -V               print program version, compilation settings, and exit\n");
  printf ("   -v               verbose mode\n");
  printf ("   -W [no-]FEATURE  enable or disable optional feature\n");
  printf ("\n");
  printf ("FEATUREs are:\n");
  for (i = 0; feature[i].name; i++)
    printf ("   %-16s %s\n", feature[i].name, feature[i].descr);
  printf ("\n");
  printf ("Report bugs and suggestions to <%s>\n", PACKAGE_BUGREPORT);
#ifdef PACKAGE_URL
  printf ("%s home page: <%s>\n", PACKAGE_NAME, PACKAGE_URL);
#endif
}

void
config_parse (int argc, char **argv)
{
  int c;
  int check_only = 0;
  char *conf_name = POUND_CONF;

  if ((progname = strrchr (argv[0], '/')) != NULL)
    progname++;
  else
    progname = argv[0];
  while ((c = getopt (argc, argv, "cf:hp:VvW:")) > 0)
    switch (c)
      {
      case 'c':
	check_only = 1;
	break;

      case 'f':
	conf_name = optarg;
	break;

      case 'h':
	print_help ();
	exit (0);

      case 'p':
	pid_name = optarg;
	break;

      case 'V':
	print_version ();
	exit (0);

      case 'v':
	print_log = 1;
	break;

      case 'W':
	if (feature_set (optarg))
	  {
	    logmsg (LOG_ERR, "invalid feature name: %s", optarg);
	    exit (1);
	  }
	break;

      default:
	exit (1);
      }

  if (optind < argc)
    {
      logmsg (LOG_ERR, "unknown extra arguments (%s...)", argv[optind]);
      exit (1);
    }

  if (parse_config_file (conf_name))
    exit (1);
  name_list_free ();

  if (check_only)
    {
      logmsg (LOG_INFO, "Config file %s is OK", conf_name);
      exit (0);
    }

  if (SLIST_EMPTY (&listeners))
    {
      logmsg (LOG_ERR, "no listeners defined");
      exit (1);
    }
}