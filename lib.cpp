/****************************************************************
Copyright (C) Lucent Technologies 1997
All Rights Reserved

Permission to use, copy, modify, and distribute this software and
its documentation for any purpose and without fee is hereby
granted, provided that the above copyright notice appear in all
copies and that both that the copyright notice and this
permission notice and warranty disclaimer appear in supporting
documentation, and that the name Lucent Technologies or any of
its entities not be used in advertising or publicity pertaining
to distribution of the software without specific, written prior
permission.

LUCENT DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS.
IN NO EVENT SHALL LUCENT OR ANY OF ITS ENTITIES BE LIABLE FOR ANY
SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE.
****************************************************************/

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include "awk.h"
#include "ytab.h"
#include <awkerr.h>

FILE*   infile = NULL;
char*   record;
int     recsize = RECSIZE;
char*   fields;
int     fieldssize = RECSIZE;

Cell**  fldtab;  /* pointers to Cells */
char    inputFS[100] = " ";

#define  MAXFLD  2
int     nfields = MAXFLD;  /* last allocated slot for $i */

int     donefld;  /* 1 = implies rec broken into fields */
int     donerec;  /* 1 = record is valid (no flds have changed) */

int     lastfld = 0;  /* last used field */
int     argno = 1;  /* current input argument number */
extern  Awkfloat *ARGC;

static Cell dollar0 = { OCELL, CFLD, NULL, NULL, 0.0, REC | STR | DONTFREE };
static Cell dollar1 = { OCELL, CFLD, NULL, NULL, 0.0, FLD | STR | DONTFREE };

static int	refldbld (const char *, const char *);

void recinit (unsigned int n)
{
  if ((record = (char *)malloc (n)) == NULL
   || (fields = (char *)malloc (n + 1)) == NULL
   || (fldtab = (Cell **)malloc ((nfields + 1) * sizeof (Cell *))) == NULL
   || (fldtab[0] = (Cell *)malloc (sizeof (Cell))) == NULL)
    FATAL (AWK_ERR_NOMEM, "out of space for $0 and fields");
  *fldtab[0] = dollar0;
  fldtab[0]->sval = record;
  fldtab[0]->nval = tostring ("0");
  makefields (1, nfields);
}

/// Create $n1..$n2 inclusive
void makefields (int n1, int n2)
{
  char temp[50];
  int i;

  for (i = n1; i <= n2; i++)
  {
    fldtab[i] = (Cell *)malloc (sizeof (struct Cell));
    if (fldtab[i] == NULL)
      FATAL (AWK_ERR_NOMEM, "out of space in makefields %d", i);
    *fldtab[i] = dollar1;
    sprintf (temp, "%d", i);
    fldtab[i]->nval = tostring (temp);
  }
}

/// Find first filename argument and open the file
void initgetrec (void)
{
  int i;
  char *p;

  for (i = 1; i < *ARGC; i++)
  {
    p = getargv (i); /* find 1st real filename */
    if (p == NULL || *p == '\0')
    {  /* deleted or zapped */
      argno++;
      continue;
    }
    if (!isclvar (p))
    {
      setsval (lookup ("FILENAME", symtab), p);
      return;
    }
    setclvar (p);  /* a commandline assignment before filename */
    argno++;
  }
  infile = stdin;    /* no filenames, so use stdin */
}

/// Get next input record
int getrec (char **pbuf, int *pbufsize, int isrecord)
{
  /* note: cares whether buf == record */
  int c;
  char *buf = *pbuf;
  uschar saveb0;
  int bufsize = *pbufsize, savebufsize = bufsize;
  char* file = 0;

  dprintf ("RS=<%s>, FS=<%s>, ARGC=%g, FILENAME=%s\n",
    *RS, *FS, *ARGC, *FILENAME);
  if (isrecord)
  {
    donefld = 0;
    donerec = 1;
  }
  saveb0 = buf[0];
  buf[0] = 0;
  while (argno < *ARGC || infile == stdin)
  {
    dprintf ("argno=%d, file=|%s|\n", argno, NN(file));
    if (infile == NULL)
    {
      /* have to open a new file */
      file = getargv (argno);
      if (file == NULL || *file == '\0')
      {
        /* deleted or zapped */
        argno++;
        continue;
      }
      if (isclvar (file))
      {
        /* a var=value arg */
        setclvar (file);
        argno++;
        continue;
      }
      *FILENAME = file;
      dprintf ("opening file %s\n", file);
      if (*file == '-' && *(file + 1) == '\0')
        infile = stdin;
      else if ((infile = fopen (file, "r")) == NULL)
        FATAL (AWK_ERR_INFILE, "can't open file %s", file);
      setfval (fnrloc, 0.0);
    }
    c = readrec (&buf, &bufsize, infile);
    if (c != 0 || buf[0] != '\0')
    {
      /* normal record */
      if (isrecord)
      {
        if (freeable (fldtab[0]))
          xfree (fldtab[0]->sval);
        fldtab[0]->sval = buf;  /* buf == record */
        fldtab[0]->tval = REC | STR | DONTFREE;
        if (is_number (fldtab[0]->sval))
        {
          fldtab[0]->fval = atof (fldtab[0]->sval);
          fldtab[0]->tval |= NUM;
        }
      }
      setfval (nrloc, nrloc->fval + 1);
      setfval (fnrloc, fnrloc->fval + 1);
      *pbuf = buf;
      *pbufsize = bufsize;
      return 1;
    }
    /* EOF arrived on this file; set up next */
    if (infile != stdin)
      fclose (infile);
    infile = NULL;
    *FILENAME = 0;
    argno++;
  }
  buf[0] = saveb0;
  *pbuf = buf;
  *pbufsize = savebufsize;
  return 0;  /* true end of file */
}

void nextfile (void)
{
  if (infile != NULL && infile != stdin)
    fclose (infile);
  infile = NULL;
  argno++;
}

/// Read one record into buf
int readrec (char **pbuf, int *pbufsize, FILE *inf)
{
  int sep, c;
  char *rr, *buf = *pbuf;
  int bufsize = *pbufsize;

  if (strlen (*FS) >= sizeof (inputFS))
    FATAL (AWK_ERR_LIMIT, "field separator %.10s... is too long", *FS);
  /*fflush(stdout); avoids some buffering problem but makes it 25% slower*/
  strcpy (inputFS, *FS);  /* for subsequent field splitting */
  if ((sep = **RS) == 0)
  {
    sep = '\n';
    while ((c = getc (inf)) == '\n' && c != EOF)  /* skip leading \n's */
      ;
    if (c != EOF)
      ungetc (c, inf);
  }
  for (rr = buf; ; )
  {
    for (; (c = getc (inf)) != sep && c != EOF; )
    {
      if (rr - buf + 1 > bufsize)
        if (!adjbuf (&buf, &bufsize, 1 + rr - buf, recsize, &rr))
          FATAL (AWK_ERR_NOMEM, "input record `%.30s...' too long", buf);
      *rr++ = c;
    }
    if (**RS == sep || c == EOF)
      break;

    /*
      **RS != sep and sep is '\n' and c == '\n'
      This is the case where RS = 0 and records are separated by two
      consecutive \n
    */
    if ((c = getc (inf)) == '\n' || c == EOF) /* 2 in a row */
      break;
    if (!adjbuf (&buf, &bufsize, 2 + rr - buf, recsize, &rr))
      FATAL (AWK_ERR_NOMEM, "input record `%.30s...' too long", buf);
    *rr++ = '\n';
    *rr++ = c;
  }
#if 0
  //Not needed; buffer has been adjusted inside the loop
  if (!adjbuf (&buf, &bufsize, 1 + rr - buf, recsize, &rr))
    FATAL ("input record `%.30s...' too long", buf);
#endif
  *rr = 0;
  dprintf ("readrec saw <%s>, returns %d\n", buf, c == EOF && rr == buf ? 0 : 1);
  *pbuf = buf;
  *pbufsize = bufsize;
  return c == EOF && rr == buf ? 0 : 1;
}

/// Get ARGV[n]
char *getargv (int n)
{
  Cell *x;
  char *s, temp[50];
  extern Array *ARGVtab;

  sprintf (temp, "%d", n);
  if (lookup (temp, ARGVtab) == NULL)
    return NULL;
  x = setsymtab (temp, "", 0.0, STR, ARGVtab);
  s = getsval (x);
  dprintf ("getargv(%d) returns |%s|\n", n, s);
  return s;
}

/*!
  Command line variable.
  Set var=value from s

  Assumes input string has correct format.
*/
void setclvar (const char *s)
{
  const char *p;
  Cell *q;

  for (p = s; *p != '='; p++)
    ;
  p = qstring (p+1, '\0');
  s = qstring (s, '=');
  q = setsymtab (s, p, 0.0, STR, symtab);
  setsval (q, p);
  if (is_number (q->sval))
  {
    q->fval = atof (q->sval);
    q->tval |= NUM;
  }
  dprintf ("command line set %s to |%s|\n", s, p);
}

/// Create fields from current record
void fldbld (void)
{
  /* this relies on having fields[] the same length as $0 */
  /* the fields are all stored in this one array with \0's */
  /* possibly with a final trailing \0 not associated with any field */
  char *r, *fr, sep;
  Cell *p;
  int i, j, n;

  if (donefld)
    return;
  if (!isstr (fldtab[0]))
    getsval (fldtab[0]);
  r = fldtab[0]->sval;
  n = strlen (r);
  if (n > fieldssize)
  {
    xfree (fields);
    if ((fields = (char *)malloc (n + 2)) == NULL) /* possibly 2 final \0s */
      FATAL (AWK_ERR_NOMEM, "out of space for fields in fldbld %d", n);
    fieldssize = n;
  }
  fr = fields;
  i = 0;  /* number of fields accumulated here */
  strcpy (inputFS, *FS);
  if (strlen (inputFS) > 1)
  {
    /* it's a regular expression */
    i = refldbld (r, inputFS);
  }
  else if ((sep = *inputFS) == ' ')
  {
    /* default whitespace */
    for (i = 0; ; )
    {
      while (*r == ' ' || *r == '\t' || *r == '\n')
        r++;
      if (*r == 0)
        break;
      i++;
      if (i > nfields)
        growfldtab (i);
      if (freeable (fldtab[i]))
        xfree (fldtab[i]->sval);
      fldtab[i]->sval = fr;
      fldtab[i]->tval = FLD | STR | DONTFREE;
      do
        *fr++ = *r++;
      while (*r != ' ' && *r != '\t' && *r != '\n' && *r != '\0');
      *fr++ = 0;
    }
    *fr = 0;
  }
  else if ((sep = *inputFS) == 0)
  {
    /* new: FS="" => 1 char/field */
    for (i = 0; *r != 0; r++)
    {
      char buf[2];
      i++;
      if (i > nfields)
        growfldtab (i);
      if (freeable (fldtab[i]))
        xfree (fldtab[i]->sval);
      buf[0] = *r;
      buf[1] = 0;
      fldtab[i]->sval = tostring (buf);
      fldtab[i]->tval = FLD | STR;
    }
    *fr = 0;
  }
  else if (*r != 0)
  {
    /* if 0, it's a null field */

 /* subtlecase : if length(FS) == 1 && length(RS > 0)
  * \n is NOT a field separator (cf awk book 61,84).
  * this variable is tested in the inner while loop.
  */
    int rtest = '\n';  /* normal case */
    if (strlen (*RS) > 0)
      rtest = '\0';
    for (;;)
    {
      i++;
      if (i > nfields)
        growfldtab (i);
      if (freeable (fldtab[i]))
        xfree (fldtab[i]->sval);
      fldtab[i]->sval = fr;
      fldtab[i]->tval = FLD | STR | DONTFREE;
      while (*r != sep && *r != rtest && *r != '\0')  /* \n is always a separator */
        *fr++ = *r++;
      *fr++ = 0;
      if (*r++ == 0)
        break;
    }
    *fr = 0;
  }
  if (i > nfields)
    FATAL (AWK_ERR_LIMIT, "record `%.30s...' has too many fields; can't happen", r);
  cleanfld (i + 1, lastfld);  /* clean out junk from previous record */
  lastfld = i;
  donefld = 1;
  for (j = 1; j <= lastfld; j++)
  {
    p = fldtab[j];
    if (is_number (p->sval))
    {
      p->fval = atof (p->sval);
      p->tval |= NUM;
    }
  }
  setfval (nfloc, (Awkfloat)lastfld);
  donerec = 1; /* restore */
#ifndef NDEBUG
  if (dbg)
  {
    for (j = 0; j <= lastfld; j++)
    {
      p = fldtab[j];
      printf ("field %d (%s): |%s|\n", j, p->nval, p->sval);
    }
  }
#endif
}

/// Clean out fields n1 .. n2 inclusive
void cleanfld (int n1, int n2)
{
  /* nvals remain intact */
  Cell *p;
  int i;

  for (i = n1; i <= n2; i++)
  {
    p = fldtab[i];
    if (freeable (p))
      xfree (p->sval);
    p->sval = 0;
    p->tval = FLD | STR | DONTFREE;
  }
}

/// Add field n after end of existing lastfld
void newfld (int n)
{
  if (n > nfields)
    growfldtab (n);
  cleanfld (lastfld + 1, n);
  lastfld = n;
  setfval (nfloc, (Awkfloat)n);
}

/// Set lastfld cleaning fldtab cells if necessary
void setlastfld (int n)
{
  if (n > nfields)
    growfldtab (n);

  if (lastfld < n)
    cleanfld (lastfld + 1, n);
  else
    cleanfld (n + 1, lastfld);

  lastfld = n;
}

/// Get nth field
Cell *fieldadr (int n)
{
  if (n < 0)
    FATAL (AWK_ERR_ARG, "trying to access out of range field %d", n);
  if (n > nfields)  /* fields after NF are empty */
    growfldtab (n);  /* but does not increase NF */
  return(fldtab[n]);
}

/// Make new fields up to at least $n
void growfldtab (int n)
{
  int nf = 2 * nfields;
  size_t s;

  if (n > nf)
    nf = n;
  s = (nf + 1) * (sizeof (struct Cell *));  /* freebsd: how much do we need? */
  if (s / sizeof (struct Cell *) - 1 == nf) /* didn't overflow */
    fldtab = (Cell **)realloc (fldtab, s);
  else          /* overflow sizeof int */
    xfree (fldtab);  /* make it null */
  if (fldtab == NULL)
    FATAL (AWK_ERR_NOMEM, "out of space creating %d fields", nf);
  makefields (nfields + 1, nf);
  nfields = nf;
}

/// Build fields from reg expr in FS
int refldbld (const char *rec, const char *fs)
{
  /* this relies on having fields[] the same length as $0 */
  /* the fields are all stored in this one array with \0's */
  char *fr;
  int i, tempstat, n;
  fa *pfa;

  n = strlen (rec);
  if (n > fieldssize)
  {
    xfree (fields);
    if ((fields = (char *)malloc (n + 1)) == NULL)
      FATAL (AWK_ERR_NOMEM, "out of space for fields in refldbld %d", n);
    fieldssize = n;
  }
  fr = fields;
  *fr = '\0';
  if (*rec == '\0')
    return 0;
  pfa = makedfa (fs, 1);
  dprintf ("into refldbld, rec = <%s>, pat = <%s>\n", rec, fs);
  tempstat = pfa->initstat;
  for (i = 1; ; i++)
  {
    if (i > nfields)
      growfldtab (i);
    if (freeable (fldtab[i]))
      xfree (fldtab[i]->sval);
    fldtab[i]->tval = FLD | STR | DONTFREE;
    fldtab[i]->sval = fr;
    dprintf ("refldbld: i=%d\n", i);
    if (nematch (pfa, rec))
    {
      pfa->initstat = 2;  /* horrible coupling to b.c */
      dprintf ("match %s (%d chars)\n", patbeg, patlen);
      strncpy (fr, rec, patbeg - rec);
      fr += patbeg - rec + 1;
      *(fr - 1) = '\0';
      rec = patbeg + patlen;
    }
    else
    {
      dprintf ("no match %s\n", rec);
      strcpy (fr, rec);
      pfa->initstat = tempstat;
      break;
    }
  }
  return i;
}

/// Create $0 from $1..$NF if necessary
void recbld (void)
{
  int i;
  char *r, *p;

  if (donerec == 1)
    return;
  r = record;
  for (i = 1; i <= *NF; i++)
  {
    p = getsval (fldtab[i]);
    if (!adjbuf (&record, &recsize, 1 + strlen (p) + r - record, recsize, &r))
      FATAL (AWK_ERR_NOMEM, "created $0 `%.30s...' too long", record);
    while ((*r = *p++) != 0)
      r++;
    if (i < *NF)
    {
      if (!adjbuf (&record, &recsize, 2 + strlen (*OFS) + r - record, recsize, &r))
        FATAL (AWK_ERR_NOMEM, "created $0 `%.30s...' too long", record);
      for (p = *OFS; (*r = *p++) != 0; )
        r++;
    }
  }
  if (!adjbuf (&record, &recsize, 2 + r - record, recsize, &r))
    FATAL (AWK_ERR_NOMEM, "built giant record `%.30s...'", record);
  *r = '\0';
  dprintf ("in recbld inputFS=%s, fldtab[0]=%p\n", inputFS, (void*)fldtab[0]);

  if (freeable (fldtab[0]))
    xfree (fldtab[0]->sval);
  fldtab[0]->tval = REC | STR | DONTFREE;
  fldtab[0]->sval = record;

  dprintf ("in recbld inputFS=%s, fldtab[0]=%p\n", inputFS, (void*)fldtab[0]);
  dprintf ("recbld = |%s|\n", record);
  donerec = 1;
}

int  errorflag = 0;


double errcheck (double x, const char *s)
{
  if (errno == EDOM)
  {
    errno = 0;
    WARNING ("%s argument out of domain", s);
    x = 1;
  }
  else if (errno == ERANGE)
  {
    errno = 0;
    WARNING ("%s result out of range", s);
    x = 1;
  }
  return x;
}

/*!
  Checks if s is a command line variable assignment 
  of the form var=something
*/
int isclvar (const char *s)
{
  const char *os = s;

  if (!isalpha ((uschar)*s) && *s != '_')
    return 0;
  for (; *s; s++)
    if (!(isalnum ((uschar)*s) || *s == '_'))
      break;
  return *s == '=' && s > os && *(s + 1) != '=';
}

/* strtod is supposed to be a proper test of what's a valid number */
/* appears to be broken in gcc on linux: thinks 0x123 is a valid FP number */
/* wrong: violates 4.10.1.4 of ansi C standard */

#include <math.h>
int is_number (const char *s)
{
  double r;
  char *ep;
  errno = 0;
  r = strtod (s, &ep);
  if (ep == s || r == HUGE_VAL || errno == ERANGE)
    return 0;
  while (*ep == ' ' || *ep == '\t' || *ep == '\n')
    ep++;
  if (*ep == '\0')
    return 1;
  else
    return 0;
}
