#define ctype @ctype
#define form @form
#define __nctype__ @nctype

/* To also create a wrapper function, add the prototype without dummy arguments into make_functions1.pl.
   Prototype of the wrapper function (the name without "_(at)nctype") should be manually added to nctietue3.h. */

void nct_allocate_varmem(nct_var*); // global but hidden: not in nctietue3.h

static void nct_print_datum_@nctype(const void* vdatum) {
    ctype datum = *(ctype*)vdatum;
#if __nctype__ == NC_FLOAT || __nctype__ == NC_DOUBLE
    if (datum > 0) {
	if (datum < 1e-5 || datum >= 1e7) {
	    printf("%e", datum);
	    return; }
    }
    else if (datum < 0 && (datum > -1e-5 || datum <= -1e7)) {
	    printf("%e", datum);
	    return; }
#endif
    printf("%@form", datum);
}

static void _printhelper_@nctype(ctype* data, long i, long len) {
    if (len < 1)
	return;
    for(; i<len-1; i++) {
	nct_print_datum_@nctype(data+i);
	printf(", ");
    }
    nct_print_datum_@nctype(data+len-1);
}

void nct_print_data_@nctype(const nct_var* var) {
    // TODO: lataa tarvittava data
    if (!var->data)
	return;
    size_t len = var->len;
    if (len <= 17) {
	_printhelper_@nctype(var->data, 0, len);
	return; }
    _printhelper_@nctype(var->data, 0, 8);
    printf(" ..., ");
    _printhelper_@nctype(var->data, len-8, len);
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

nct_anyd nct_max_anyd_@nctype(const nct_var* var) {
#if CHECK_INVALID
    if(!(var->len))
	nct_return_error((nct_anyd){ {0}, -1 });
#endif
#if __nctype__==NC_FLOAT || __nctype__==NC_DOUBLE
    size_t numi=0;
    ctype num = -INFINITY; //TODO: -ffinite-math-only
    for(int i=0; i<var->len; i++)
#else
    size_t numi=0;
    ctype num = ((ctype*)var->data)[0];
    for(size_t i=1; i<var->len; i++)
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
#if CHECK_INVALID
    if(!(var->len))
	nct_return_error((nct_anyd){ {0}, -1 });
#endif
    /* using the first value would not work with nan-values */
#if __nctype__==NC_FLOAT || __nctype__==NC_DOUBLE
    size_t numi=0;
    ctype num = INFINITY;
    for(int i=0; i<var->len; i++)
#else
    size_t numi=0;
    ctype num = ((ctype*)var->data)[0];
    for(size_t i=1; i<var->len; i++)
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

void* nct_minmax_@nctype(const nct_var* var, void* vresult) {
#if CHECK_INVALID
    if(!(var->len))
	nct_return_error(NULL);
#endif
    ctype maxval, minval, *result=vresult;
    /* using the first value would not work with nan-values */
#if __nctype__==NC_FLOAT || __nctype__==NC_DOUBLE
    maxval = -INFINITY;
    minval = INFINITY;
    for(int i=0; i<var->len; i++)
#else
    maxval = minval = ((ctype*)var->data)[0];
    for(int i=1; i<var->len; i++)
#endif
	if(maxval < ((ctype*)var->data)[i])
	    maxval = ((ctype*)var->data)[i];
	else if(minval > ((ctype*)var->data)[i])
	    minval = ((ctype*)var->data)[i];
    result[0] = minval;
    result[1] = maxval;
    return result;
}

void* nct_minmax_nan_@nctype(const nct_var* var, long nanval_long, void* vresult) {
#if CHECK_INVALID
    if (!(var->len))
	nct_return_error(NULL);
#endif
    ctype maxval, minval, *result=vresult, nanval=nanval_long;
#if __nctype__==NC_FLOAT || __nctype__==NC_DOUBLE
#define _my_isnan(a) (!((a) == (a)) || (a) == nanval)
#else
#define _my_isnan(a) ((a) == nanval)
#endif
    int i=0;
    int len = var->len;
    for (; i<len && _my_isnan(((ctype*)var->data)[i]); i++);
    if (i == len) {
	result[0] = result[1] = nanval;
	return result;
    }
    maxval = minval = ((ctype*)var->data)[i];
    for(; i<len; i++) {
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

nct_var* nct_mean_first_@nctype(nct_var* var) {
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

#undef ctype
#undef form
#undef __nctype__
