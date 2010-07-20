/****************************************************************************
 *           MatchBuf manipulation and match reporting facilities           *
 *                           Author: Herve Pages                            *
 ****************************************************************************/
#include "Biostrings.h"
#include "IRanges_interface.h"

static int debug = 0;

SEXP debug_match_reporting()
{
#ifdef DEBUG_BIOSTRINGS
	debug = !debug;
	Rprintf("Debug mode turned %s in file %s\n",
		debug ? "on" : "off", __FILE__);
#else
	Rprintf("Debug mode not available in file %s\n", __FILE__);
#endif
	return R_NilValue;
}

int _get_match_storing_code(const char *ms_mode)
{
	if (strcmp(ms_mode, "MATCHES_AS_NULL") == 0)
		return MATCHES_AS_NULL;
	if (strcmp(ms_mode, "MATCHES_AS_WHICH") == 0)
		return MATCHES_AS_WHICH;
	if (strcmp(ms_mode, "MATCHES_AS_COUNTS") == 0)
		return MATCHES_AS_COUNTS;
	if (strcmp(ms_mode, "MATCHES_AS_STARTS") == 0)
		return MATCHES_AS_STARTS;
	if (strcmp(ms_mode, "MATCHES_AS_ENDS") == 0)
		return MATCHES_AS_ENDS;
	if (strcmp(ms_mode, "MATCHES_AS_RANGES") == 0)
		return MATCHES_AS_RANGES;
	if (strcmp(ms_mode, "MATCHES_AS_NORMALRANGES") == 0)
		return MATCHES_AS_NORMALRANGES;
	if (strcmp(ms_mode, "MATCHES_AS_COVERAGE") == 0)
		return MATCHES_AS_COVERAGE;
	error("Biostrings internal error in _get_match_storing_code(): "
	      "\"%s\": unknown match storing mode", ms_mode);
	return -1; /* keeps gcc -Wall happy */
}


/****************************************************************************
 * MatchBuf manipulation.
 */

MatchBuf _new_MatchBuf(int ms_code, int nPSpair)
{
	int count_only;
	static MatchBuf match_buf;

	if (ms_code != MATCHES_AS_NULL
	 && ms_code != MATCHES_AS_WHICH
	 && ms_code != MATCHES_AS_COUNTS
	 && ms_code != MATCHES_AS_STARTS
	 && ms_code != MATCHES_AS_ENDS
	 && ms_code != MATCHES_AS_RANGES)
		error("Biostrings internal error in _new_MatchBuf(): ",
		      "%d: unsupported match storing code", ms_code);
	count_only = ms_code == MATCHES_AS_WHICH ||
		     ms_code == MATCHES_AS_COUNTS;
	match_buf.ms_code = ms_code;
	match_buf.PSlink_ids = new_IntAE(0, 0, 0);
	match_buf.match_counts = new_IntAE(nPSpair, nPSpair, 0);
	if (count_only) {
		/* By setting 'buflength' to -1 we indicate that these
		   buffers must not be used */
		match_buf.match_starts.buflength = -1;
		match_buf.match_widths.buflength = -1;
	} else {
		match_buf.match_starts = new_IntAEAE(nPSpair, nPSpair);
		match_buf.match_widths = new_IntAEAE(nPSpair, nPSpair);
	}
	return match_buf;
}

void _MatchBuf_report_match(MatchBuf *match_buf,
		int PSpair_id, int start, int width)
{
	IntAE *PSlink_ids, *count_buf, *start_buf, *width_buf;

	PSlink_ids = &(match_buf->PSlink_ids);
	count_buf = &(match_buf->match_counts);
	if (count_buf->elts[PSpair_id]++ == 0)
		IntAE_insert_at(PSlink_ids, PSlink_ids->nelt, PSpair_id);
	if (match_buf->match_starts.buflength != -1) {
		start_buf = match_buf->match_starts.elts + PSpair_id;
		IntAE_insert_at(start_buf, start_buf->nelt, start);
	}
	if (match_buf->match_widths.buflength != -1) {
		width_buf = match_buf->match_widths.elts + PSpair_id;
		IntAE_insert_at(width_buf, width_buf->nelt, width);
	}
	return;
}

void _MatchBuf_flush(MatchBuf *match_buf)
{
	int i;
	const int *PSlink_id;

	for (i = 0, PSlink_id = match_buf->PSlink_ids.elts;
	     i < match_buf->PSlink_ids.nelt;
	     i++, PSlink_id++)
	{
		match_buf->match_counts.elts[*PSlink_id] = 0;
		if (match_buf->match_starts.buflength != -1)
			match_buf->match_starts.elts[*PSlink_id].nelt = 0;
		if (match_buf->match_widths.buflength != -1)
			match_buf->match_widths.elts[*PSlink_id].nelt = 0;
	}
	match_buf->PSlink_ids.nelt = 0;
	return;
}

void _MatchBuf_append_and_flush(MatchBuf *match_buf1,
		MatchBuf *match_buf2, int view_offset)
{
	int i;
	const int *PSlink_id;
	IntAE *start_buf1, *start_buf2, *width_buf1, *width_buf2;

	if (match_buf1->ms_code == MATCHES_AS_NULL
	 || match_buf2->ms_code == MATCHES_AS_NULL)
		return;
	if (match_buf1->match_counts.nelt != match_buf2->match_counts.nelt
	 || match_buf1->ms_code != match_buf2->ms_code)
		error("Biostrings internal error in "
		      "_MatchBuf_append_and_flush(): "
		      "buffers are incompatible");
	for (i = 0, PSlink_id = match_buf2->PSlink_ids.elts;
	     i < match_buf2->PSlink_ids.nelt;
	     i++, PSlink_id++)
	{
		if (match_buf1->match_counts.elts[*PSlink_id] == 0)
			IntAE_insert_at(&(match_buf1->PSlink_ids),
				match_buf1->PSlink_ids.nelt, *PSlink_id);
		match_buf1->match_counts.elts[*PSlink_id] +=
			match_buf2->match_counts.elts[*PSlink_id];
		if (match_buf1->match_starts.buflength != -1) {
			start_buf1 = match_buf1->match_starts.elts + *PSlink_id;
			start_buf2 = match_buf2->match_starts.elts + *PSlink_id;
			IntAE_append_shifted_vals(start_buf1,
			    start_buf2->elts, start_buf2->nelt, view_offset);
		}
		if (match_buf1->match_widths.buflength != -1) {
			width_buf1 = match_buf1->match_widths.elts + *PSlink_id;
			width_buf2 = match_buf2->match_widths.elts + *PSlink_id;
			IntAE_append(width_buf1,
				width_buf2->elts, width_buf2->nelt);
		}
	}
	_MatchBuf_flush(match_buf2);
	return;
}

SEXP _MatchBuf_which_asINTEGER(const MatchBuf *match_buf)
{
	SEXP ans;
	int i;

	PROTECT(ans = new_INTEGER_from_IntAE(&(match_buf->PSlink_ids)));
	sort_int_array(INTEGER(ans), LENGTH(ans), 0);
	for (i = 0; i < LENGTH(ans); i++)
		INTEGER(ans)[i]++;
	UNPROTECT(1);
	return ans;
}

SEXP _MatchBuf_counts_asINTEGER(const MatchBuf *match_buf)
{
	return new_INTEGER_from_IntAE(&(match_buf->match_counts));
}

SEXP _MatchBuf_starts_asLIST(const MatchBuf *match_buf)
{
	if (match_buf->match_starts.buflength == -1)
		error("Biostrings internal error: _MatchBuf_starts_asLIST() "
		      "was called in the wrong context");
	return new_LIST_from_IntAEAE(&(match_buf->match_starts), 1);
}

static SEXP _MatchBuf_starts_toEnvir(const MatchBuf *match_buf, SEXP env)
{
	if (match_buf->match_starts.buflength == -1)
		error("Biostrings internal error: _MatchBuf_starts_toEnvir() "
		      "was called in the wrong context");
	return IntAEAE_toEnvir(&(match_buf->match_starts), env, 1);
}

SEXP _MatchBuf_ends_asLIST(const MatchBuf *match_buf)
{
	if (match_buf->match_starts.buflength == -1
	 || match_buf->match_widths.buflength == -1)
		error("Biostrings internal error: _MatchBuf_ends_asLIST() "
		      "was called in the wrong context");
	IntAEAE_sum_and_shift(&(match_buf->match_starts),
			      &(match_buf->match_widths), -1);
	return new_LIST_from_IntAEAE(&(match_buf->match_starts), 1);
}

static SEXP _MatchBuf_ends_toEnvir(const MatchBuf *match_buf, SEXP env)
{
	if (match_buf->match_starts.buflength == -1
	 || match_buf->match_widths.buflength == -1)
		error("Biostrings internal error: _MatchBuf_ends_toEnvir() "
		      "was called in the wrong context");
	IntAEAE_sum_and_shift(&(match_buf->match_starts),
			      &(match_buf->match_widths), -1);
	return IntAEAE_toEnvir(&(match_buf->match_starts), env, 1);
}

SEXP _MatchBuf_as_MIndex(const MatchBuf *match_buf)
{
	error("_MatchBuf_as_MIndex(): IMPLEMENT ME!");
	return R_NilValue;
}

SEXP _MatchBuf_as_SEXP(const MatchBuf *match_buf, SEXP env)
{
	switch (match_buf->ms_code) {
	    case MATCHES_AS_NULL:
		return R_NilValue;
	    case MATCHES_AS_WHICH:
		return _MatchBuf_which_asINTEGER(match_buf);
	    case MATCHES_AS_COUNTS:
		return _MatchBuf_counts_asINTEGER(match_buf);
	    case MATCHES_AS_STARTS:
		if (env != R_NilValue)
			return _MatchBuf_starts_toEnvir(match_buf, env);
		return _MatchBuf_starts_asLIST(match_buf);
	    case MATCHES_AS_ENDS:
		if (env != R_NilValue)
			return _MatchBuf_ends_toEnvir(match_buf, env);
		return _MatchBuf_ends_asLIST(match_buf);
	    case MATCHES_AS_RANGES:
		return _MatchBuf_as_MIndex(match_buf);
	}
	error("Biostrings internal error in _MatchBuf_as_SEXP(): "
	      "unknown 'match_buf->ms_code' value %d", match_buf->ms_code);
	return R_NilValue;
}


/****************************************************************************
 * Internal match buffer instance with a simple API.
 */

static MatchBuf internal_match_buf;
int active_PSpair_id;
static int match_shift;

void _init_match_reporting(const char *ms_mode, int nPSpair)
{
	int ms_code;

	ms_code = _get_match_storing_code(ms_mode);
	internal_match_buf = _new_MatchBuf(ms_code, nPSpair);
	active_PSpair_id = 0;
	match_shift = 0;
	return;
}

void _set_active_PSpair(int PSpair_id)
{
	active_PSpair_id = PSpair_id;
	return;
}

void _set_match_shift(int shift)
{
	match_shift = shift;
}

void _report_match(int start, int width)
{
	start += match_shift;
	_MatchBuf_report_match(&internal_match_buf,
			active_PSpair_id, start, width);
	return;
}

/* Drops reported matches for all PSpairs! */
void _drop_reported_matches()
{
	_MatchBuf_flush(&internal_match_buf);
	return;
}

int _get_match_count()
{
	return internal_match_buf.match_counts.elts[active_PSpair_id];
}

SEXP _reported_matches_asSEXP()
{
	SEXP start, width, ans;

	switch (internal_match_buf.ms_code) {
	    case MATCHES_AS_NULL:
		return R_NilValue;
	    case MATCHES_AS_COUNTS:
	    case MATCHES_AS_WHICH:
		return ScalarInteger(_get_match_count());
	    case MATCHES_AS_RANGES:
		PROTECT(start = new_INTEGER_from_IntAE(
		  internal_match_buf.match_starts.elts + active_PSpair_id));
		PROTECT(width = new_INTEGER_from_IntAE(
		  internal_match_buf.match_widths.elts + active_PSpair_id));
		PROTECT(ans = new_IRanges("IRanges", start, width, R_NilValue));
		UNPROTECT(3);
		return ans;
	}
	error("Biostrings internal error in _reported_matches_asSEXP(): "
	      "invalid 'internal_match_buf.ms_code' value %d",
	      internal_match_buf.ms_code);
	return R_NilValue;
}

MatchBuf *_get_internal_match_buf()
{
	return &internal_match_buf;
}

