/****************************************************************************
 *                       Read/write FASTA/FASTQ files                       *
 *                                 --------                                 *
 *                            Author: H. Pag\`es                            *
 ****************************************************************************/
#include "Biostrings.h"
#include "XVector_interface.h"
#include "S4Vectors_interface.h"

#include <math.h>  /* for llround */


#define IOBUF_SIZE 20002
static char errmsg_buf[200];

static int has_prefix(const char *s, const char *prefix)
{
	int i = 0;
	char c;

	while ((c = prefix[i]) != '\0') {
		if (s[i] != c)
			return 0;
		i++;
	}
	return 1;
}

/* TODO: Move this to XVector (together with _copy_CHARSXP_to_Chars_holder). */
static void copy_Chars_holder(Chars_holder *dest, const Chars_holder *src,
			      const int *lkup, int lkup_length)
{
	char *dest_ptr;

	/* dest->ptr is a (const char *) so we need to cast it to (char *)
	   before we can write to it */
	dest_ptr = (char *) dest->ptr;
	if (lkup == NULL) {
		memcpy(dest_ptr, src->ptr, dest->length);
	} else {
		Ocopy_bytes_to_i1i2_with_lkup(0, dest->length - 1,
			dest_ptr, dest->length,
			src->ptr, src->length,
			lkup, lkup_length);
	}
	return;
}


/****************************************************************************
 *  A. FASTA FORMAT                                                         *
 ****************************************************************************/

/*
 * Even if the standard FASTA markup is made of 1-letter sympols, we want to
 * be ready to support longer sympols just in case.
 */
static const char *FASTA_comment_markup = ";", *FASTA_desc_markup = ">";


/****************************************************************************
 * Reading FASTA files.
 */

typedef struct fasta_loader {
	const int *lkup;
	int lkup_length;
	void (*load_desc_line)(struct fasta_loader *loader,
			       int recno, long long int offset,
			       const Chars_holder *desc_line);
	void (*load_empty_seq)(struct fasta_loader *loader);
	void (*load_seq_data)(struct fasta_loader *loader,
			      const Chars_holder *seq_data);
	int nrec;
	void *ext;  /* loader extension (optional) */
} FASTAloader;

/*
 * The FASTA INDEX loader.
 * Used in parse_FASTA_file() to build the FASTA index only.
 */

typedef struct index_fasta_loader_ext {
	IntAE *recno_buf;
	LLongAE *offset_buf;
	CharAEAE *desc_buf;
	IntAE *seqlength_buf;
} INDEX_FASTAloaderExt;

static INDEX_FASTAloaderExt new_INDEX_FASTAloaderExt()
{
	INDEX_FASTAloaderExt loader_ext;

	loader_ext.recno_buf = new_IntAE(0, 0, 0);
	loader_ext.offset_buf = new_LLongAE(0, 0, 0);
	loader_ext.desc_buf = new_CharAEAE(0, 0);
	loader_ext.seqlength_buf = new_IntAE(0, 0, 0);
	return loader_ext;
}

static void FASTA_INDEX_load_desc_line(FASTAloader *loader,
				       int recno, long long int offset,
				       const Chars_holder *desc_line)
{
	INDEX_FASTAloaderExt *loader_ext;
	IntAE *recno_buf;
	LLongAE *offset_buf;
	CharAEAE *desc_buf;

	loader_ext = loader->ext;

	recno_buf = loader_ext->recno_buf;
	IntAE_insert_at(recno_buf, IntAE_get_nelt(recno_buf), recno + 1);

	offset_buf = loader_ext->offset_buf;
	LLongAE_insert_at(offset_buf, LLongAE_get_nelt(offset_buf), offset);

	desc_buf = loader_ext->desc_buf;
	// This works only because desc_line->seq is nul-terminated!
	CharAEAE_append_string(desc_buf, desc_line->ptr);
	return;
}

static void FASTA_INDEX_load_empty_seq(FASTAloader *loader)
{
	INDEX_FASTAloaderExt *loader_ext;
	IntAE *seqlength_buf;

	loader_ext = loader->ext;
	seqlength_buf = loader_ext->seqlength_buf;
	IntAE_insert_at(seqlength_buf, IntAE_get_nelt(seqlength_buf), 0);
	return;
}

static void FASTA_INDEX_load_seq_data(FASTAloader *loader,
		const Chars_holder *seq_data)
{
	INDEX_FASTAloaderExt *loader_ext;
	IntAE *seqlength_buf;

	loader_ext = loader->ext;
	seqlength_buf = loader_ext->seqlength_buf;
	seqlength_buf->elts[IntAE_get_nelt(seqlength_buf) - 1] +=
		seq_data->length;
	return;
}

static FASTAloader new_FASTAloader_with_INDEX_ext(SEXP lkup, int load_descs,
		INDEX_FASTAloaderExt *loader_ext)
{
	FASTAloader loader;

	if (lkup == R_NilValue) {
		loader.lkup = NULL;
		loader.lkup_length = 0;
	} else {
		loader.lkup = INTEGER(lkup);
		loader.lkup_length = LENGTH(lkup);
	}
	loader.load_desc_line = load_descs ? &FASTA_INDEX_load_desc_line : NULL;
	loader.load_empty_seq = &FASTA_INDEX_load_empty_seq;
	loader.load_seq_data = &FASTA_INDEX_load_seq_data;
	loader.nrec = 0;
	loader.ext = loader_ext;
	return loader;
}

/*
 * The FASTA loader.
 */

typedef struct fasta_loader_ext {
	XVectorList_holder ans_holder;
	Chars_holder ans_elt_holder;
} FASTAloaderExt;

static FASTAloaderExt new_FASTAloaderExt(SEXP ans)
{
	FASTAloaderExt loader_ext;

	loader_ext.ans_holder = hold_XVectorList(ans);
	return loader_ext;
}

static void FASTA_load_empty_seq(FASTAloader *loader)
{
	FASTAloaderExt *loader_ext;
	Chars_holder *ans_elt_holder;

	loader_ext = loader->ext;
	ans_elt_holder = &(loader_ext->ans_elt_holder);
	*ans_elt_holder = get_elt_from_XRawList_holder(
					&(loader_ext->ans_holder),
					loader->nrec);
	ans_elt_holder->length = 0;
	return;
}

static void FASTA_load_seq_data(FASTAloader *loader,
		const Chars_holder *seq_data)
{
	FASTAloaderExt *loader_ext;
	Chars_holder *ans_elt_holder;

	loader_ext = loader->ext;
	ans_elt_holder = &(loader_ext->ans_elt_holder);
	/* ans_elt_holder->ptr is a const char * so we need to cast it to
	   char * in order to write to it */
	memcpy((char *) ans_elt_holder->ptr + ans_elt_holder->length,
	       seq_data->ptr, seq_data->length * sizeof(char));
	ans_elt_holder->length += seq_data->length;
	return;
}

static FASTAloader new_FASTAloader(SEXP lkup, FASTAloaderExt *loader_ext)
{
	FASTAloader loader;

	if (lkup == R_NilValue) {
		loader.lkup = NULL;
		loader.lkup_length = 0;
	} else {
		loader.lkup = INTEGER(lkup);
		loader.lkup_length = LENGTH(lkup);
	}
	loader.load_desc_line = NULL;
	loader.load_empty_seq = &FASTA_load_empty_seq;
	loader.load_seq_data = &FASTA_load_seq_data;
	loader.nrec = 0;
	loader.ext = loader_ext;
	return loader;
}

static int translate(Chars_holder *seq_data, const int *lkup, int lkup_length)
{
	char *dest;
	int nbinvalid, i, j, key, val;

	/* seq_data->ptr is a const char * so we need to cast it to
	   char * before we can write to it */
	dest = (char *) seq_data->ptr;
	nbinvalid = j = 0;
	for (i = 0; i < seq_data->length; i++) {
		key = (unsigned char) seq_data->ptr[i];
		if (key >= lkup_length || (val = lkup[key]) == NA_INTEGER) {
			nbinvalid++;
			continue;
		}
		dest[j++] = val;
	}
	seq_data->length = j;
	return nbinvalid;
}

/*
 * Ignore empty lines and lines starting with 'FASTA_comment_markup' like in
 * the original Pearson FASTA format.
 */
static const char *parse_FASTA_file(SEXP filexp,
		int nrec, int skip, int seek_first_rec,
		FASTAloader *loader,
		int *recno, long long int *offset, long long int *ninvalid)
{
	int lineno, EOL_in_buf, EOL_in_prev_buf, ret_code, nbyte_in,
	    FASTA_desc_markup_length, load_rec, is_new_rec;
	char buf[IOBUF_SIZE];
	Chars_holder data;
	long long int prev_offset;

	FASTA_desc_markup_length = strlen(FASTA_desc_markup);
	load_rec = -1;
	for (lineno = EOL_in_prev_buf = 1;
	     (ret_code = filexp_gets(filexp, buf, IOBUF_SIZE, &EOL_in_buf));
	     lineno += EOL_in_prev_buf = EOL_in_buf)
	{
		if (ret_code == -1) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "read error while reading characters "
				 "from line %d", lineno);
			return errmsg_buf;
		}
		if (EOL_in_buf) {
			nbyte_in = strlen(buf);
			data.length = delete_trailing_LF_or_CRLF(buf, nbyte_in);
		} else {
			data.length = nbyte_in = IOBUF_SIZE - 1;
		}
		prev_offset = *offset;
		*offset += nbyte_in;
		if (seek_first_rec) {
			if (EOL_in_prev_buf
			 && has_prefix(buf, FASTA_desc_markup)) {
				seek_first_rec = 0;
			} else {
				continue;
			}
		}
		data.ptr = buf;
		if (!EOL_in_prev_buf)
			goto parse_seq_data;
		if (data.length == 0)
			continue; // we ignore empty lines
		if (has_prefix(data.ptr, FASTA_comment_markup)) {
			if (!EOL_in_buf) {
				snprintf(errmsg_buf, sizeof(errmsg_buf),
					 "cannot read line %d, "
					 "line is too long", lineno);
				return errmsg_buf;
			}
			continue; // we ignore comment lines
		}
		buf[data.length] = '\0';
		is_new_rec = has_prefix(data.ptr, FASTA_desc_markup);
		if (is_new_rec) {
			if (!EOL_in_buf) {
				snprintf(errmsg_buf, sizeof(errmsg_buf),
					 "cannot read line %d, "
					 "line is too long", lineno);
				return errmsg_buf;
			}
			load_rec = *recno >= skip;
			if (load_rec && nrec >= 0 && *recno >= skip + nrec)
				return NULL;
			load_rec = load_rec && loader != NULL;
			if (load_rec && loader->load_desc_line != NULL) {
				data.ptr += FASTA_desc_markup_length;
				data.length -= FASTA_desc_markup_length;
				loader->load_desc_line(loader,
						       *recno, prev_offset,
						       &data);
			}
			if (load_rec && loader->load_empty_seq != NULL)
				loader->load_empty_seq(loader);
			if (load_rec)
				loader->nrec++;
			(*recno)++;
			continue;
		}
		if (load_rec == -1) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "\"%s\" expected at beginning of line %d",
				 FASTA_desc_markup, lineno);
			return errmsg_buf;
		}
		parse_seq_data:
		if (load_rec && loader->load_seq_data != NULL) {
			if (loader->lkup != NULL)
				*ninvalid += translate(&data,
						       loader->lkup,
						       loader->lkup_length);
			loader->load_seq_data(loader, &data);
		}
	}
	if (seek_first_rec) {
		snprintf(errmsg_buf, sizeof(errmsg_buf),
			 "no FASTA record found");
		return errmsg_buf;
	}
	return NULL;
}

static SEXP make_fasta_index_data_frame(const IntAE *recno_buf,
					const IntAE *fileno_buf,
					const LLongAE *offset_buf,
					const CharAEAE *desc_buf,
					const IntAE *seqlength_buf)
{
	SEXP df, colnames, tmp;
	int i;

	PROTECT(df = NEW_LIST(5));

	PROTECT(colnames = NEW_CHARACTER(5));
	PROTECT(tmp = mkChar("recno"));
	SET_STRING_ELT(colnames, 0, tmp);
	UNPROTECT(1);
	PROTECT(tmp = mkChar("fileno"));
	SET_STRING_ELT(colnames, 1, tmp);
	UNPROTECT(1);
	PROTECT(tmp = mkChar("offset"));
	SET_STRING_ELT(colnames, 2, tmp);
	UNPROTECT(1);
	PROTECT(tmp = mkChar("desc"));
	SET_STRING_ELT(colnames, 3, tmp);
	UNPROTECT(1);
	PROTECT(tmp = mkChar("seqlength"));
	SET_STRING_ELT(colnames, 4, tmp);
	UNPROTECT(1);
	SET_NAMES(df, colnames);
	UNPROTECT(1);

	PROTECT(tmp = new_INTEGER_from_IntAE(recno_buf));
	SET_ELEMENT(df, 0, tmp);
	UNPROTECT(1);

	PROTECT(tmp = new_INTEGER_from_IntAE(fileno_buf));
	SET_ELEMENT(df, 1, tmp);
	UNPROTECT(1);

	PROTECT(tmp = NEW_NUMERIC(LLongAE_get_nelt(offset_buf)));
	for (i = 0; i < LENGTH(tmp); i++)
		REAL(tmp)[i] = (double) offset_buf->elts[i];
	SET_ELEMENT(df, 2, tmp);
	UNPROTECT(1);

	PROTECT(tmp = new_CHARACTER_from_CharAEAE(desc_buf));
	SET_ELEMENT(df, 3, tmp);
	UNPROTECT(1);

	PROTECT(tmp = new_INTEGER_from_IntAE(seqlength_buf));
	SET_ELEMENT(df, 4, tmp);
	UNPROTECT(1);

	/* list_as_data_frame() performs IN-PLACE coercion */
	list_as_data_frame(df, IntAE_get_nelt(recno_buf));
	UNPROTECT(1);
	return df;
}

/* --- .Call ENTRY POINT --- */
SEXP fasta_index(SEXP filexp_list,
		 SEXP nrec, SEXP skip, SEXP seek_first_rec, SEXP lkup)
{
	int nrec0, skip0, seek_rec0, i, recno, old_nrec, new_nrec, k;
	INDEX_FASTAloaderExt loader_ext;
	FASTAloader loader;
	IntAE *seqlength_buf, *fileno_buf;
	SEXP filexp;
	long long int offset, ninvalid;
	const char *errmsg;

	nrec0 = INTEGER(nrec)[0];
	skip0 = INTEGER(skip)[0];
	seek_rec0 = LOGICAL(seek_first_rec)[0];
	loader_ext = new_INDEX_FASTAloaderExt();
	loader = new_FASTAloader_with_INDEX_ext(lkup, 1, &loader_ext);
	seqlength_buf = loader_ext.seqlength_buf;
	fileno_buf = new_IntAE(0, 0, 0);
	for (i = recno = 0; i < LENGTH(filexp_list); i++) {
		filexp = VECTOR_ELT(filexp_list, i);
		offset = ninvalid = 0LL;
		errmsg = parse_FASTA_file(filexp, nrec0, skip0, seek_rec0,
					  &loader, &recno, &offset, &ninvalid);
		if (errmsg != NULL)
			error("reading FASTA file %s: %s",
			      CHAR(STRING_ELT(GET_NAMES(filexp_list), i)),
			      errmsg_buf);
		if (ninvalid != 0LL)
			warning("reading FASTA file %s: ignored %lld "
				"invalid one-letter sequence codes",
				CHAR(STRING_ELT(GET_NAMES(filexp_list), i)),
				ninvalid);
		old_nrec = IntAE_get_nelt(fileno_buf);
		new_nrec = IntAE_get_nelt(seqlength_buf);
		for (k = old_nrec; k < new_nrec; k++)
			IntAE_insert_at(fileno_buf, k, i + 1);
	}
	return make_fasta_index_data_frame(loader_ext.recno_buf,
					   fileno_buf,
					   loader_ext.offset_buf,
					   loader_ext.desc_buf,
					   seqlength_buf);
}

/* --- .Call ENTRY POINT ---
 * "FASTA blocks" are groups of consecutive FASTA records.
 * Args:
 *   seqlengths:  Integer vector with 1 element per record to read. The
 *                elements must be placed in the order that the records are
 *                going to be read.
 *   filexp_list: A list of N "File External Pointers" (see src/io_utils.c in
 *                the XVector package) with 1 element per input file. Files are
 *                going to be accessed from first to last in the list.
 *   nrec_list:   A list of N integer vectors (1 element per input file).
 *                Each integer vector has 1 value per FASTA block, which is the
 *                number of records in the block. IMPORTANT: Even if not
 *                required, the blocks in each integer vector should preferably
 *                be placed in the same order as in the file. This ensures that
 *                the calls to filexp_seek() are always moving forward
 *                in the file and are therefore efficient (moving backward on a
 *                compressed file can be *extremely* slow).
 *   offset_list: A list of N numeric vectors (1 element per input file) with
 *                the same shape as 'nrec_list', i.e. each numeric vector has 1
 *                value per FASTA block. This value is the offset of the block
 *                (i.e. the offset of its first record) relative to the start
 *                of the file. Measured in bytes.
 *   elementType: The elementType of the XStringSet to return (its class is
 *                inferred from this).
 *   lkup:        Lookup table for encoding the incoming sequence bytes.
 */
SEXP read_XStringSet_from_fasta_blocks(SEXP seqlengths,
		SEXP filexp_list, SEXP nrec_list, SEXP offset_list,
		SEXP elementType, SEXP lkup)
{
	SEXP ans, filexp, nrec, offset;
	FASTAloaderExt loader_ext;
	FASTAloader loader;
	int i, j, nrec_j, recno;
	long long int offset_j, ninvalid;

	PROTECT(ans = _alloc_XStringSet(CHAR(STRING_ELT(elementType, 0)),
					seqlengths));
	loader_ext = new_FASTAloaderExt(ans);
	loader = new_FASTAloader(lkup, &loader_ext);
	for (i = 0; i < LENGTH(filexp_list); i++) {
		filexp = VECTOR_ELT(filexp_list, i);
		nrec = VECTOR_ELT(nrec_list, i);
		offset = VECTOR_ELT(offset_list, i);
		for (j = 0; j < LENGTH(nrec); j++) {
			nrec_j = INTEGER(nrec)[j];
			offset_j = llround(REAL(offset)[j]);
			filexp_seek(filexp, offset_j, SEEK_SET);
			recno = 0;
			ninvalid = 0LL;
			parse_FASTA_file(filexp, nrec_j, 0, 0,
					 &loader, &recno, &offset_j, &ninvalid);
		}
	}
	UNPROTECT(1);
	return ans;
}


/****************************************************************************
 * Writing FASTA files.
 */

/* --- .Call ENTRY POINT --- */
SEXP write_XStringSet_to_fasta(SEXP x, SEXP filexp_list, SEXP width, SEXP lkup)
{
	XStringSet_holder X;
	int x_length, width0, lkup_length, i, j1, j2, dest_nbytes;
	const int *lkup0;
	SEXP filexp, x_names, desc;
	Chars_holder X_elt;
	char buf[IOBUF_SIZE];

	X = _hold_XStringSet(x);
	x_length = _get_length_from_XStringSet_holder(&X);
	filexp = VECTOR_ELT(filexp_list, 0);
	width0 = INTEGER(width)[0];
	if (width0 >= IOBUF_SIZE)
		error("'width' must be <= %d", IOBUF_SIZE - 1);
	buf[width0] = 0;
	if (lkup == R_NilValue) {
		lkup0 = NULL;
		lkup_length = 0;
	} else {
		lkup0 = INTEGER(lkup);
		lkup_length = LENGTH(lkup);
	}
	x_names = get_XVectorList_names(x);
	for (i = 0; i < x_length; i++) {
		filexp_puts(filexp, FASTA_desc_markup);
		if (x_names != R_NilValue) {
			desc = STRING_ELT(x_names, i);
			if (desc == NA_STRING)
				error("'names(x)' contains NAs");
			filexp_puts(filexp, CHAR(desc));
		}
		filexp_puts(filexp, "\n");
		X_elt = _get_elt_from_XStringSet_holder(&X, i);
		for (j1 = 0; j1 < X_elt.length; j1 += width0) {
			j2 = j1 + width0;
			if (j2 > X_elt.length)
				j2 = X_elt.length;
			dest_nbytes = j2 - j1;
			j2--;
			Ocopy_bytes_from_i1i2_with_lkup(j1, j2,
				buf, dest_nbytes,
				X_elt.ptr, X_elt.length,
				lkup0, lkup_length);
			buf[dest_nbytes] = 0;
			filexp_puts(filexp, buf);
			filexp_puts(filexp, "\n");
		}
	}
	return R_NilValue;
}



/****************************************************************************
 *  B. FASTQ FORMAT                                                         *
 ****************************************************************************/

static const char *FASTQ_line1_markup = "@", *FASTQ_line3_markup = "+";


/****************************************************************************
 * Reading FASTQ files.
 */

typedef struct fastq_loader {
	void (*load_seqid)(struct fastq_loader *loader,
			   const Chars_holder *seqid);
	void (*load_seq)(struct fastq_loader *loader,
			 const Chars_holder *seq);
	void (*load_qualid)(struct fastq_loader *loader,
			    const Chars_holder *qualid);
	void (*load_qual)(struct fastq_loader *loader,
			  const Chars_holder *qual);
	int nrec;
	void *ext;  /* loader extension (optional) */
} FASTQloader;

/*
 * The FASTQ SEQLEN loader.
 * Used in parse_FASTQ_file() to load the read lengths only.
 */

typedef struct seqlen_fastq_loader_ext {
	IntAE *seqlength_buf;
} SEQLEN_FASTQloaderExt;

static SEQLEN_FASTQloaderExt new_SEQLEN_FASTQloaderExt()
{
	SEQLEN_FASTQloaderExt loader_ext;

	loader_ext.seqlength_buf = new_IntAE(0, 0, 0);
	return loader_ext;
}

static void FASTQ_SEQLEN_load_seq(FASTQloader *loader, const Chars_holder *seq)
{
	SEQLEN_FASTQloaderExt *loader_ext;
	IntAE *seqlength_buf;

	loader_ext = loader->ext;
	seqlength_buf = loader_ext->seqlength_buf;
	IntAE_insert_at(seqlength_buf, IntAE_get_nelt(seqlength_buf),
			seq->length);
	return;
}

static FASTQloader new_FASTQloader_with_SEQLEN_ext(
		SEQLEN_FASTQloaderExt *loader_ext)
{
	FASTQloader loader;

	loader.load_seqid = NULL;
	loader.load_seq = FASTQ_SEQLEN_load_seq;
	loader.load_qualid = NULL;
	loader.load_qual = NULL;
	loader.nrec = 0;
	loader.ext = loader_ext;
	return loader;
}

/*
 * The FASTQ loader.
 */

typedef struct fastq_loader_ext {
	CharAEAE *seqid_buf;
	XVectorList_holder seq_holder;
	const int *lkup;
	int lkup_length;
	XVectorList_holder qual_holder;
} FASTQloaderExt;

static FASTQloaderExt new_FASTQloaderExt(SEXP sequences, SEXP lkup,
		SEXP qualities)
{
	FASTQloaderExt loader_ext;

	loader_ext.seqid_buf =
		new_CharAEAE(_get_XStringSet_length(sequences), 0);
	loader_ext.seq_holder = hold_XVectorList(sequences);
	if (lkup == R_NilValue) {
		loader_ext.lkup = NULL;
		loader_ext.lkup_length = 0;
	} else {
		loader_ext.lkup = INTEGER(lkup);
		loader_ext.lkup_length = LENGTH(lkup);
	}
	if (qualities != R_NilValue)
		loader_ext.qual_holder = hold_XVectorList(qualities);
	return loader_ext;
}

static void FASTQ_load_seqid(FASTQloader *loader, const Chars_holder *seqid)
{
	FASTQloaderExt *loader_ext;
	CharAEAE *seqid_buf;

	loader_ext = loader->ext;
	seqid_buf = loader_ext->seqid_buf;
	// This works only because seqid->ptr is nul-terminated!
	CharAEAE_append_string(seqid_buf, seqid->ptr);
	return;
}

static void FASTQ_load_seq(FASTQloader *loader, const Chars_holder *seq)
{
	FASTQloaderExt *loader_ext;
	Chars_holder seq_elt_holder;

	loader_ext = loader->ext;
	seq_elt_holder = get_elt_from_XRawList_holder(
					&(loader_ext->seq_holder),
					loader->nrec);
	copy_Chars_holder(&seq_elt_holder, seq,
			  loader_ext->lkup, loader_ext->lkup_length);
	return;
}

static void FASTQ_load_qual(FASTQloader *loader, const Chars_holder *qual)
{
	FASTQloaderExt *loader_ext;
	Chars_holder qual_elt_holder;

	loader_ext = loader->ext;
	qual_elt_holder = get_elt_from_XRawList_holder(
					&(loader_ext->qual_holder),
					loader->nrec);
	copy_Chars_holder(&qual_elt_holder, qual, NULL, 0);
	return;
}

static FASTQloader new_FASTQloader(int load_seqids, int load_quals,
				   FASTQloaderExt *loader_ext)
{
	FASTQloader loader;

	loader.load_seqid = load_seqids ? &FASTQ_load_seqid : NULL;
	loader.load_seq = FASTQ_load_seq;
	if (load_quals) {
		/* Quality ids are always ignored for now. */
		//loader.load_qualid = &FASTQ_load_qualid;
		loader.load_qualid = NULL;
		loader.load_qual = &FASTQ_load_qual;
	} else {
		loader.load_qualid = NULL;
		loader.load_qual = NULL;
	}
	loader.nrec = 0;
	loader.ext = loader_ext;
	return loader;
}

/*
 * Ignore empty lines.
 */
static const char *parse_FASTQ_file(SEXP filexp,
		int nrec, int skip, int seek_first_rec,
		FASTQloader *loader, int *recno)
{
	int lineno, EOL_in_buf, EOL_in_prev_buf, ret_code,
	    FASTQ_line1_markup_length, FASTQ_line3_markup_length,
	    lineinrecno, load_rec, seq_len;
	char buf[IOBUF_SIZE];
	Chars_holder data;

	FASTQ_line1_markup_length = strlen(FASTQ_line1_markup);
	FASTQ_line3_markup_length = strlen(FASTQ_line3_markup);
	lineinrecno = 0;
	for (lineno = EOL_in_prev_buf = 1;
	     (ret_code = filexp_gets(filexp, buf, IOBUF_SIZE, &EOL_in_buf));
	     lineno += EOL_in_prev_buf = EOL_in_buf)
	{
		if (ret_code == -1) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "read error while reading characters "
				 "from line %d", lineno);
			return errmsg_buf;
		}
		if (seek_first_rec) {
			if (EOL_in_prev_buf
			 && has_prefix(buf, FASTQ_line1_markup)) {
				seek_first_rec = 0;
			} else {
				continue;
			}
		}
		if (!EOL_in_buf) {
			snprintf(errmsg_buf, sizeof(errmsg_buf),
				 "cannot read line %d, line is too long",
				 lineno);
			return errmsg_buf;
		}
		data.length = delete_trailing_LF_or_CRLF(buf, -1);
		if (data.length == 0)
			continue; // we ignore empty lines
		buf[data.length] = '\0';
		data.ptr = buf;
		lineinrecno++;
		if (lineinrecno > 4)
			lineinrecno = 1;
		switch (lineinrecno) {
		    case 1:
			if (!has_prefix(buf, FASTQ_line1_markup)) {
				snprintf(errmsg_buf, sizeof(errmsg_buf),
				     "\"%s\" expected at beginning of line %d",
				     FASTQ_line1_markup, lineno);
				return errmsg_buf;
			}
			load_rec = *recno >= skip;
			if (load_rec && nrec >= 0 && *recno >= skip + nrec)
				return NULL;
			load_rec = load_rec && loader != NULL;
			if (load_rec && nrec >= 0)
				load_rec = *recno < skip + nrec;
			if (load_rec && loader->load_seqid != NULL) {
				data.ptr += FASTQ_line1_markup_length;
				data.length -= FASTQ_line1_markup_length;
				loader->load_seqid(loader, &data);
			}
		    break;
		    case 2:
			if (load_rec) {
				seq_len = data.length;
				if  (loader->load_seq != NULL)
					loader->load_seq(loader, &data);
			}
		    break;
		    case 3:
			if (!has_prefix(buf, FASTQ_line3_markup)) {
				snprintf(errmsg_buf, sizeof(errmsg_buf),
					 "\"%s\" expected at beginning of "
					 "line %d", FASTQ_line3_markup, lineno);
				return errmsg_buf;
			}
			if (load_rec && loader->load_qualid != NULL) {
				data.ptr += FASTQ_line3_markup_length;
				data.length -= FASTQ_line3_markup_length;
				loader->load_qualid(loader, &data);
			}
		    break;
		    case 4:
			if (load_rec) {
				if (data.length != seq_len) {
					snprintf(errmsg_buf, sizeof(errmsg_buf),
						 "length of quality string "
						 "at line %d\n  differs from "
						 "length of corresponding "
						 "sequence", lineno);
					return errmsg_buf;
				}
				if (loader->load_qual != NULL)
					loader->load_qual(loader, &data);
			}
			if (load_rec)
				loader->nrec++;
			(*recno)++;
		    break;
		}
	}
	if (seek_first_rec) {
		snprintf(errmsg_buf, sizeof(errmsg_buf),
			 "no FASTQ record found");
		return errmsg_buf;
	}
	return NULL;
}

static SEXP get_fastq_seqlengths(SEXP filexp_list,
		int nrec, int skip, int seek_first_rec)
{
	SEQLEN_FASTQloaderExt loader_ext;
	FASTQloader loader;
	int recno, i;
	SEXP filexp;
	const char *errmsg;

	loader_ext = new_SEQLEN_FASTQloaderExt();
	loader = new_FASTQloader_with_SEQLEN_ext(&loader_ext);
	recno = 0;
	for (i = 0; i < LENGTH(filexp_list); i++) {
		filexp = VECTOR_ELT(filexp_list, i);
		errmsg = parse_FASTQ_file(filexp, nrec, skip, seek_first_rec,
					  &loader, &recno);
		if (errmsg != NULL)
			error("reading FASTQ file %s: %s",
			      CHAR(STRING_ELT(GET_NAMES(filexp_list), i)),
			      errmsg_buf);
	}
	return new_INTEGER_from_IntAE(loader_ext.seqlength_buf);
}

/* --- .Call ENTRY POINT ---
 * Args:
 *   filexp_list:     A list of external pointers.
 *   nrec:            An integer vector of length 1.
 *   skip:            An integer vector of length 1.
 *   seek_first_rec:  A logical vector of length 1.
 */
SEXP fastq_seqlengths(SEXP filexp_list,
		SEXP nrec, SEXP skip, SEXP seek_first_rec)
{
	int nrec0, skip0, seek_rec0;

	nrec0 = INTEGER(nrec)[0];
	skip0 = INTEGER(skip)[0];
	seek_rec0 = LOGICAL(seek_first_rec)[0];
	return get_fastq_seqlengths(filexp_list, nrec0, skip0, seek_rec0);
}

/* --- .Call ENTRY POINT ---
 * Return an XStringSet object if 'with_qualities' is FALSE, or a list of 2
 * parallel XStringSet objects of the same shape if 'with_qualities' is TRUE.
 */
SEXP read_XStringSet_from_fastq(SEXP filexp_list, SEXP nrec, SEXP skip,
		SEXP seek_first_rec,
		SEXP use_names, SEXP elementType, SEXP lkup,
		SEXP with_qualities)
{
	int nrec0, skip0, seek_rec0, load_seqids, load_quals, recno, i;
	SEXP filexp, seqlengths, sequences, seqids, qualities, ans;
	FASTQloaderExt loader_ext;
	FASTQloader loader;

	nrec0 = INTEGER(nrec)[0];
	skip0 = INTEGER(skip)[0];
	seek_rec0 = LOGICAL(seek_first_rec)[0];
	load_seqids = LOGICAL(use_names)[0];
	load_quals = LOGICAL(with_qualities)[0];
	PROTECT(seqlengths = get_fastq_seqlengths(filexp_list,
						  nrec0, skip0, seek_rec0));
	PROTECT(sequences = _alloc_XStringSet(CHAR(STRING_ELT(elementType, 0)),
					      seqlengths));
	if (load_quals) {
		PROTECT(qualities = _alloc_XStringSet("BString", seqlengths));
	} else {
		qualities = R_NilValue;
	}
	loader_ext = new_FASTQloaderExt(sequences, lkup, qualities);
	loader = new_FASTQloader(load_seqids, load_quals, &loader_ext);
	recno = 0;
	for (i = 0; i < LENGTH(filexp_list); i++) {
		filexp = VECTOR_ELT(filexp_list, i);
		filexp_rewind(filexp);
		parse_FASTQ_file(filexp, nrec0, skip0, seek_rec0,
				 &loader, &recno);
	}
	if (load_seqids) {
		PROTECT(seqids =
			new_CHARACTER_from_CharAEAE(loader_ext.seqid_buf));
		_set_XStringSet_names(sequences, seqids);
		UNPROTECT(1);
	}
	if (!load_quals) {
		UNPROTECT(2);
		return sequences;
	}
	PROTECT(ans = NEW_LIST(2));
	SET_ELEMENT(ans, 0, sequences);
	SET_ELEMENT(ans, 1, qualities);
	UNPROTECT(4);
	return ans;
}


/****************************************************************************
 * Writing FASTQ files.
 */

static const char *get_FASTQ_rec_id(SEXP x_names, SEXP q_names, int i)
{
	SEXP seqid, qualid;

	seqid = NA_STRING;
	if (x_names != R_NilValue) {
		seqid = STRING_ELT(x_names, i);
		if (seqid == NA_STRING)
			error("'names(x)' contains NAs");
	}
	if (q_names != R_NilValue) {
		qualid = STRING_ELT(q_names, i);
		if (qualid == NA_STRING)
			error("'names(qualities)' contains NAs");
		if (seqid == NA_STRING)
			seqid = qualid;
		/* Comparing the *content* of 2 CHARSXP's with
		   'qualid != seqid' only works because of the global CHARSXP
		   cache. Otherwise we would need to do:
		     LENGTH(qualid) != LENGTH(seqid) ||
		       memcmp(CHAR(qualid), CHAR(seqid), LENGTH(seqid))
		 */
		else if (qualid != seqid)
			error("when 'x' and 'qualities' both have "
			      "names, they must be identical");
	}
	if (seqid == NA_STRING)
		error("either 'x' or 'qualities' must have names");
	return CHAR(seqid);
}

static void write_FASTQ_id(SEXP filexp, const char *markup, const char *id)
{
	filexp_puts(filexp, markup);
	filexp_puts(filexp, id);
	filexp_puts(filexp, "\n");
}

static void write_FASTQ_seq(SEXP filexp, const char *buf)
{
	filexp_puts(filexp, buf);
	filexp_puts(filexp, "\n");
}

static void write_FASTQ_qual(SEXP filexp, int seqlen,
		const XStringSet_holder *Q, int i)
{
	Chars_holder Q_elt;
	int j;

	Q_elt = _get_elt_from_XStringSet_holder(Q, i);
	if (Q_elt.length != seqlen)
		error("'x' and 'quality' must have the same width");
	for (j = 0; j < seqlen; j++)
		filexp_putc(filexp, (int) *(Q_elt.ptr++));
	filexp_puts(filexp, "\n");
}

static void write_FASTQ_fakequal(SEXP filexp, int seqlen)
{
	int j;

	for (j = 0; j < seqlen; j++)
		filexp_putc(filexp, (int) ';');
	filexp_puts(filexp, "\n");
}

/* --- .Call ENTRY POINT --- */
SEXP write_XStringSet_to_fastq(SEXP x, SEXP filexp_list,
		SEXP qualities, SEXP lkup)
{
	XStringSet_holder X, Q;
	int x_length, lkup_length, i;
	const int *lkup0;
	SEXP filexp, x_names, q_names;
	const char *id;
	Chars_holder X_elt;
	char buf[IOBUF_SIZE];

	X = _hold_XStringSet(x);
	x_length = _get_length_from_XStringSet_holder(&X);
	if (qualities != R_NilValue) {
		Q = _hold_XStringSet(qualities);
		if (_get_length_from_XStringSet_holder(&Q) != x_length)
			error("'x' and 'qualities' must have the same length");
		q_names = get_XVectorList_names(qualities);
	} else {
		q_names = R_NilValue;
	}
	filexp = VECTOR_ELT(filexp_list, 0);
	if (lkup == R_NilValue) {
		lkup0 = NULL;
		lkup_length = 0;
	} else {
		lkup0 = INTEGER(lkup);
		lkup_length = LENGTH(lkup);
	}
	x_names = get_XVectorList_names(x);
	for (i = 0; i < x_length; i++) {
		id = get_FASTQ_rec_id(x_names, q_names, i);
		X_elt = _get_elt_from_XStringSet_holder(&X, i);
		Ocopy_bytes_from_i1i2_with_lkup(0, X_elt.length - 1,
			buf, X_elt.length,
			X_elt.ptr, X_elt.length,
			lkup0, lkup_length);
		buf[X_elt.length] = 0;
		write_FASTQ_id(filexp, FASTQ_line1_markup, id);
		write_FASTQ_seq(filexp, buf);
		write_FASTQ_id(filexp, FASTQ_line3_markup, id);
		if (qualities != R_NilValue) {
			write_FASTQ_qual(filexp, X_elt.length, &Q, i);
		} else {
			write_FASTQ_fakequal(filexp, X_elt.length);
		}
	}
	return R_NilValue;
}

