/* This file defines those functions which must be repeated for each data type.
   They are further processed with Perl program make_functions.pl: in=functions.in.c, out=functions.c.

   No wrapper function is made if function name begins with underscore,
   or function returns ctype.
   */
#define NCT_NO_VERSION_CHECK
#include "nctietue3.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int my_isnan_float(float f) {
    const unsigned exponent = ((1u<<31)-1) - ((1u<<(31-8))-1);
    uint32_t bits;
    memcpy(&bits, &f, 4);
    return (bits & exponent) == exponent;
}

static int my_isnan_double(double f) {
    const long unsigned exponent = ((1lu<<63)-1) - ((1lu<<(63-11))-1);
    uint64_t bits;
    memcpy(&bits, &f, 8);
    return (bits & exponent) == exponent;
}

void nct_allocate_varmem(nct_var*); // global but hidden: not in nctietue3.h
void nct_verbose_line_ending(); // global but hidden: not in nctietue3.h

@startperl // entry for the perl program

#define ctype @ctype
#define form @form
#define __nctype__ @nctype

@begin_stringalso
void nct_fprint_datum_@nctype(nct_fprint_t print, void *file, const void *vdatum) {
    @ctype datum = *(@ctype*)vdatum;
#if __nctype__ == NC_FLOAT || __nctype__ == NC_DOUBLE
    if (datum > 0) {
	if (datum < 1e-5 || datum >= 1e6) {
	    print(file, "%e", datum);
	    return; }
    }
    else if (datum < 0 && (datum > -1e-5 || datum <= -1e6)) {
	print(file, "%e", datum);
	return; }
#endif
    print(file, "%@form", datum);
}

void nct_fprint_datum_at_@nctype(nct_fprint_t print, void *file, const void* vdatum, long pos) {
    nct_fprint_datum_@nctype(print, file, ((@ctype*)vdatum)+pos);
}

void nct_print_datum_@nctype(const void* vdatum) {
    nct_fprint_datum_@nctype((nct_fprint_t)fprintf, stdout, vdatum);
}

void nct_print_datum_at_@nctype(const void* vdatum, long pos) {
    nct_print_datum_@nctype(((@ctype*)vdatum)+pos);
}

static void _printhelper_@nctype(@ctype* data, long i, long len) {
    if (len < 1)
	return;
    for(; i<len-1; i++) {
	nct_print_datum_@nctype(data+i);
	printf(", ");
    }
    nct_print_datum_@nctype(data+len-1);
}

void nct_print_data_@nctype(nct_var* var) {
    size_t len = var->len;
    if (len <= 17) {
	nct_perhaps_load_partially(var, 0, len);
	_printhelper_@nctype(var->data, 0, len);
	return; }

    int old = nct_readflags;
    nct_readflags &= nct_rkeep;
    nct_perhaps_load_partially(var, 0, 8);
    _printhelper_@nctype(var->data, 0, 8);
    printf(" ..., ");
    nct_readflags = old;
    nct_perhaps_load_partially(var, len-8, len);
    _printhelper_@nctype(var->data, len-8-var->startpos, len-var->startpos);
}
@end_stringalso

void nct_add_@nctype(nct_var *var, size_t i, double value) {
    ((@ctype*)var->data)[i] += value;
}

void nct_add_all_@nctype(nct_var *var, double value) {
    for (long i=var->len-1; i>=0; i--)
	((@ctype*)var->data)[i] += value;
}

ctype* nct_range_@nctype(ctype i0, ctype i1, ctype gap) {
    size_t len = (i1-i0) / gap + 1;
    ctype* dest = malloc(len*sizeof(ctype));
    for(size_t i=0; i<len; i++)
	dest[i] = i0 + i*gap;
    return dest;
}

nct_var* nct_put_interval_@nctype(nct_var* var, double d0, double dgap) {
    ctype i0 = d0, gap = dgap;
    nct_allocate_varmem(var);
    ctype* dest = var->data;
    size_t len = var->len;
    for(size_t i=0; i<len; i++)
	dest[i] = i0 + i*gap; // previous + gap might be inaccurate on floating point numbers
    return var;
}

double nct_get_floating_from_@nctype(const void* vdata, long ind) {
    return (double)((ctype*)vdata)[ind];
}

long long nct_get_integer_from_@nctype(const void* vdata, long ind) {
     return (long long)((ctype*)vdata)[ind];
}

double nct_get_floating_@nctype(const nct_var* var, size_t ind) {
    return (double)((ctype*)var->data)[ind];
}

long long nct_get_integer_@nctype(const nct_var* var, size_t ind) {
    return (long long)((ctype*)var->data)[ind];
}

double nct_get_floating_last_@nctype(const nct_var* var, size_t ind) {
    return (double)((ctype*)var->data)[var->len-ind];
}

long long nct_get_integer_last_@nctype(const nct_var* var, size_t ind) {
    return (long long)((ctype*)var->data)[var->len-ind];
}

double nct_getatt_floating_@nctype(const nct_att* att, size_t ind) {
    return ((ctype*)att->value)[ind];
}

long long nct_getatt_integer_@nctype(const nct_att* att, size_t ind) {
    return ((ctype*)att->value)[ind];
}

/* Different implementation might be more optimal for interpolating fast changing dimensions.
   This is probably better for slow changing dimensions. */

void* nct_get_interpolated_@nctype(const nct_var* var, int idim, const nct_var* todim) {
    size_t newlen = var->len / nct_get_vardim(var, idim)->len * todim->len;
    const nct_var* frdim = nct_get_vardim(var, idim);
    size_t frdimlen = frdim->len;
    size_t todimlen = todim->len;
    @ctype* new = malloc(newlen * sizeof(@ctype));
    if (!new) {
	nct_puterror("malloc %zu*%zu failed: %s\n", newlen, sizeof(@ctype), strerror(errno));
	nct_other_error;
    }

    /* For example, when interpolating e2 among dims e[0-4], e0 changing slowest:
       naffected   = |e4|*|e3|
       oldcyclelen = |e4|*|e3|*|e2(old)|
       nrepeat	   = |e0|*|e1| */
    long naffected = nct_get_len_from(var, idim+1);
    long oldcyclelen = naffected * frdimlen;
    long nrepeat = var->len / oldcyclelen,
	   inew = 0;

    for (long rep=0; rep<nrepeat; rep++) {
	if (nct_verbose) {
	    printf("interpolating (%s) %li / %li", var->name, rep+1, nrepeat);
	    nct_verbose_line_ending();
	}

	for (int inewdim=0; inewdim<todimlen; inewdim++) {
	    double targetcoord = nct_get_floating(todim, inewdim);
	    int ihigher = nct_bsearch(frdim, targetcoord, 1);

	    if (ihigher >= frdimlen) {
		size_t offset = rep*oldcyclelen + (frdimlen-1)*naffected;
		for (int i=0; i<naffected; i++)
		    new[inew++] = ((@ctype*)var->data)[offset+i];
		continue;
	    }
	    else if (ihigher <= 0) {
		size_t offset = rep*oldcyclelen;
		for (int i=0; i<naffected; i++)
		    new[inew++] = ((@ctype*)var->data)[offset+i];
		continue;
	    }

	    double b = nct_get_floating(frdim, ihigher);
	    double a = nct_get_floating(frdim, ihigher-1);
	    double rel = (targetcoord - a) / (b - a);

	    size_t offset = rep*oldcyclelen + (ihigher-1)*naffected;
	    for (int i=0; i<naffected; i++) {
		@ctype b = ((@ctype*)var->data)[offset+i+naffected];
		@ctype a  = ((@ctype*)var->data)[offset+i];
		new[inew++] = a + (b-a)*rel;
	    }
	}
    }
    return new;
}

nct_anyd nct_max_anyd_@nctype(const nct_var* var) {
    long len = var->endpos - var->startpos;
#if __nctype__==NC_FLOAT || __nctype__==NC_DOUBLE
    size_t numi=0;
    ctype num = -INFINITY; //TODO: -ffinite-math-only
    for(int i=0; i<len; i++)
#else
    size_t numi=0;
    ctype num = ((ctype*)var->data)[0];
    for(size_t i=1; i<len; i++)
#endif
	if(num < ((ctype*)var->data)[i])
	    num = ((ctype*)var->data)[numi=i];
    return (nct_anyd){ {.form=num}, numi };
}

double nct_max_floating_@nctype(const nct_var* var) {
    return nct_max_anyd_@nctype(var).a.form;
}

long long nct_max_integer_@nctype(const nct_var* var) {
    return nct_max_anyd_@nctype(var).a.form;
}

nct_anyd nct_min_anyd_@nctype(const nct_var* var) {
    /* using the first value would not work with nan-values */
    long len = var->endpos - var->startpos;
#if __nctype__==NC_FLOAT || __nctype__==NC_DOUBLE
    size_t numi=0;
    ctype num = INFINITY;
    for(int i=0; i<len; i++)
#else
    size_t numi=0;
    ctype num = ((ctype*)var->data)[0];
    for(size_t i=1; i<len; i++)
#endif
	if(num > ((ctype*)var->data)[i])
	    num = ((ctype*)var->data)[numi=i];
    return (nct_anyd){ {.form=num}, numi };
}

double nct_min_floating_@nctype(const nct_var* var) {
    return nct_min_anyd_@nctype(var).a.form;
}

long long nct_min_integer_@nctype(const nct_var* var) {
    return nct_min_anyd_@nctype(var).a.form;
}

void* nct_minmax_at_@nctype(const nct_var* var, long start, long end, void* vresult) {
    ctype maxval, minval, *result=vresult;
    /* using the first value would not work with nan-values */
#if __nctype__==NC_FLOAT || __nctype__==NC_DOUBLE
    maxval = -INFINITY;
    minval = INFINITY;
    for (int i=start; i<end; i++)
	if (my_isnan_@ctype(((ctype*)var->data)[i])); // tarpeellinen Ofast-optimoinnilla
#else
    maxval = minval = ((ctype*)var->data)[start];
    for (int i=start+1; i<end; i++)
	if (0);
#endif
	else if (maxval < ((ctype*)var->data)[i])
	    maxval = ((ctype*)var->data)[i];
	else if (minval > ((ctype*)var->data)[i])
	    minval = ((ctype*)var->data)[i];
    /* If data were in ascending order, minimum value was never assigned for a floating point number
       due to the optimization above where else-if was used instead if. */
#if __nctype__==NC_FLOAT || __nctype__==NC_DOUBLE
    if (minval == INFINITY)
	for (int i=start+1; i<end; i++)
	    if (!my_isnan_@ctype(((ctype*)var->data)[i]) &&
		minval > ((ctype*)var->data)[i]) {
		minval = ((ctype*)var->data)[i];
		break;
	    }
#endif
    result[0] = minval;
    result[1] = maxval;
    return result;
}

void* nct_minmax_nan_at_@nctype(const nct_var* var, long nanval_long, long start, long end, void* vresult) {
    ctype maxval, minval, *result=vresult, nanval=nanval_long;
#if __nctype__==NC_FLOAT || __nctype__==NC_DOUBLE
#define _my_isnan(a) (!((a) == (a)) || (a) == nanval)
#else
#define _my_isnan(a) ((a) == nanval)
#endif
    int i=start;
    for (; i<end && _my_isnan(((ctype*)var->data)[i]); i++);
    if (i == end) {
	result[0] = result[1] = nanval;
	return result;
    }
    maxval = minval = ((ctype*)var->data)[i];
    for(; i<end; i++) {
	if (_my_isnan(((ctype*)var->data)[i]))
	    continue;
	if(maxval < ((ctype*)var->data)[i])
	    maxval = ((ctype*)var->data)[i];
	else if(minval > ((ctype*)var->data)[i])
	    minval = ((ctype*)var->data)[i];
    }
    result[0] = minval;
    result[1] = maxval;
    return result;
#undef _my_isnan
}

void* nct_minmax_@nctype(const nct_var* var, void* vresult) {
    return nct_minmax_at_@nctype(var, 0, var->endpos - var->startpos, vresult);
}

void* nct_minmax_nan_@nctype(const nct_var* var, long nanval_long, void* vresult) {
    return nct_minmax_nan_at_@nctype(var, nanval_long, 0, var->endpos - var->startpos, vresult);
}

nct_var* nct_mean_first_@nctype(nct_var* var) {
    if(var->endpos - var->startpos < var->len)
	nct_load(var);
    size_t zerolen = var->super->dims[var->dimids[0]]->len;
    size_t new_len = var->len / zerolen;
    for(size_t i=0; i<new_len; i++) {
	for(size_t j=1; j<zerolen; j++)
	    ((ctype*)var->data)[i] += ((ctype*)var->data)[i+new_len*j];
	((ctype*)var->data)[i] /= zerolen;
    }
    return nct_drop_vardim(var, 0, 1);
}

nct_var* nct_meannan_first_@nctype(nct_var* var) {
    if(var->endpos - var->startpos < var->len)
	nct_load(var);
#if __nctype__==NC_FLOAT || __nctype__==NC_DOUBLE
    size_t zerolen = var->super->dims[var->dimids[0]]->len;
    size_t new_len = var->len / zerolen;
    for(size_t i=0; i<new_len; i++) {
	int count = 0;
	ctype new_value = 0;
	for(size_t j=0; j<zerolen; j++) {
	    ctype test = ((ctype*)var->data)[i+new_len*j];
	    if(test==test) {
		count++;
		new_value += test;
	    }
	}
	((ctype*)var->data)[i] = new_value/count;
    }
    return nct_drop_vardim(var, 0, 1);
#else
    return nct_mean_first_@nctype(var);
#endif
}

void nct__memcpy_double_as_@nctype(void *vdst, const double *src, long n) {
    @ctype *dst = vdst;
    for (long i=0; i<n; i++)
	dst[i] = src[i];
}

#undef ctype
#undef form
#undef __nctype__
