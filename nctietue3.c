#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "nctietue3.h"
#include <netcdf_mem.h> // must be after netcdf.h from nctietue3.h
/* for multifile read using regex */
#include <regex.h>
#include <dirent.h>
#include <unistd.h>

#define Min(a, b) ((a) < (b) ? a : (b))
#define Max(a, b) ((a) > (b) ? a : (b))

nct_var*	nct_set_concat(nct_var* var0, nct_var* var1, int howmany_left);
#define from_concatlist(var, num) (((nct_var**)(var)->rule[nct_r_concat].arg.v)[num])

#define nct_rnoall (nct_rlazy | nct_rcoord) // if (nct_readflags & nct_rnoall) ...

/* uses private nct_set.rules */
#define setrule(a, r) ((a)->rules |= 1<<(r))
#define hasrule(a, r) ((a)->rules & 1<<(r))
#define anyrule(a, r) ((a)->rules & (r))
#define rmrule(a, r)  ((a)->rules &= ~(1<<(r)))

#define startpass			\
    int __nct_err = nct_error_action;	\
    if (nct_error_action == nct_auto)	\
	nct_error_action = nct_pass

#define endpass nct_error_action = __nct_err

#define print_varerror(var, indent)  do {	\
    FILE* tmp = stdout;				\
    stdout = nct_stderr? nct_stderr: stderr;	\
    nct_print_var_meta(var, indent);		\
    stdout = tmp;				\
} while(0)

#define cannot_free(var) (var->not_freeable || (var->nusers && *var->nusers))

/* Use of this macro should be avoided.
 * Usage:
 *	#define ONE_TYPE(a,b,c) (Your code)
 *	ALL_TYPES;
 *      #undef ONE_TYPE
 */
#define ALL_TYPES					\
    ONE_TYPE(NC_BYTE, hhi, char)			\
	ONE_TYPE(NC_UBYTE, hhu, unsigned char)		\
	ONE_TYPE(NC_CHAR, c, char)			\
	ONE_TYPE(NC_SHORT, hi, short)		        \
	ONE_TYPE(NC_USHORT, hu, unsigned short)		\
	ONE_TYPE(NC_INT, i, int)			\
	ONE_TYPE(NC_UINT, u, unsigned)			\
	ONE_TYPE(NC_UINT64, llu, long long unsigned)	\
	ONE_TYPE(NC_INT64, lli, long long int)		\
	ONE_TYPE(NC_FLOAT, f, float)			\
	ONE_TYPE(NC_DOUBLE, lf, double)

/* Used similarly than the macro above. */
#define TIMEUNITS TIMEUNIT(milliseconds) TIMEUNIT(seconds) TIMEUNIT(minutes) TIMEUNIT(hours) TIMEUNIT(days)

static long ms_per_timeunit[]	= {1,	1*1000,	60*1000,	3600*1000,	86400*1000};

#define ONE_TYPE(nctype,b,ctype) [nctype] = #ctype,
static const char* const nct_typenames[] = { ALL_TYPES };
#undef ONE_TYPE

#define TIMEUNIT(arg) [nct_##arg] = #arg,
static const char* const nct_timeunits[] = { TIMEUNITS };
#undef TIMEUNIT

#define ncfunk_open(name, access, idptr)			\
    do {							\
	if ((nct_ncret = nc_open(name, access, idptr))) {	\
	    nct_backtrace();					\
	    ncerror(nct_ncret);					\
	    fprintf(nct_stderr? nct_stderr: stderr, "    failed to open \"\033[1m%s\033[0m\"\n", (char*)name);	\
	    nct_other_error;					\
	}							\
    } while(0)

int nct_readflags, nct_ncret, nct_error_action, nct_verbose, nct_register;
FILE* nct_stderr;
void (*nct_after_load)(nct_var*, void*, size_t, const size_t*, const size_t*) = NULL;
const short nct_typelen[] = {
    [NC_BYTE]=1, [NC_UBYTE]=1, [NC_SHORT]=2, [NC_USHORT]=2,
    [NC_INT]=4, [NC_UINT]=4, [NC_INT64]=8, [NC_UINT64]=8,
    [NC_FLOAT]=4, [NC_DOUBLE]=8, [NC_CHAR]=1, [NC_STRING]=sizeof(void*)
};

const char* nct_backtrace_str = NULL;
const char* nct_backtrace_file = NULL;
int nct_backtrace_line = 0;

const char* nct_error_color	= "\033[1;91m";
const char* nct_backtrace_color	= "\033[38;5;178m";
const char* nct_varset_color	= "\033[1;35m";
const char* nct_varname_color	= "\033[92m";
const char* nct_dimname_color	= "\033[44;92m";
const char* nct_type_color	= "\033[93m";
const char* nct_att_color	= "\033[3;38;5;159m";
const char* nct_default_color	= "\033[0m";

static void     _nct_free_var(nct_var*);
static void	_nct_free_att(nct_att*);
static nct_set* nct_read_ncf_lazy_gd(nct_set* dest, const void* filename, int flags);
static nct_set* nct_read_ncf_lazy(const void* filename, int flags);
static nct_set* nct_after_lazyread(nct_set* s, int flags);

void* nct_getfun[] = {
    [NC_NAT]    = nc_get_var,
    [NC_BYTE]   = nc_get_var_schar,
    [NC_CHAR]   = nc_get_var,
    [NC_SHORT]  = nc_get_var_short,
    [NC_INT]    = nc_get_var_int,
    [NC_FLOAT]  = nc_get_var_float,
    [NC_DOUBLE] = nc_get_var_double,
    [NC_UBYTE]  = nc_get_var_uchar,
    [NC_USHORT] = nc_get_var_ushort,
    [NC_UINT]   = nc_get_var_uint,
    [NC_INT64]  = nc_get_var_longlong,
    [NC_UINT64] = nc_get_var_ulonglong,
};

void* nct_getfun_partial[] = {
    [NC_NAT]    = nc_get_vara,
    [NC_BYTE]   = nc_get_vara_schar,
    [NC_CHAR]   = nc_get_vara,
    [NC_SHORT]  = nc_get_vara_short,
    [NC_INT]    = nc_get_vara_int,
    [NC_FLOAT]  = nc_get_vara_float,
    [NC_DOUBLE] = nc_get_vara_double,
    [NC_UBYTE]  = nc_get_vara_uchar,
    [NC_USHORT] = nc_get_vara_ushort,
    [NC_UINT]   = nc_get_vara_uint,
    [NC_INT64]  = nc_get_vara_longlong,
    [NC_UINT64] = nc_get_vara_ulonglong,
};

void* nct_getfun_1[] = {
    [NC_NAT]    = nc_get_var1,
    [NC_BYTE]   = nc_get_var1_schar,
    [NC_CHAR]   = nc_get_var1,
    [NC_SHORT]  = nc_get_var1_short,
    [NC_INT]    = nc_get_var1_int,
    [NC_FLOAT]  = nc_get_var1_float,
    [NC_DOUBLE] = nc_get_var1_double,
    [NC_UBYTE]  = nc_get_var1_uchar,
    [NC_USHORT] = nc_get_var1_ushort,
    [NC_UINT]   = nc_get_var1_uint,
    [NC_INT64]  = nc_get_var1_longlong,
    [NC_UINT64] = nc_get_var1_ulonglong,
};

#include "internals.h"
#include "transpose.c"

#ifdef HAVE_PROJ
#include "extra/nctproj.h"
#include "extra/nctproj.c"
#endif

#ifdef HAVE_LZ4
#include "extra/lz4.c"
#endif

static struct nct_fileinfo_t* _nct_link_fileinfo(struct nct_fileinfo_t*);

void nct_verbose_line_ending() {
    if (nct_verbose == nct_verbose_overwrite)
	printf("\033[K\r"), fflush(stdout);
    else if (nct_verbose == nct_verbose_newline)
	putchar('\n');
}

nct_var* nct_add_dim(nct_set* set, size_t len, char* name) {
    if (set->dimcapacity < set->ndims+1) {
	void* vp;
	int capac = set->ndims+1+2;
	if (!(vp = realloc(set->dims, capac*sizeof(void*))))
	    goto failed;
	set->dims = vp++;
	set->dimcapacity = capac;
    }
    int id = set->ndims++;
    *(set->dims[id] = malloc(sizeof(nct_var))) = (nct_var) {
	.super	= set,
	.id_dim	= nct_dimid_(id),
	.ncid	= -1,
	.name	= name,
	.len	= len,
    };
    return set->dims[id];
failed:
    nct_puterror("realloc failed in nct_add_dim: %s\n", strerror(errno));
    nct_return_error(NULL);
}

nct_var* nct_add_var(nct_set* set, void* src, nc_type dtype, char* name,
		     int ndims, int* dimids) {
    if (set->varcapacity < set->nvars+1)
	if (!(set->vars = realloc(set->vars, (set->varcapacity=set->nvars+3)*sizeof(void*))))
	    goto failed;
    nct_var* var = set->vars[set->nvars] = malloc(sizeof(nct_var));
    *var = (nct_var) {
	.super       = set,
	.id_var      = nct_varid_(set->nvars),
	.ncid        = -1,
	.name        = name,
	.ndims       = ndims,
	.dimcapacity = ndims+1,
	.dimids      = malloc((ndims+1)*sizeof(int)),
	.dtype       = dtype,
	.data        = src,
    };
    if(!var->dimids)
	goto failed;
    memcpy(var->dimids, dimids, ndims*sizeof(int));
    set->nvars++;
    var->len = nct_get_len_from(var, 0);
    var->endpos = var->len; // User will provide the data before using it.
    return var;
failed:
    startpass;
    nct_puterror("(re/m)alloc failed in nct_add_var: %s\n", strerror(errno));
    nct_return_error(NULL);
    endpass; // TODO: this is never reached which is a bug
}

nct_var* nct_add_var_alldims(nct_set* set, void* src, nc_type dtype, char* name) {
    int ndims = set->ndims;
    if (ndims <= 5) {
	int dimids[] = {0,1,2,3,4};
	return nct_add_var(set, src, dtype, name, ndims, dimids); }
    int dimids[ndims];
    for(int i=0; i<ndims; i++)
	dimids[i] = i;
    return nct_add_var(set, src, dtype, name, ndims, dimids);
}

nct_att* nct_add_varatt(nct_var* var, nct_att* att) {
    void* vp;
    if(var->attcapacity < var->natts+1) {
	if(!(vp=realloc(var->atts, (var->attcapacity=var->natts+3)*sizeof(nct_att))))
	    goto failed;
	var->atts = vp;
    }
    var->atts[var->natts++] = *att;
    return var->atts + var->natts-1;
failed:
    startpass;
    var->attcapacity = var->natts;
    nct_puterror("realloc failed in nct_add_varatt_text.\n");
    print_varerror(var, "    ");
    nct_return_error(NULL);
    endpass; // TODO: this is never reached which is a bug
}

nct_att* nct_add_varatt_text(nct_var* var, char* name, char* value, unsigned freeable) {
    nct_att att = {
	.name     = name,
	.value    = value,
	.dtype    = NC_CHAR,
	.len      = value? strlen(value)+1: 0,
	.freeable = freeable
    };
    return nct_add_varatt(var, &att);
}

nct_var* nct_add_vardim_first(nct_var* var, int dimid) {
    if (var->dimcapacity < var->ndims+1) {
	var->dimcapacity = var->ndims+2;
	var->dimids = realloc(var->dimids, var->dimcapacity*sizeof(int));
    }
    memmove(var->dimids+1, var->dimids, var->ndims*sizeof(int));
    var->dimids[0] = dimid;
    var->ndims++;
    var->len *= var->super->dims[dimid]->len;
    return var;
}

nct_set* nct_clear(nct_set* set) {
    int owner = set->owner;
    set->owner = 0; // keep the nct_set object allocated
    nct_free1(set);
    *set = (nct_set){0};
    set->owner = owner;
    return set;
}

void nct_close_nc(int *ncid) {
    ncfunk(nc_close, *ncid);
    *ncid = -1;
}

void nct_allocate_varmem(nct_var* var) {
    if (var->capacity >= var->len)
	return;
    if cannot_free(var)
	var->data = malloc(var->len*nctypelen(var->dtype));
    else {
	if (var->data)
	    free(var->data);
	var->data = malloc(var->len*nctypelen(var->dtype));
    }
    var->capacity = var->len;
    if (var->data)
	return;

    nct_puterror("memory allocation failed: %s\n", strerror(errno));
    print_varerror(var, "    ");
    var->capacity = 0;
    nct_other_error;
}

/* A new dimension needs to be added only to the first set. */
static nct_var* _nct_concat_handle_new_dim(nct_var *var, nct_set *concatenation) {
    static nct_var *dim;
    static nct_set *previous_concatenation;
    if (!var) {
	dim = NULL;
	previous_concatenation = NULL;
	return dim; }

    if (!dim)
	dim = nct_ensure_unique_name(nct_add_dim(var->super, 1, "dimension"));

    dim->len += concatenation != previous_concatenation;
    previous_concatenation = concatenation;

    int dimid = nct_dimid(dim);
    if (dimid < 0) {
	nct_puterror("corrupted dimension in concatenation\n");
	return dim = NULL; }
    if (nct_get_vardimid(var, dimid) >= 0)
	return dim;
    long len = var->len;
    nct_add_vardim_first(var, dimid);
    var->len = len; // concatenation will also increase length so let's not do it twice
    return dim;
}

/* Concatenation is currently not supported along other existing dimensions than the first one.
   Coordinates will be loaded if aren't already.
   Variable will not be loaded, except if the first is loaded and the second is not.
   When unloaded sets are concatenated, they must not be freed before loading the data.
   The following is hence an error:
   *	concat(set0, set1) // var n is not loaded
   *	free(set1)
   *	load(set0->vars[n])
   */
nct_set* nct_concat_varids_name(nct_set *vs0, nct_set *vs1, char* dimname, int howmany_left, const int* varids0, int nvars) {
    int dimid0 = nct_get_dimid(vs0, dimname);
    if (dimid0 < 0)
	dimid0 = nct_dimid(nct_add_dim(vs0, 1, dimname));
    else if (vs0->dims[dimid0]->len == 0) {
	/* Change length zero to one. We assume that such dimension does not belong to any variable. */
	nct_var* dim = vs0->dims[dimid0];
	dim->len = 1;
	if (nct_iscoord(dim)) {
	    nct_allocate_varmem(dim); // There is no data, but not having memory might be an error.
	    memset(dim->data, 0, nctypelen(dim->dtype));
	}
    }

    /* Concatenate the dimension. */
    int dimid1 = nct_get_dimid(vs1, dimname);
    if (dimid1 < 0)
	vs0->dims[dimid0]->len++;
    else {
	/* Change length zero to one. We assume that such dimension does not belong to any variable. */
	if (vs1->dims[dimid1]->len == 0) {
	    nct_var* dim = vs1->dims[dimid1];
	    dim->len = 1;
	    if (nct_iscoord(dim)) {
		nct_allocate_varmem(dim); // There is no data, but not having memory might be an error.
		memset(dim->data, 0, nctypelen(dim->dtype));
	    }
	}
	vs0->dims[dimid0]->len += vs1->dims[dimid1]->len;

	/* If the dimension is also a variable, concat that */
	int varid0 = nct_get_varid(vs0, dimname);
	int varid1 = nct_get_varid(vs1, dimname);

	if (varid0 >= 0 && varid1 >= 0) {
	    nct_att* att = nct_get_varatt(vs0->vars[varid0], "units"); // convert timeunits if the dimension is time
	    if (att)
		nct_convert_timeunits(vs1->vars[varid1], att->value);
	    if (!vs0->vars[varid0]->data)
		nct_load(vs0->vars[varid0]);
	    if (!vs1->vars[varid1]->data)
		nct_load(vs1->vars[varid1]);
	    _nct_concat_var(vs0->vars[varid0], vs1->vars[varid1], dimid0, howmany_left);
	}
    }

    int dimname_is_new = 1;
    nct_foreach(vs0, var)
	if (nct_get_vardimid(var, dimid0) >= 0) {
	    dimname_is_new = 0;
	    break; }

    /* Concatenate all variables.
       Called concat-functions change var->len but not vardim->len which has already been changed above
       so that changes would not cumulate when having multiple variables. */
    int iloop = 0;
    nct_var* var0 = varids0 ? vs0->vars[varids0[iloop]] : nct_firstvar(vs0);
    do {
	nct_var* var1 = nct_get_var(vs1, var0->name);
	if (!var1)
	    continue;
	if (dimname_is_new) {
	    size_t len = var0->len;
	    nct_add_vardim_first(var0, dimid0);
	    var0->len = len; // concatenation will also increase length so let's not do it twice
	}
	else if (nct_get_vardimid(var0, dimid0) < 0)
	    _nct_concat_handle_new_dim(var0, vs1);
	if (var0->endpos < var0->len)
	    /* To be concatenated when loaded. */
	    nct_set_concat(var0, var1, howmany_left);
	else {
	    /* Concatenate now. */
	    if(var1->endpos-var1->startpos < var1->len)
		nct_load(var1); // Now data is written here and copied to var0. Not good.
	    _nct_concat_var(var0, var1, dimid0, howmany_left);
	}
    } while (++iloop != nvars && (
	    (varids0 && (var0 = vs0->vars[varids0[iloop]])) ||
	    (var0 = nct_nextvar(var0))));

    if (howmany_left == 0)
	_nct_concat_handle_new_dim(NULL, NULL); // to reset the dimension to be added
    return vs0;
}

/* Called from concatenate -v:args */
char* _makename_concat_v(nct_var *var, const char* args) {
    int capac = 256, newlen=0;
    char *new = malloc(capac);

    while (*args) {
	if (*args != '$') {
	    if (newlen+1 < capac)
		new = realloc(new, capac = newlen+256);
	    new[newlen++] = *args++;
	}
	else {
	    args++;
	    int num = 0;
	    const char *copy = NULL;
	    int copylen;
	    while (1) {
		if ('0' <= *args && *args <= '9') {
		    num = num*10 + *args-'0';
		    args++;
		    continue; }
		else if (*args == '@')
		    copy = var->name;
		else if (*args == '$')
		    copy = "$";
		else if (num) {
		    copy = nct_get_filename_capture(var->super, num, &copylen);
		    goto come_from_num;
		}
		break;
	    }
	    copylen = strlen(copy);
	    args++;
come_from_num:
	    if (newlen + copylen < capac)
		new = realloc(new, capac = newlen+copylen+256);
	    memcpy(new+newlen, copy, copylen);
	    newlen += copylen;
	}
    } // while (*args)
    new[newlen] = '\0';
    return new;
}

nct_set* nct_concat_varids(nct_set *vs0, nct_set *vs1, char* dimname, int howmany_left, const int* varids0, int nvars) {
    if (!dimname)
	dimname = "-0";
    if (dimname[0] == '-') {
	int dimid0;
	/* A number tells which vardim to concatenate along. (TODO:not) Defined based on the dimensions of the first var. */
	if (sscanf(dimname+1, "%i", &dimid0) == 1) {
	    nct_foreach(vs0, v)
		if (dimid0 < v->ndims) {
		    dimname = vs0->dims[v->dimids[dimid0]]->name;
		    break;
		}
	}
	/* Not a concatenation but useful. */
	else if (!strncmp(dimname, "-v", 2)) {
	    nct_foreach(vs1, var1) {
		// nct_load(var1); // not needed since var->fileinfo was added
		/* -v:args tells how to rename the variables */
		if (dimname[2] == ':') {
		    char *name = _makename_concat_v(var1, dimname+3);
		    nct_rename(var1, name, 1);
		}
		nct_var* var = nct_copy_var(vs0, var1, 1);
		if (!var->fileinfo)
		    var->fileinfo = _nct_link_fileinfo(var1->super->fileinfo);
		nct_ensure_unique_name(var);
	    }
	    /* The first set should also be renamed similarily but only once. */
	    if (dimname[2] == ':' && howmany_left == 0)
		nct_foreach(vs0, var) {
		    if (var->fileinfo) // Whether this is a concatenated variable or original in vs0.
			break;
		    char *name = _makename_concat_v(var, dimname+3);
		    nct_rename(var, name, 1);
		}
	    return vs0;
	}
	else
	    dimname++; // don't include the hyphen
    }
    return nct_concat_varids_name(vs0, vs1, dimname, howmany_left, varids0, nvars);
}

nct_set* nct_concat(nct_set *vs0, nct_set *vs1, char* dimname, int howmany_left) {
    return nct_concat_varids(vs0, vs1, dimname, howmany_left, NULL, -1);
}

long long nct_convert_time_anyd(time_t time, nct_anyd units) {
    long long offset_ms = (time - units.a.t) * 1000;
    long ms_per_unit = nct_get_interval_ms(units.d);
    return offset_ms / ms_per_unit;
}

nct_var* nct_convert_timeunits(nct_var* var, const char* units) {
    nct_att* att = nct_get_varatt(var, "units");
    if(!att)
	return NULL;
    if(!strcmp(att->value, units))
	return var; // already correct
    time_t sec0, sec1;
    nct_anyd time0_anyd = nct_timegm0(var, NULL);
    if(time0_anyd.d < 0)
	return NULL;
    sec0 = timegm(nct_gmtime(1, time0_anyd)) - timegm(nct_gmtime(0, time0_anyd)); // days -> 86400 etc.
    if(att->freeable & nct_ref_content)
	free(att->value);

    /* change the attribute */
    att->value = strdup(units);
    att->freeable |= nct_ref_content;
    nct_anyd time1_anyd = nct_timegm0(var, NULL); // different result than time0_anyd, since att has been changed
    if(time1_anyd.d < 0)
	return NULL;

    if (!var->data)
	if (!nct_load(var)) {
	    if (!var->dtype)
		var->dtype = time1_anyd.d <= nct_seconds ? NC_INT64 : NC_INT;
	    nct_put_interval(var, 0, 1);
	}

    sec1 = timegm(nct_gmtime(1, time1_anyd)) - timegm(nct_gmtime(0, time1_anyd)); // days -> 86400 etc.
    int len = var->len;
    switch (var->dtype) {
	case NC_FLOAT:
	    for(int i=0; i<len; i++) {
		time_t t = time0_anyd.a.t + ((float*)var->data)[i]*sec0;
		time_t diff_t1 = t - time1_anyd.a.t;
		((float*)var->data)[i] = diff_t1/sec1;
	    }
	    break;
	case NC_DOUBLE:
	    for(int i=0; i<len; i++) {
		time_t t = time0_anyd.a.t + ((double*)var->data)[i]*sec0;
		time_t diff_t1 = t - time1_anyd.a.t;
		((double*)var->data)[i] = diff_t1/sec1;
	    }
	    break;
	default:
	    int s1 = nctypelen(var->dtype);
	    for(int i=0; i<len; i++) {
		/* seconds from original offset */
		time_t t = time0_anyd.a.t + nct_get_integer(var, i)*sec0;
		/* seconds from new offset */
		time_t diff_t1 = t - time1_anyd.a.t;
		/* convert seconds to new unit and set the value */
		long long val = diff_t1/sec1;
		memcpy(var->data+s1*i, &val, s1);
	    }
    }
    return var;
}

nct_att* nct_copy_att(nct_var* var, const nct_att* src) {
    long len = src->len * nctypelen(src->dtype);
    nct_att att = {
	.name	= strdup(src->name),
	.value	= malloc(len),
	.dtype	= src->dtype,
	.len	= src->len,
	.freeable = nct_ref_content | nct_ref_name,
    };
    memcpy(att.value, src->value, len);
    return nct_add_varatt(var, &att);
}

nct_var* nct_copy_var(nct_set* dest, nct_var* src, int link) {
    nct_var* var;
    int n = src->ndims;
    int dimids[n];
    nct_var *dstdim;
    for(int i=0; i<n; i++) {
	nct_var* srcdim = src->super->dims[src->dimids[i]];
	dimids[i] = nct_get_dimid(dest, srcdim->name);
	/* Create the dimension if not present in dest. */
	/* Create a new dimension if lengths mismatch in source and destination. */
	if (dimids[i] < 0 || dest->dims[dimids[i]]->len != srcdim->len) {
	    nct_var* dim = nct_add_dim(dest, srcdim->len, strdup(srcdim->name));
	    dim->freeable_name = 1;
	    nct_ensure_unique_name(dim);
	    dimids[i] = nct_dimid(dim);
	}
	dstdim = dest->dims[dimids[i]];
	if (nct_iscoord(srcdim) && !nct_iscoord(dstdim))
	    _nct_copy_var_internal(nct_dim2coord(dstdim, NULL, srcdim->dtype), srcdim, 0);
    }
    if (!nct_iscoord(src))
	var = nct_add_var(dest, NULL, src->dtype, strdup(src->name), n, dimids);
    else
	var = dstdim;
    var->fileinfo = _nct_link_fileinfo(src->fileinfo ? src->fileinfo : src->super->fileinfo);
    var->ncid = src->ncid;
    var->nfiledims = src->nfiledims;
    memcpy(var->filedimensions, src->filedimensions, sizeof(var->filedimensions[0]) * var->nfiledims);
    var->freeable_name = 1;
    return _nct_copy_var_internal(var, src, link);
}

nct_set* nct_copy(nct_set *toset, const nct_set *fromset, int link) {
    *toset = (nct_set){0};
    for (int i=0; i<fromset->ndims; i++) {
	nct_var *dim = fromset->dims[i];
	char *name = dim->freeable_name ? strdup(dim->name) : dim->name;
	nct_add_dim(toset, dim->len, name)->freeable_name = dim->freeable_name;
    }
    for (int i=0; i<fromset->nvars; i++)
	nct_copy_var(toset, fromset->vars[i], link);
    return toset;
}

/* The compiler doesn't understand that we have checked that ndims < 100
   and hence, sprintf(dimname+1, "%i" ndims++) won't overflow. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-overflow"
static nct_set* nct_create_simple_v_gd(nct_set* s, void* dt, int dtype, va_list args) {
    size_t len;
    int ndims = 0;
    char dimname[] = "e0\0\0";
    while((len = va_arg(args, size_t)) > 0 && ndims < 100) {
	sprintf(dimname+1, "%i", ndims++);
	nct_add_dim(s, len, strdup(dimname)) -> freeable_name=1;
    }
    va_end(args);
    int dimids[ndims];
    for(int i=0; i<ndims; i++)
	dimids[i] = i;
    nct_add_var(s, dt, dtype, "data", ndims, dimids);
    return s;
}
#pragma GCC diagnostic pop

nct_set* _nct_create_simple(void* dt, int dtype, ...) {
    nct_set* s = calloc(1, sizeof(nct_set));
    va_list args;
    va_start(args, dtype);
    nct_create_simple_v_gd(s, dt, dtype, args) -> owner = 1;
    return s;
}

nct_set* _nct_create_simple_gd(nct_set* s, void* dt, int dtype, ...) {
    va_list args;
    va_start(args, dtype);
    return nct_create_simple_v_gd(s, dt, dtype, args);
}

nct_var* nct_dim2coord(nct_var* var, void* src, nc_type dtype) {
    nct_set* set = var->super;
    if (set->varcapacity < set->nvars+1)
	if (!(set->vars = realloc(set->vars, (set->varcapacity=set->nvars+3)*sizeof(void*))))
	    goto failed;
    set->vars[set->nvars] = var;
    int dimid = var->id_dim;
    *var = (nct_var){
	.super		= var->super,
	.id_var		= nct_varid_(set->nvars),
	.id_dim		= dimid,
	.ncid		= var->ncid,
	.name		= var->name,
	.freeable_name	= var->freeable_name,
	.ndims		= 1,
	.dimcapacity	= 1,
	.dimids		= malloc(1*sizeof(int)),
	.natts		= var->natts,
	.attcapacity	= var->attcapacity,
	.atts		= var->atts,
	.len		= var->len,
	.capacity	= src? var->len: 0,
	.dtype		= dtype,
	.data		= src,
    };
    if (!var->dimids)
	goto failed;
    var->dimids[0] = nct_dimid(var);
    set->nvars++;
    return var;
failed:
    nct_puterror("(re/m)alloc failed in nct_add_var: %s\n", strerror(errno));
    nct_return_error(NULL);
}

nct_var* nct_coord2dim(nct_var* var) {
    var->dimids = (free(var->dimids), NULL);
    var->ndims = 0;
    for(int i=var->natts-1; i>=0; i--)
	_nct_free_att(var->atts+i);
    var->atts = (free(var->atts), NULL);
    var->natts = var->attcapacity = 0;
    nct_unlink_data(var);
    var->dtype = NC_NAT;
    _nct_drop_var(var);
    var->ncid = -1;
    var->id_var = 0;
    return var;
}

double nct_diff_at_floating(nct_var* var, long ind) {
    return nct_get_floating(var, ind+1) - nct_get_floating(var, ind);
}

long long nct_diff_at_integer(nct_var* var, long ind) {
    return nct_get_integer(var, ind+1) - nct_get_integer(var, ind);
}

nct_var* nct_drop_vardim(nct_var* var, int dim, int shrink) {
    size_t new_len = var->len / var->super->dims[var->dimids[dim]]->len;
    if (shrink && !cannot_free(var)) {
	var->data = realloc(var->data, new_len*nctypelen(var->dtype));
	var->capacity = new_len;
    }
    var->len = new_len;
    int* ptr = var->dimids;
    int new_ndims = --var->ndims;
    memmove(ptr+dim, ptr+dim+1, new_ndims*sizeof(int)-dim);
    return var;
}

nct_var* nct_drop_vardim_first(nct_var* var) {
    return nct_drop_vardim(var, 0, 1);
}

nct_var* nct_ensure_unique_name(nct_var* var) {
    const char* restrict name = var->name;
    int i;
    int thisid = nct_varid(var);
    nct_var **vars = var->super->vars;
    int nvars = var->super->nvars;
    for(int j=0; j<2; j++) {
	for(i=0; i<thisid; i++)
	    if (!strcmp(vars[i]->name, name))
		goto makename;
	for(i=thisid+1; i<nvars; i++)
	    if (!strcmp(vars[i]->name, name))
		goto makename;
	thisid = nct_dimid(var);
	vars = var->super->dims;
	nvars = var->super->ndims;
    }
    return var;

makename:
    char* newname = nct_find_unique_name_from(var->super, var->name, 0);
    if(var->freeable_name)
	free(var->name);
    var->name = newname;
    var->freeable_name = 1;
    return var;
}

nct_var* nct_expand_dim(nct_var* dim, int howmuch, int start0_end1, nct_any fill) {
    dim->len += howmuch;
    int dimid = nct_dimid(dim);
    if nct_iscoord(dim)
	_nct_expand_var(dim, 0, howmuch, start0_end1, fill);
    nct_foreach(dim->super, var) {
	int ndims = var->ndims;
	for(int i=0; i<ndims; i++)
	    if (var->dimids[i] == dimid) {
		_nct_expand_var(var, i, howmuch, start0_end1, fill);
		break; }
    }
    return dim;
}

void nct_finalize() {
    nc_finalize();
}

char* nct_find_unique_name_from(nct_set* set, const char* initname, int num) {
    if(!initname)
	initname = "var";
    char newname[strlen(initname)+16];
    strcpy(newname, initname);
    char* ptr = newname + strlen(newname);
    for(int i=0; i<99999; i++) {
	sprintf(ptr, "%i", i+num);
	nct_foreach(set, var)
	    if(!strcmp(newname, var->name))
		goto next;
	int ndims = set->ndims;
	for(int i=0; i<ndims; i++)
	    if(!strcmp(newname, set->dims[i]->name))
		goto next;
	return strdup(newname);
next:;
    }
    nct_return_error(NULL);
}

long nct_bsearch_(const nct_var* var, double value, int beforeafter) {
    double (*getfun)(const nct_var*, size_t) = var->data ? nct_get_floating : nct_getl_floating;
    size_t sem[] = {0, var->len-1, (var->len)/2}; // start, end, mid
    if (var->endpos)
	sem[1] = Min(var->endpos - var->startpos - 1, var->len-1);
    while (1) {
	if (sem[1]-sem[0] <= 1)
	    break;
	double try = getfun(var, sem[2]);
	sem[try>value] = sem[2]; // if (try>value) end = mid; else start = mid;
	sem[2] = (sem[0]+sem[1]) / 2;
    }
    double v0 = getfun(var, sem[0]),
	   v1 = getfun(var, sem[1]);
    nct_register = !(value==v0 || value==v1);
    long ret =
	value<v0  ? sem[0] :
	value==v0 ? sem[0] + (beforeafter==1) :
	value<v1  ? sem[1] :
	value==v1 ? sem[1] + (beforeafter==1) :
	sem[1] + 1;
    ret -= beforeafter == -2;
    ret -= beforeafter == -1 && nct_register;
    return ret;
}

long nct_find_sorted_(const nct_var* var, double value, int beforeafter) __attribute__((deprecated, alias("nct_bsearch_")));

long nct_bsearch_time(const nct_var* var, time_t time, int beforeafter) {
    struct tm tm0;
    nct_anyd epoch = nct_timegm0(var, &tm0);
    long diff_s = time - epoch.a.t;
    double tofind = diff_s * 1000 / nct_get_interval_ms(epoch.d);
    return nct_bsearch(var, tofind, beforeafter);
}

long nct_bsearch_time_str(const nct_var* dim, const char *timestr, int beforeafter) {
    struct tm tm;
    nct__read_timestr(timestr, &tm);
    return nct_bsearch_time(dim, timegm(&tm), beforeafter);
}

nct_var* nct_firstvar(const nct_set* set) {
    int nvars = set->nvars;
    for(int i=0; i<nvars; i++)
	if(!nct_iscoord(set->vars[i]))
	    return set->vars[i];
    return NULL;
}

nct_var* nct_nextvar(const nct_var* var) {
    nct_set* set = var->super;
    int nvars = set->nvars;
    for(int i=nct_varid(var)+1; i<nvars; i++)
	if(!nct_iscoord(set->vars[i]))
	    return set->vars[i];
    return NULL;
}

nct_var* nct_prevvar(const nct_var* var) {
    nct_set* set = var->super;
    for(int i=nct_varid(var)-1; i>=0; i--)
	if(!nct_iscoord(set->vars[i]))
	    return set->vars[i];
    return NULL;
}

nct_var* nct_lastvar(const nct_set* set) {
    for(int i=set->nvars-1; i>=0; i--)
	if(!nct_iscoord(set->vars[i]))
	    return set->vars[i];
    return NULL;
}

static void _nct_free_att(nct_att* att) {
    if(att->dtype == NC_STRING)
	nc_free_string(att->len, att->value);
    if(att->freeable & nct_ref_content)
	free(att->value);
    if(att->freeable & nct_ref_name)
	free(att->name);
    att->freeable = 0;
}

void _nct_free(int _, ...) {
    va_list args;
    va_start(args, _);
    intptr_t addr;
    while((int)(addr = va_arg(args, intptr_t)) != -1)
	nct_free1((nct_set*)addr);
    va_end(args);
}

static struct nct_fileinfo_t* _nct_link_fileinfo(struct nct_fileinfo_t *fileinfo) {
    if (fileinfo)
	fileinfo->nusers++;
    return fileinfo;
}

/* uses private nct_set.fileinfo */
static void _nct_unlink_fileinfo(struct nct_fileinfo_t *fileinfo) {
    if (!fileinfo || --fileinfo->nusers)
	return;
    if (fileinfo->ismem_t) {
	struct nct_fileinfo_mem_t* info = (void*)fileinfo;
	if (info->owner & nct_ref_content)
	    free(info->content);
	/* If these pragmas come after the if-statement, surrounding only the free-statement,
	   the if-condition is omitted. Seems like a bug in GCC. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
	if (info->owner & nct_ref_name)
	    free((char*)info->fileinfo.name);
#pragma GCC diagnostic pop
	free(info->fileinfo.groups);
    }
    else {
	/* Always owner unless readmem flags is present. Quite unlogical. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
	free(fileinfo->name);
#pragma GCC diagnostic pop
	free(fileinfo->groups);
    }
    if (fileinfo->ncid > 0)
	nct_close_nc(&fileinfo->ncid);
    free(fileinfo);
}

static void _nct_free_var(nct_var* var) {
    free(var->dimids);
    for(int i=0; i<var->natts; i++)
	_nct_free_att(var->atts+i);
    free(var->atts);
    if (var->dtype == NC_STRING)
	for(int i=0; i<var->len; i++)
	    free(((char**)var->data)[i]);
    nct_unlink_data(var);
    nct_unlink_stream(var);
    var->capacity = 0;
    if (var->freeable_name)
	free(var->name);
    if hasrule(var, nct_r_concat)
	free(var->rule[nct_r_concat].arg.v);
    free(var->stack);
    if (var->fileinfo)
	_nct_unlink_fileinfo(var->fileinfo);
    var->fileinfo = NULL;
}

void nct_free1(nct_set* set) {
    if (!set) return;
    int n;
    n = set->natts;
    for(int i=0; i<n; i++)
	_nct_free_att(set->atts+i);
    free(set->atts);
    n = set->nvars;
    for(int i=0; i<n; i++) {
	if (nct_iscoord(set->vars[i]))
	    continue;
	_nct_free_var(set->vars[i]);
	memset(set->vars[i], 0, sizeof(nct_var));
	free(set->vars[i]);
    }
    n = set->ndims;
    for(int i=0; i<n; i++) {
	_nct_free_var(set->dims[i]);
	memset(set->dims[i], 0, sizeof(nct_var));
	free(set->dims[i]);
    }
    memset(set->vars, 0, sizeof(void*)*set->nvars);
    free(set->vars);
    memset(set->dims, 0, sizeof(void*)*set->ndims);
    free(set->dims);
    startpass;
    if (set->ncid > 0)
	nct_close_nc(&set->ncid);
    endpass;
    set->fileinfo = (_nct_unlink_fileinfo(set->fileinfo), NULL);
    if hasrule(set, nct_r_list) {
	nct_set* ptr = set;
	nct_set nullset = {0};
	/* List contains structs and not pointers to them. Hence memcmp instead of while(++ptr). */
	while (memcmp(++ptr, &nullset, sizeof(nct_set)))
	    nct_free1(ptr);
    }
    if (set->owner)
	free(set);
}

void nct_get_coords_from_ind(const nct_var* var, size_t* out, size_t ind) {
    int ndims = var->ndims;
    for(int i=0; i<ndims; i++) {
	size_t nextlen = nct_get_len_from(var, i+1);
	out[i] = ind / nextlen;
	ind %= nextlen;
    }
}

size_t nct_get_ind_from_coords(const nct_var* var, const size_t* coords) {
    size_t ind = 0;
    size_t cum = 1;
    for (int i=var->ndims-1; i>=0; i--) {
	ind += cum * coords[i];
	cum *= nct_get_vardim(var, i)->len;
    }
    return ind;
}

nct_var* nct_get_dim(const nct_set* set, const char* name) {
    int ndims = set->ndims;
    for(int i=0; i<ndims; i++)
	if (!strcmp(name, set->dims[i]->name)) {
	    nct_register = i;
	    return set->dims[i];
	}
    return NULL;
}

nct_var* nct_get_var(const nct_set* set, const char* name) {
    int nvars = set->nvars;
    for(int i=0; i<nvars; i++)
	if (!strcmp(name, set->vars[i]->name)) {
	    nct_register = i;
	    return set->vars[i];
	}
    return NULL;
}

nct_att* nct_get_varatt(const nct_var* var, const char* name) {
    int natts = var->natts;
    for(int i=0; i<natts; i++)
	if(!strcmp(var->atts[i].name, name)) {
	    nct_register = i;
	    return var->atts+i;
	}
    return NULL;
}

double nct_get_varatt_floating(const nct_var *var, const char *name, int ind) {
    nct_att *att = nct_get_varatt(var, name);
    if (!att) {
	nct_puterror("attribute %s not found:", name);
	print_varerror(var, "    ");
	nct_return_error(NAN);
    }
    return nct_get_floating_from(att->dtype, att->value, ind);
}

long long nct_get_varatt_integer(const nct_var *var, const char *name, int ind) {
    nct_att *att = nct_get_varatt(var, name);
    if (!att) {
	nct_puterror("attribute %s not found:\n", name);
	print_varerror(var, "    ");
	nct_return_error(0);
    }
    return nct_get_integer_from(att->dtype, att->value, ind);
}

char* nct_get_varatt_text(const nct_var* var, const char* name) {
    int natts = var->natts;
    for(int i=0; i<natts; i++)
	if (!strcmp(var->atts[i].name, name))
	    return var->atts[i].value;
    return NULL;
}

nct_var* nct_get_vardim(const nct_var* var, int num) {
    return var->super->dims[var->dimids[num]];
}

int nct_get_vardimid(const nct_var* restrict var, int dimid) {
    for(int i=0; i<var->ndims; i++)
	if (var->dimids[i] == dimid)
	    return i;
    return -1;
}

void nct_get_vardims_list(const nct_var* var, ...) {
    va_list list;
    va_start(list, var);
    for (int i=0; i<var->ndims; i++) {
	nct_var** ptr = va_arg(list, nct_var**);
	if (!ptr)
	    continue;
	*ptr = nct_get_vardim(var, i);
    }
    va_end(list);
}

void nct_get_varshape_list(const nct_var* var, ...) {
    va_list list;
    va_start(list, var);
    for (int i=0; i<var->ndims; i++) {
	long* ptr = va_arg(list, long*);
	if (!ptr)
	    continue;
	*ptr = nct_get_vardim(var, i)->len;
    }
    va_end(list);
}

int nct_get_varid(const nct_set* restrict set, const char* restrict name) {
    int nvars = set->nvars;
    for(int i=0; i<nvars; i++)
	if (!strcmp(name, set->vars[i]->name))
	    return i;
    return -1;
}

int nct_get_dimid(const nct_set* restrict set, const char* restrict name) {
    int nvars = set->ndims;
    for(int i=0; i<nvars; i++)
	if (!strcmp(name, set->dims[i]->name))
	    return i;
    return -1;
}

size_t nct_get_len_from(const nct_var* var, int start) {
    size_t len = 1;
    int ndims = var->ndims;
    for (int i=start; i<ndims; i++)
	len *= var->super->dims[var->dimids[i]]->len;
    return len;
}

long nct_get_interval_ms(enum nct_timeunit unit) {
    if (unit < 0)
	return -1;
    if (unit >= sizeof(ms_per_timeunit) / sizeof(ms_per_timeunit[0])) {
	nct_puterror("invalid time unit (%i) in %s", unit, __func__);
	nct_return_error(0);
    }
    return ms_per_timeunit[unit];
}

double nct_getg_floating(const nct_var* var, size_t ind) {
    return var->data ? nct_get_floating(var, ind) : nct_getl_floating(var, ind);
}

double nct_getl_floating(const nct_var* var, size_t ind) {
    size_t coords[var->ndims];
    nct_get_coords_from_ind(var, coords, ind);
    double result;
    nct_ncget_1_t fun = nct_getfun_1[NC_DOUBLE];
    fun(var->super->ncid, var->ncid, coords, &result);
    return result;
}

double nct_getg_integer(const nct_var* var, size_t ind) {
    return var->data ? nct_get_integer(var, ind) : nct_getl_integer(var, ind);
}

long long nct_getl_integer(const nct_var* var, size_t ind) {
    size_t coords[var->ndims];
    nct_get_coords_from_ind(var, coords, ind);
    long long result;
    nct_ncget_1_t fun = nct_getfun_1[NC_INT64];
    fun(var->super->ncid, var->ncid, coords, &result);
    return result;
}

nct_var* nct_interpolate(nct_var* var, int idim, nct_var* todim, int inplace_if_possible) {
    nct_perhaps_load_partially(var, 0, var->len);
    void* newdata = nct_get_interpolated(var, idim, todim);

    if (!inplace_if_possible || todim->super != var->super)
	var = nct_ensure_unique_name(nct_copy_var(todim->super, var, -1));
    var->data = newdata;
    var->dimids[idim] = nct_dimid(todim);

    var->len = nct_get_len_from(var, 0);
    var->startpos = 0;
    var->endpos = var->len;
    return var;
}

nct_var* nct_copy_coord_with_interval(nct_var* coord, double gap, char* new_name) {
    double v0 = nct_get_floating(coord, 0);
    double v1 = nct_get_floating_last(coord, 1);
    nct_var* new = nct_add_dim(coord->super, round(v1-v0+1), new_name);
    nct_put_interval(nct_dim2coord(new, NULL, NC_INT), v0, gap);
    for(int a=0; a<coord->natts; a++)
	nct_copy_att(new, coord->atts+a);
    return new;
}

int nct_interpret_timeunit(const nct_var* var, struct tm* timetm, int* timeunit) {
    char* units = nct_get_varatt_text(var, "units");
    *timeunit = -1; // will be overwritten on success
    if(!units) {
	nct_puterror("timevariable \"%s\" doesn't have attribute \"units\"\n", var->name);
	return 1; }
    int ui;

    /* "units" has form "hours since 2012-05-19" */

    for(ui=0; ui<nct_len_timeunits; ui++)
	if(!strncmp(units, nct_timeunits[ui], strlen(nct_timeunits[ui])))
	    break;
    if(ui == nct_len_timeunits) {
	nct_puterror("unknown timeunit \"%s\"\n", units);
	return 1; }

    units += strlen(nct_timeunits[ui]);

    if (nct__read_timestr(units, timetm))
	return 1;
    *timeunit = ui;
    return 0;
}

int nct_link_data(nct_var* dest, nct_var* src) {
    if (!src->nusers)
	src->nusers = calloc(1, sizeof(int));
    ++*src->nusers;
    dest->data = src->data;
    dest->nusers = src->nusers;
    dest->not_freeable = src->not_freeable;
    dest->rule[nct_r_start].arg.lli = src->rule[nct_r_start].arg.lli;
    return 0;
}

int* get_stream_nusers_p(const nct_var* var) {
    return var->nusers_stream;
}

void set_stream_nusers_p(nct_var* var, int* ptr) {
    var->nusers_stream = ptr;
}

int nct_link_stream(nct_var* dest, nct_var* src) {
    if (!hasrule(src, nct_r_stream))
	return -1;
    int* nusers = get_stream_nusers_p(src);
    if (!nusers) {
	nusers = calloc(1, sizeof(int));
	set_stream_nusers_p(src, nusers);
    }
    ++*nusers;
    nct_set_stream(dest, nct_get_stream(src));
    set_stream_nusers_p(dest, nusers);
    return 0;
}

static int nct_open_mem(struct nct_fileinfo_mem_t*);

/* No need to call outside perhaps_close_the_file */
static void perhaps_free_the_content(struct nct_fileinfo_mem_t* info) {
    if (!(nct_readflags & (nct_rkeepmem|nct_rkeep)) && info->owner & nct_ref_content)
	info->content = (free(info->content), NULL);
}

/* uses private nct_set.fileinfo */
static void perhaps_close_the_file(nct_var* var) {
    int *ncidp;
    if (var->fileinfo) {
	ncidp = &var->fileinfo->ncid;
	if (var->fileinfo->ismem_t)
	    perhaps_free_the_content((void*)var->fileinfo);
    }
    else {
	ncidp = nct_readflags & nct_rkeep ? NULL : &var->super->ncid;
	if (hasrule(var->super, nct_r_mem))
	    perhaps_free_the_content(var->super->fileinfo);
    }
    if (ncidp && *ncidp > 0)
	nct_close_nc(ncidp);
}

static void perhaps_close_the_file_set(nct_set* set) {
    int *ncidp;
    ncidp = nct_readflags & nct_rkeep ? NULL : &set->ncid;
    if (hasrule(set, nct_r_mem))
	perhaps_free_the_content(set->fileinfo);
    if (ncidp && *ncidp > 0)
	nct_close_nc(ncidp);
}

/* uses private nct_set.fileinfo */
static int perhaps_open_the_file(nct_var* var) {
    struct nct_fileinfo_t *info;
    if (var->fileinfo) {
	info = var->fileinfo;
	if (info->ncid <= 0) {
	    if (info->ismem_t)
		info->ncid = nct_open_mem((void*)info);
	    else
		ncfunk_open(info->name, NC_NOWRITE, &info->ncid);
	}
	return info->ncid;
    }
    else if (var->super->ncid >= 0)
	return var->super->ncid;

    if (!var->super->fileinfo)
	return -1;

    info = var->super->fileinfo;
    if (info->ismem_t)
	var->super->ncid = nct_open_mem((void*)info);
    else
	ncfunk_open(nct_get_filename(var->super), NC_NOWRITE, &var->super->ncid);
    return var->super->ncid;
}

/* No need to call outside nct_open_mem. */
static void perhaps_load_the_content(struct nct_fileinfo_mem_t* info) {
    if (info->content)
	return;
    info->content = info->getcontent(info->fileinfo.name, &info->size);
    info->owner |= nct_ref_content;
}

static int nct_open_mem(struct nct_fileinfo_mem_t* params) {
    int ncid;
    perhaps_load_the_content(params);
    if ((nct_ncret = nc_open_mem(params->fileinfo.name, NC_NOWRITE, params->size, params->content, &ncid))) {
	ncerror(nct_ncret);
	fprintf(nct_stderr? nct_stderr: stderr, "    failed to open \"\033[1m%s\033[0m\"\n", params->fileinfo.name);
	nct_other_error;
    }
    return ncid;
}

#include "load_data.h" // nct_load_as

FILE* nct_get_stream(const nct_var* var) {
    return hasrule(var, nct_r_stream) ? var->rule[nct_r_stream].arg.v : NULL;
}

/* uses private nct_set.fileinfo */
const char* nct_get_filename(const nct_set* set) {
    if (set->fileinfo)
	return ((struct nct_fileinfo_t*)set->fileinfo)->name;
    return NULL;
}

/* uses private nct_set.fileinfo */
const char* nct_get_filename_var(const nct_var* var) {
    if (!var->fileinfo)
	return nct_get_filename(var->super);
    return var->fileinfo->name;
}

/* uses private nct_set.fileinfo */
const char* nct_get_filename_capture(const nct_set* set, int igroup, int *capture_len) {
    struct nct_fileinfo_t *info = set->fileinfo;
    regmatch_t *match = info->groups + igroup;
    if (capture_len)
	*capture_len = match->rm_eo - match->rm_so;
    return info->name + info->dirnamelen + match->rm_so;
}

/* uses private nct_set.fileinfo */
const char* nct_get_filename_var_capture(const nct_var* var, int igroup, int *capture_len) {
    if (!var->fileinfo)
	return nct_get_filename_capture(var->super, igroup, capture_len);
    struct nct_fileinfo_t *info = var->fileinfo;
    regmatch_t *match = info->groups + igroup;
    if (capture_len)
	*capture_len = match->rm_eo - match->rm_so;
    return info->name + info->dirnamelen + match->rm_so;
}

double* nct_coordbounds_from_central(const nct_var *crdsrc, double *data) {
    long crdlensrc = crdsrc->len;
    double last = nct_get_floating(crdsrc, 0);
    double this = nct_get_floating(crdsrc, 1);
    data[0] = last - (this - last)/2;
    int ind = 1;
    while (1) {
	data[ind] = this - (this - last)/2;
	last = this;
	if (ind == crdlensrc-1) {
	    data[ind+1] = 2*data[ind] - data[ind-1];
	    return data;
	}
	this = (nct_get_floating(crdsrc, ++ind));
    }
}

nct_var* nct_load_stream(nct_var* var, size_t len) {
    int len0 = var->len;
    var->len = len;
    nct_allocate_varmem(var);
    var->len = len0;
    FILE* f = nct_get_stream(var);
    size_t ret = fread(var->data, nctypelen(var->dtype), len, f);
    if (ret != len)
	nct_puterror("fread %zu returned %zu in nct_load_stream\n", len, ret);
    return var;
}

/* In nct_localtime, the argument (nct_anyd)time0 should be the return value from nct_mktime0.
   The right static function (nct_localtime_$timestep) is called based on that. */
static struct tm* nct_localtime_milliseconds(long timevalue, time_t time0) {
    time0 += timevalue/1000;
    return localtime(&time0);
}
static struct tm* nct_localtime_seconds(long timevalue, time_t time0) {
    time0 += timevalue;
    return localtime(&time0);
}
static struct tm* nct_localtime_minutes(long timevalue, time_t time0) {
    time0 += timevalue*60;
    return localtime(&time0);
}
static struct tm* nct_localtime_hours(long timevalue, time_t time0) {
    time0 += timevalue*3600;
    return localtime(&time0);
}
static struct tm* nct_localtime_days(long timevalue, time_t time0) {
    time0 += timevalue*86400;
    return localtime(&time0);
}
#define TIMEUNIT(arg) [nct_##arg]=nct_localtime_##arg,

struct tm* nct_localtime(long timevalue, nct_anyd time0) {
    static struct tm*(*fun[])(long, time_t) = { TIMEUNITS }; // array of pointers to the static functions above
    return fun[time0.d](timevalue, time0.a.t);               // a call to the right function: nct_localtime_$timestep
}
#undef TIMEUNIT

nct_anyd nct_mktime(const nct_var* var, struct tm* timetm, nct_anyd* epoch, size_t ind) {
    struct tm timetm_buf;
    int d;
    if (!timetm)
	timetm = &timetm_buf;
    if (!epoch) {
	nct_anyd any = nct_mktime0(var, timetm);
	*timetm = *nct_localtime(nct_get_integer(var, ind), any);
	d = any.d;
    }
    else {
	*timetm = *nct_localtime(nct_get_integer(var, ind), *epoch);
	d = epoch->d;
    }
    return (nct_anyd){.a.t=mktime(timetm), .d=d};
}

nct_anyd nct_mktime0(const nct_var* var, struct tm* timetm) {
    struct tm tm_;
    int ui;
    if(!timetm)
	timetm = &tm_;
    if (nct_interpret_timeunit(var, timetm, &ui))
	return (nct_anyd){.d=-1};
    return (nct_anyd){.a.t=mktime(timetm), .d=ui};
}

nct_anyd nct_mktime0_nofail(const nct_var* var, struct tm* tm) {
    FILE* stderr0 = nct_stderr;
    //int act0 = nct_error_action;
    nct_stderr = fopen("/dev/null", "a");
    //nct_error_action = nct_pass;
    nct_anyd result = nct_mktime0(var, tm);
    fclose(nct_stderr);
    nct_stderr = stderr0;
    //nct_error_action = act0;
    return result;
}

/* In nct_gmtime, the argument (nct_anyd)time0 should be the return value from nct_gmtime0.
   The right static function (nct_gmtime_$timestep) is called based on that. */
static struct tm* nct_gmtime_milliseconds(long timevalue, time_t time0) {
    time0 += timevalue/1000;
    return gmtime(&time0);
}
static struct tm* nct_gmtime_seconds(long timevalue, time_t time0) {
    time0 += timevalue;
    return gmtime(&time0);
}
static struct tm* nct_gmtime_minutes(long timevalue, time_t time0) {
    time0 += timevalue*60;
    return gmtime(&time0);
}
static struct tm* nct_gmtime_hours(long timevalue, time_t time0) {
    time0 += timevalue*3600;
    return gmtime(&time0);
}
static struct tm* nct_gmtime_days(long timevalue, time_t time0) {
    time0 += timevalue*86400;
    return gmtime(&time0);
}
#define TIMEUNIT(arg) [nct_##arg]=nct_gmtime_##arg,

struct tm* nct_gmtime(long timevalue, nct_anyd time0) {
    static struct tm*(*fun[])(long, time_t) = { TIMEUNITS }; // array of pointers to the static functions above
    return fun[time0.d](timevalue, time0.a.t);               // a call to the right function: nct_gmtime_$timestep
}
#undef TIMEUNIT

nct_anyd nct_timegm(const nct_var* var, struct tm* timetm, nct_anyd* epoch, size_t ind) {
    struct tm timetm_buf;
    int d;
    if (!timetm)
	timetm = &timetm_buf;
    if (!epoch) {
	nct_anyd any = nct_timegm0(var, timetm);
	*timetm = *nct_gmtime(nct_get_integer(var, ind), any);
	d = any.d;
    }
    else {
	*timetm = *nct_gmtime(nct_get_integer(var, ind), *epoch);
	d = epoch->d;
    }
    return (nct_anyd){.a.t=timegm(timetm), .d=d};
}

nct_anyd nct_timegm0(const nct_var* var, struct tm* timetm) {
    struct tm tm_;
    int ui;
    if(!timetm)
	timetm = &tm_;
    if (nct_interpret_timeunit(var, timetm, &ui))
	return (nct_anyd){.d=-1};
    return (nct_anyd){.a.t=timegm(timetm), .d=ui};
}

nct_anyd nct_timegm0_nofail(const nct_var* var, struct tm* tm) {
    FILE* stderr0 = nct_stderr;
    //int act0 = nct_error_action;
    nct_stderr = fopen("/dev/null", "a");
    //nct_error_action = nct_pass;
    nct_anyd result = nct_timegm0(var, tm);
    fclose(nct_stderr);
    nct_stderr = stderr0;
    //nct_error_action = act0;
    return result;
}

short* __attribute__((malloc)) nct_time_to_year(const nct_var *timevar) {
    nct_anyd epoch = nct_timegm0(timevar, NULL);
    struct tm tm;
    short *years = malloc(timevar->len * sizeof(short));
    for (int i=0; i<timevar->len; i++) {
	nct_timegm(timevar, &tm, &epoch, i);
	years[i] = tm.tm_year + 1900;
    }
    return years;
}

long nct_match_starttime(nct_var* timevar0, nct_var* timevar1) {
    nct_var* vars[] = {timevar0, timevar1};
    nct_anyd time[2];

    for(int i=0; i<2; i++) {
	time[i] = nct_timegm(vars[i], NULL, NULL, 0);
	if (time[i].d < 0)
	    nct_return_error(-1);
    }

    int smaller = time[1].a.t < time[0].a.t;
    long diff_ms = (time[!smaller].a.t - time[smaller].a.t) * 1000;
    long diff_n = diff_ms  / ms_per_timeunit[time[smaller].d];
    nct_set_rstart(vars[smaller], nct_bsearch(vars[smaller], diff_n, -1));
    return diff_n;
}

long nct_match_endtime(nct_var* timevar0, nct_var* timevar1) {
    nct_var* vars[] = {timevar0, timevar1};
    nct_anyd time[2];

    for(int i=0; i<2; i++) {
	time[i] = nct_timegm(vars[i], NULL, NULL, vars[i]->len-1);
	if (time[i].d < 0)
	    nct_return_error(-1);
    }

    int smaller = time[1].a.t < time[0].a.t;
    long diff_ms = (time[!smaller].a.t - time[smaller].a.t) * 1000;
    long diff_n = diff_ms  / ms_per_timeunit[time[!smaller].d];
    nct_shorten_length(vars[!smaller], nct_bsearch(vars[!smaller], nct_get_floating_last(vars[!smaller], 1) - diff_n, 0) + 1);
    return diff_n;
}

static const int stack_size1 = 8;

int nct_stack_not_empty(const nct_var* var) {
    return !!var->stackbytes;
}

nct_var* nct_perhaps_load_partially_as(nct_var* var, long start, long end, nc_type nctype) {
    if (var->startpos > start || var->endpos < end)
	nct_load_partially_as(var, start, end, nctype);
    return var;
}

nct_var* nct_perhaps_load_partially(nct_var* var, long start, long end) {
    return nct_perhaps_load_partially_as(var, start, end, NC_NAT);
}

long long nct_pop_integer(nct_var* var) {
    long long ret;
    memcpy(&ret, var->stack + var->stackbytes - stack_size1, stack_size1);
    var->stackbytes -= stack_size1;
    return ret;
}

void nct_push_integer(nct_var* var, long long integ) {
    if (var->stackbytes + stack_size1 >= var->stackcapasit)
	var->stack = realloc(var->stack, var->stackcapasit += 8*stack_size1);
    memcpy(var->stack + var->stackbytes, &integ, stack_size1);
    var->stackbytes += stack_size1;
}

void nct_print_att(nct_att* att, const char* indent) {
    printf("%s%s%s:\t ", indent, nct_att_color, att->name);
    if (att->dtype == NC_CHAR) {
	printf("%s%s\n", (char*)att->value, nct_default_color);
	return;
    }
    int size1 = nctypelen(att->dtype);
    for(int i=0; i<att->len-1; i++) {
	nct_print_datum(att->dtype, att->value+i*size1);
	printf(", ");
    }
    if (att->len)
	nct_print_datum(att->dtype, att->value+(att->len-1)*size1);
    printf("%s\n", nct_default_color);
}

/* Global but hidden function. */
void nct_print_atts(nct_var* var, const char* indent0, const char* indent1) {
    char inde[strlen(indent0) + strlen(indent1)];
    strcpy(inde, indent0);
    strcat(inde, indent1);
    for(int i=0; i<var->natts; i++)
	nct_print_att(var->atts + i, inde);
}

void nct_print_var_meta(const nct_var* var, const char* indent) {
    printf("%s%s%s %s%s(%zu)%s:\n%s  %i dimensions: ( ",
	   indent, nct_type_color, nct_typenames[var->dtype],
	   nct_varname_color, var->name, var->len, nct_default_color,
	   indent, var->ndims);
    for(int i=0; i<var->ndims; i++) {
	nct_var* dim = var->super->dims[var->dimids[i]];
	printf("%s(%zu), ", dim->name, dim->len);
    }
    printf(")\n");
}

void nct_print_var(nct_var* var, const char* indent) {
    if (!nct_get_var(var->super, var->name))
	return nct_print_dim(var, indent);
    nct_print_var_meta(var, indent);
    printf("%s  [", indent);
    nct_print_data(var);
    puts("]");
    nct_print_atts(var, indent, "  ");
}

void nct_print_dim(nct_var* var, const char* indent) {
    printf("%s%s%s %s%s(%zu)%s:\n",
	   indent, nct_type_color, nct_typenames[var->dtype],
	   nct_dimname_color, var->name, var->len, nct_default_color);
    if(nct_iscoord(var)) {
	printf("%s  [", indent);
	nct_print_data(var);
	puts("]");
    }
    nct_print_atts(var, indent, "  ");
}

static void _nct_print(nct_set* set, int nodata) {
    const char* filename = nct_get_filename(set);
    if (filename)
	printf("%s:\n", filename);
    printf("%s%i variables, %i dimensions%s\n", nct_varset_color, set->nvars, set->ndims, nct_default_color);
    int n = set->ndims;
    for(int i=0; i<n; i++) {
	putchar('\n');
	nct_print_dim(set->dims[i], "  ");
    }
    if (nodata)
	nct_foreach(set, var) {
	    putchar('\n');
	    nct_print_var_meta(var, "  ");
	}
    else
	nct_foreach(set, var) {
	    putchar('\n');
	    nct_print_var(var, "  ");
	}
}

void nct_print(nct_set* set) {
    _nct_print(set, 0);
}

void nct_print_meta(nct_set* set) {
    _nct_print(set, 1);
}

static nct_set* nct_after_lazyread(nct_set* s, int flags) {
    unsigned old = nct_readflags;
    if (flags & nct_rlazy)
	goto end;
    nct_readflags = flags | nct_rkeep; // don't close and reopen on each variable
    if (flags & nct_rcoord) {
	int ndims = s->ndims;
	for(int i=0; i<ndims; i++)
	    if (nct_iscoord(s->dims[i]))
		nct_load(s->dims[i]);
	goto end;
    }
    for(int i=s->nvars-1; i>=0; i--)
	nct_load(s->vars[i]);

end:
    nct_readflags = old;
    perhaps_close_the_file_set(s);
    return s;
}

void* nct_read_from_nc_as(const char* filename, const char* varname, nc_type nctype) {
    if (!varname) {
	nct_readm_ncf(v, filename, nct_rlazy);
	nct_var* var = nct_firstvar(&v);
	if (!var) {
	    nct_puterror("No variables in \"%s\"\n", filename);
	    nct_free1(&v);
	    nct_return_error(NULL);
	}
	void* ret = NULL;
	if (nct_load_as(var, nctype)) {
	    ret = var->data;
	    var->not_freeable = 1;
	}
	nct_free1(&v);
	return ret;
    }
    int varid, ndims, dimids[128], dtype, ncid;
    size_t len=1, len1;
    ncfunk_open(filename, NC_NOWRITE, &ncid);
    ncfunk(nc_inq_varid, ncid, varname, &varid);
    ncfunk(nc_inq_var, ncid, varid, NULL, &dtype, &ndims, dimids, NULL);
    for(int i=0; i<ndims; i++) {
	ncfunk(nc_inq_dim, ncid, dimids[i], NULL, &len1);
	len *= len1;
    }
    if (nctype)
	dtype = nctype;
    int size1 = nctypelen(dtype);
    void *ret = malloc(len*size1);
    if (!ret) {
	nct_puterror("malloc failed");
	nct_return_error(NULL);
    }
    nct_ncget_t func = nct_getfun[dtype];
    ncfunk(func, ncid, varid, ret);
    ncfunk(nc_close, ncid);
    return ret;
}

static nct_set* nct_read_ncf_lazy(const void* filename, int flags) {
    nct_set* s = malloc(sizeof(nct_set));
    nct_read_ncf_lazy_gd(s, filename, flags) -> owner=1;
    return s;
}

#define matches(name, len, type) (len >= sizeof(type)-1  &&  !strcmp(name+len-sizeof(type)+1, type))
static nct_set* _read_unknown_format(nct_set* dest, const char* name, int flags) {
#ifdef HAVE_LZ4
    static const char lz4[] = ".lz4";
    int len = strlen(name);

    if matches(name, len, lz4)
	return nct_read_ncf_lz4_gd(dest, name, flags);
#endif
    /* Go back to the core reading function with the information that we are reading netcdf. */
    return nct_read_ncf_lazy_gd(dest, name, flags|nct_rnetcdf);
}
#undef matches

static struct nct_fileinfo_t* nct_init_fileinfo(const char *filename) {
    struct nct_fileinfo_t *fileinfo = calloc(1, sizeof(struct nct_fileinfo_t));
    fileinfo->nusers = 1;
    fileinfo->name = strdup(filename);
    return fileinfo;
}

/* Uses private nct_set.fileinfo. This is the core reading function. */
static nct_set* nct_read_ncf_lazy_gd(nct_set* dest, const void* vfile, int flags) {
    int ncid, ndims, nvars;
    struct nct_fileinfo_mem_t *fileinfo;
    if (flags & nct_rmem) {
	fileinfo = malloc(sizeof(struct nct_fileinfo_mem_t));
	memcpy(fileinfo, vfile, sizeof(struct nct_fileinfo_mem_t));
	fileinfo->fileinfo.ismem_t = 1;
	if (!(fileinfo->owner & nct_ref_name)) {
	    fileinfo->fileinfo.name = strdup(fileinfo->fileinfo.name);
	    fileinfo->owner |= nct_ref_name;
	}
	ncid = nct_open_mem(fileinfo);
    }
    else if (flags & nct_rnetcdf) {
	fileinfo = calloc(1, sizeof(struct nct_fileinfo_t));
	fileinfo->fileinfo.name = strdup(vfile);
	ncfunk_open(fileinfo->fileinfo.name, NC_NOWRITE, &ncid);
    }
    else // If another filetype isn't recognized, calls this function again with flags|nct_rnetcdf.
	return _read_unknown_format(dest, vfile, flags);
    fileinfo->fileinfo.nusers = 1;
    ncfunk(nc_inq_ndims, ncid, &ndims);
    ncfunk(nc_inq_nvars, ncid, &nvars);
    *dest = (nct_set){
	.ncid = ncid,
	.ndims = ndims,
	.nvars = nvars,
	.dimcapacity = ndims + 1,
	.varcapacity = nvars + 3,
	.fileinfo = fileinfo,
    };

    if (flags & nct_rmem)
	setrule(dest, nct_r_mem);

    dest->dims = calloc(dest->dimcapacity, sizeof(void*));
    dest->vars = calloc(dest->varcapacity, sizeof(void*));
    for(int i=0; i<nvars; i++)
	_nct_read_var_info(dest, i, flags);
    for(int i=0; i<ndims; i++)
	/* If a variable is found with the same name,
	   this will make a pointer to that instead of creating a new object.
	   Id will then become nct_coordid(var_id). */
	_nct_read_dim(dest, i);
    /* Lengths could not be set before reading dimensions. */
    nct_foreach(dest, var) {
	size_t len = 1;
	int ndims = var->ndims;
	int too_many;
	if ((too_many = ndims>nct_maxdims))
	    ndims = nct_maxdims;
	for(int i=0; i<ndims; i++) {
	    size_t len1 = nct_get_vardim(var, i)->len;
	    var->filedimensions[i] = len1;
	    len *= len1;
	}
	for(int i=ndims; i<nct_maxdims; i++)
	    var->filedimensions[i] = 1;
	var->len = len;
	if (too_many)
	    var->len = nct_get_len_from(var, 0);
    }
    return dest;
}

nct_set* nct_read_ncf(const void* vfile, int flags) {
    nct_set* s = nct_read_ncf_lazy(vfile, flags);
    return nct_after_lazyread(s, flags);
}

nct_set* nct_read_ncf_gd(nct_set* s, const void* vfile, int flags) {
    nct_read_ncf_lazy_gd(s, vfile, flags);
    return nct_after_lazyread(s, flags);
}

/* Reading multiple files as if they were one file.
   If data shouldn't be loaded according to readflags,
   these create null-terminated list nct_set* and return pointer to the first member.
   */

nct_set* nct_read_mfnc_ptr(const char* filenames, int nfiles, char* concat_args) {
    return nct_read_mfncf_ptr(filenames, nct_readflags, nfiles, concat_args);
}

/* filenames has the form of "name1\0name2\0name3\0last_name\0\0" */
/* This is the core multifile function. */
nct_set* nct_read_mfncf_ptr1(const char* filenames, int readflags, int nfiles, char* concat_args,
	regmatch_t **groups, int dirnamelen) {
    const char* ptr = filenames;
    /* nfiles is just a hint. If necessary, it can be calculated. */
    if (nfiles<0) {
	nfiles = 0;
	while (*ptr) {
	    nfiles++;
	    ptr += strlen(ptr)+1;
	}
	if (!nfiles) {
	    nct_puterror("empty filename to read\n");
	    nct_return_error(NULL); // error if user told to count files and result == 0
	}
    }
    else if (!nfiles)
	return NULL; // no error if user wanted to read 0 files
    int nfiles0 = nfiles;
    ptr = filenames;

    nct_set *set, *setptr;
    if (!(readflags & nct_rnoall))
	goto read_and_load;

    /* No loading data. Making list of nct_set* and setting right loading rules. */
    set = setptr = calloc(nfiles+1, sizeof(nct_set)); // +1 for NULL-termination
    nct_read_ncf_gd(set, ptr, readflags);
    set->owner = 1;
    if (groups)
	((struct nct_fileinfo_t*)set->fileinfo)->groups = groups[0];
    ((struct nct_fileinfo_t*)set->fileinfo)->dirnamelen = dirnamelen;
    int ind = 0;
    nfiles--;
    setrule(set, nct_r_list); // so that nct_free knows to free other members as well
    ptr += strlen(ptr)+1;
    if (!(readflags & nct_rcoordall)) {
	readflags &= ~(nct_rcoord|nct_rcoordall);
	readflags |= nct_rlazy;
    }

    nct_set original_set = {0};
    if (readflags & nct_requalfiles) {
	nct_copy(&original_set, set, -1);
	for (int i=0; i<original_set.nvars; i++) {
	    _nct_unlink_fileinfo(original_set.vars[i]->fileinfo);
	    original_set.vars[i]->fileinfo = NULL;
	}
    }

    /* The first set was already read above. */
    /* Don't loop as while (*ptr) to allow reading fewer files than available. */
    while (nfiles) {
	if (readflags & nct_requalfiles) {
	    nct_copy(++setptr, &original_set, -1);
	    setptr->fileinfo = nct_init_fileinfo(ptr);
	}
	else
	    nct_read_ncf_gd(++setptr, ptr, readflags);
	if (groups)
	    ((struct nct_fileinfo_t*)setptr->fileinfo)->groups = groups[++ind];
	((struct nct_fileinfo_t*)setptr->fileinfo)->dirnamelen = dirnamelen;
	nct_concat(set, setptr, concat_args, --nfiles);
	ptr += strlen(ptr)+1;
	if (nct_verbose) {
	    printf("concatenating %i / %i", nfiles0 - nfiles, nfiles0);
	    nct_verbose_line_ending();
	}
    }
    nct_free1(&original_set);
    return set;

read_and_load:
    set = malloc(sizeof(nct_set));
    nct_read_nc_gd(set, ptr);
    set->owner = 1;
    nfiles--;
    ptr += strlen(ptr)+1;
    if (groups)
	((struct nct_fileinfo_t*)set->fileinfo)->groups = groups[0];
    ((struct nct_fileinfo_t*)set->fileinfo)->dirnamelen = dirnamelen;
    ind = 0;
    while (nfiles) {
	nct_readm_ncf(set1, ptr, readflags);
	if (groups)
	    ((struct nct_fileinfo_t*)set1.fileinfo)->groups = groups[++ind];
	((struct nct_fileinfo_t*)set1.fileinfo)->dirnamelen = dirnamelen;
	nct_concat(set, &set1, concat_args, --nfiles);
	nct_free1(&set1); // TODO: read files straight to vs0 to avoid unnecessarily allocating and freeing memory
	ptr += strlen(ptr)+1;
	if (nct_verbose) {
	    printf("loading and concatenating %i / %i", nfiles0 - nfiles, nfiles0);
	    nct_verbose_line_ending();
	}
    }
    return set;
}

nct_set* nct_read_mfncf_ptr(const char* filenames, int readflags, int nfiles, char* concat_args) {
    return nct_read_mfncf_ptr1(filenames, readflags, nfiles, concat_args, NULL, 0);
}


nct_set* nct_read_mfnc_ptrptr(char** filenames, int nfiles, char* concat_args) {
    return nct_read_mfncf_ptrptr(filenames, nct_readflags, nfiles, concat_args);
}

nct_set* nct_read_mfncf_ptrptr(char** filenames, int readflags, int nfiles, char* concat_args) {
    int len = 0;
    int n = 0;
    while (n!=nfiles && filenames[n]) {
	len += strlen(filenames[n]) + 1;
	n++;
    }
    char* files = malloc(len+1);
    char* ptr = files;
    for(int i=0; i<n; i++) {
	int a = strlen(filenames[i]) + 1;
	memcpy(ptr, filenames[i], a);
	ptr += a;
    }
    *ptr = 0;
    nct_set* set = nct_read_mfncf_ptr(files, readflags, n, concat_args);
    free(files);
    return set;
}

nct_set* nct_read_mfnc_regex(const char* filename, int regex_cflags, char* concat_args) {
    struct nct_mf_regex_args args = {
	.regex = filename,
	.regex_cflags = regex_cflags,
	.concat_args = concat_args,
	.nct_readflags = nct_readflags,
    };
    return nct_read_mfnc_regex_args(&args);
}

/* The core read regex function. */
nct_set* nct_read_mfnc_regex_args(struct nct_mf_regex_args *args) {
    char *names = nct__get_filenames_args(args);
    int num = nct__getn_filenames(); // returns the number of files read on previous call

    if (args->max_nfiles && num > args->max_nfiles) {
	/* If some files are omitted, their capture groups will not be freed in _nct_unlink_fileinfo. */
	if (args->groups_out)
	    for (int i=args->max_nfiles; i<num; i++)
		free(args->groups_out[i]);
	num = args->max_nfiles;
    }

    if (nct_verbose) {
	char* str = names;
	printf("match from %s:", args->regex);
	nct_verbose_line_ending();
	while (*str) {
	    printf("%s", str);
	    nct_verbose_line_ending();
	    str += strlen(str) + 1;
	}
    }
    if (num == 0) {
	free(names);
	nct_puterror("No files match \"%s\"\n", args->regex);
	nct_return_error(NULL);
    }

    nct_set* s = nct_read_mfncf_ptr1(names, args->nct_readflags, num, args->concat_args, args->groups_out, args->dirnamelen_out);

    if (!args->return_groups) {
	free(args->groups_out); // Frees ptrptr. All pointers are in struct fileinfos.
	args->groups_out = NULL;
    }

    free(names);
    return s;
}

nct_var* nct_rename(nct_var* var, char* name, int freeable) {
    if (var->freeable_name)
	free(var->name);
    var->name = name;
    var->freeable_name = freeable;
    return var;
}

nct_var* nct_rewind(nct_var* var) {
    if (var->data)
	var->data -= var->rule[nct_r_start].arg.lli*nctypelen(var->dtype);
    return var;
}

void nct_rm_dim(nct_var* var) {
    if (nct_iscoord(var))
	nct_coord2dim(var);
    _nct_drop_dim(var);
    _nct_free_var(var);
    free(var);
}

int nct_rm_unused_dims(nct_set *set) {
    nct_var *delete[set->ndims];
    int ndelete = 0;
    for (int idim=0; idim<set->ndims; idim++) {
	nct_foreach(set, var)
	    if (nct_get_vardimid(var, idim) >= 0)
		goto next;
	delete[ndelete++] = set->dims[idim];
next:;
    }
    for (int i=0; i<ndelete; i++)
	nct_rm_dim(delete[i]);
    return ndelete;
}

void nct_rm_varatt_num(nct_var* var, int num) {
    nct_att* ptr = var->atts + num;
    memmove(ptr, ptr+1, (var->natts-(num+1)) * sizeof(nct_att));
    var->natts--;
    /* allocated memory for var->atts is not shortened */
}

void nct_rm_varatt_name(nct_var* var, const char* attname) {
    if (nct_get_varatt(var, attname))
	nct_rm_varatt_num(var, nct_register);
}

void nct_rm_var(nct_var* var) {
    if (nct_iscoord(var)) {
	nct_coord2dim(var);
	return;
    }
    _nct_drop_var(var);
    _nct_free_var(var);
    free(var);
}

/* This uses static functions from load_data.h */
nct_var* nct_update_concatlist(nct_var* var0) {
    if (!hasrule(var0, nct_r_concat))
	return var0;
    if (var0->len == 0) {
	var0->rule[nct_r_concat].n = 0;
	return var0;
    }

    int fileno;
    size_t start_trunc_new;
    if (get_filenum(var0->len, var0, &fileno, &start_trunc_new))
	return var0;
    var0->rule[nct_r_concat].n = fileno - !start_trunc_new; // truncate the list
    if (!start_trunc_new)
	return var0;

    nct_var* var1 = fileno ? from_concatlist(var0, fileno-1) : var0;
    nct_set_length(var1, start_trunc_new);
    nct_update_concatlist(var1);
    return var0;
}

nct_var* nct_set_concat(nct_var* var0, nct_var* var1, int howmany_left) {
    setrule(var0, nct_r_concat);
    nct_rule* r = var0->rule + nct_r_concat;
    int size = r->n+1 + howmany_left; // how many var pointers will be put into the concatenation list
    if(r->capacity < size) {
	r->arg.v = realloc(r->arg.v, size*sizeof(void*));
	r->capacity = size;
    }
    ((nct_var**)r->arg.v)[r->n++] = var1;
    var0->len += var1->len;
    if (!var1->fileinfo)
	var1->fileinfo = _nct_link_fileinfo(var1->super->fileinfo);
    return var0;
}

nct_var* nct_set_timeend_str(nct_var *dim, const char *timestr, int beforeafter) {
    size_t ind = nct_bsearch_time_str(dim, timestr, beforeafter);
    return nct_set_length(dim, ind);
}

nct_var* nct_set_length(nct_var* dim, size_t arg) {
    dim->len = arg;
    setrule(dim->super, nct_r_start); // start is used to mark length as well
    nct_foreach(dim->super, var)
	var->len = nct_get_len_from(var, 0);
    return dim;
}

nct_var* nct_shorten_length(nct_var* dim, size_t arg) {
    if (dim->len < arg)
	return NULL;
    return nct_set_length(dim, arg);
}

nct_var* nct_iterate_concatlist(nct_var* var) {
    static int n_concat, iconcat;
    static nct_var* svar;
    if (var) {
	svar = var;
	n_concat = !!hasrule(var, nct_r_concat) * var->rule[nct_r_concat].n;
	iconcat = 0;
	return var;
    }
    if (iconcat++ >= n_concat)
	return NULL;
    return from_concatlist(svar, iconcat-1);
}

nct_var* nct_set_rstart(nct_var* dim, long arg) {
    long new = arg + dim->rule[nct_r_start].arg.lli;
    return nct_set_start(dim, new);
}

nct_var* nct_set_start(nct_var* dim, size_t arg) {
    long change = arg - dim->rule[nct_r_start].arg.lli; // new_start - old_start
    dim->rule[nct_r_start].arg.lli = arg;
    dim->len -= change;
    if (dim->data)
	dim->data += change*nctypelen(dim->dtype);
    setrule(dim->super, nct_r_start);
    nct_foreach(dim->super, var)
	var->len = nct_get_len_from(var, 0);
    return dim;
}

nct_var* nct_set_timestart_str(nct_var *dim, const char *timestr, int beforeafter) {
    size_t ind = nct_bsearch_time_str(dim, timestr, beforeafter);
    return nct_set_rstart(dim, ind);
}

void nct_set_stream(nct_var* var, FILE* f) {
    setrule(var, nct_r_stream);
    nct_rule* r = var->rule+nct_r_stream;
    r->arg.v = f;
}

void nct_unlink_data(nct_var* var) {
    if (!cannot_free(var))
	free(nct_rewind(var)->data);
    if (var->nusers) {
	if (!*var->nusers)
	    free(var->nusers);
	else
	    --*var->nusers;
    }
    var->data = var->nusers = NULL;
    var->capacity = 0;
    var->startpos = var->endpos = 0;
}

void nct_unlink_stream(nct_var* var) {
    if (!hasrule(var, nct_r_stream))
	return;
    int* nusers = get_stream_nusers_p(var);
    if (!nusers || !*nusers) {
	fclose(nct_get_stream(var));
	free(nusers);
    }
    else
	--*nusers;
    rmrule(var, nct_r_stream);
}

enum {_createcoords=1<<0, _defonly=1<<1, _mutable=1<<2};
static int _nct_create_nc(const nct_set* src, const char* name, unsigned what) {
    int ncid, id;
    startpass;

    ncfunk(nc_create, name, NC_NETCDF4|NC_CLOBBER, &ncid);
    ncfunk(nc_set_fill, ncid, NC_NOFILL, NULL);
    int n = src->ndims;
    for (int i=0; i<n; i++)
	ncfunk(nc_def_dim, ncid, src->dims[i]->name, src->dims[i]->len, &id);
    n = src->nvars;
    for (int i=0; i<n; i++) {
	nct_var* v = src->vars[i];
	if (what & _createcoords && !nct_iscoord(v))
	    continue;
	int load = 0;
	if (!v->data) {
	    if (nct_loadable(v))
		load = 1;
	    else if (!(what & _defonly))
		continue;
	}
	ncfunk(nc_def_var, ncid, v->name, v->dtype, v->ndims, v->dimids, &id);
	if (what & _mutable)
	    v->ncid = id;
	for (int a=0; a<v->natts; a++)
	    if (v->atts[a].dtype == NC_CHAR)
		ncfunk(nc_put_att_text, ncid, i, v->atts[a].name,
			v->atts[a].len, v->atts[a].value);
	    else
		ncfunk(nc_put_att, ncid, i, v->atts[a].name, v->atts[a].dtype,
			v->atts[a].len, v->atts[a].value);
	if (what & _defonly)
	    continue;
	if (load) nct_load(v);
	ncfunk(nc_put_var, ncid, id, v->data);
	if (load) nct_unlink_data(v);
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
    if (what & _mutable)
	/* an unsafe hack to discard the const qualifier */
	memcpy(&src->ncid, &ncid, sizeof(src->ncid));
#pragma GCC diagnostic pop
    endpass;
    return ncid;
}
int nct_create_nc(const nct_set* src, const char* name)		{ return _nct_create_nc(src, name, 0); }
int nct_create_nc_mut(nct_set* src, const char* name)		{ return _nct_create_nc(src, name, _mutable); }
int nct_create_nc_def(nct_set* src, const char* name)		{ return _nct_create_nc(src, name, _mutable|_defonly); }
int nct_createcoords_nc(const nct_set* src, const char* name)	{ return _nct_create_nc(src, name, _createcoords); }
int nct_createcoords_nc_mut(nct_set* src, const char* name)	{ return _nct_create_nc(src, name, _createcoords|_mutable); }
int nct_createcoords_nc_def(nct_set* src, const char* name)	{ return _nct_create_nc(src, name, _createcoords|_mutable|_defonly); }

const nct_set* nct_write_nc(const nct_set* src, const char* name) {
    startpass;
    ncfunk(nc_close, nct_create_nc(src, name));
    endpass;
    return src;
}

nct_set* nct_write_mut_nc(nct_set* src, const char* name) {
    startpass;
    ncfunk(nc_close, nct_create_nc(src, name));
    endpass;
    return src;
}

char* nct__get_filenames(const char* restrict regex, int flags) {
    struct nct_mf_regex_args args = {.regex=regex, .regex_cflags=flags};
    return nct__get_filenames_args(&args);
}

char* nct__get_filenames_cmpfun(const char* restrict regex, int flags, void *strcmpfun_for_sorting) {
    struct nct_mf_regex_args args = {.regex = regex, .regex_cflags = flags, .strcmpfun_for_sorting = strcmpfun_for_sorting};
    return nct__get_filenames_args(&args);
}

char* nct__get_filenames_deprecated(const char* restrict regex, int regex_cflags, void *strcmpfun_for_sorting,
	void (*fun)(const char* restrict, int, regmatch_t* pmatch, void*), int size1dest, int nmatch, void** dest) {
    struct nct_mf_regex_args args = {
	.regex = regex,
	.regex_cflags = regex_cflags,
	.strcmpfun_for_sorting = strcmpfun_for_sorting,
	/*.fun = fun,
	.size1dest = size1dest,
	.dest = dest,
	.nmatch = nmatch,*/
	.groups_out = NULL,
    };
    return nct__get_filenames_args(&args);
}

char *nct__get_filenames_args(struct nct_mf_regex_args* argsp) {
    static int num;
    if (!argsp)
	return (char*)(intptr_t)num;
    struct nct_mf_regex_args args = *argsp; // for convenience
    /* find the name of the directory */
    int i, ind = 0;
    DIR* dp;
    struct dirent *entry;
    regex_t reg;
    char* dirname = NULL;
    int smatch = 2048, lmatch=0; // space-of-match, length-of-match
    char* match = malloc(smatch);
    /*if (args.fun)
	*args.dest = malloc(smatch*args.size1dest);*/
    for (i=0; args.regex[i]; i++)
	if (args.regex[i] == '/')
	    ind = i;
    /* open the directory */
    int p2 = strlen(args.regex) - ind;
    char str[(p2>ind? p2: ind) + 2];
    if (ind) {
	strncpy(str, args.regex, ind);
	str[ind] = '\0';
    }
    else strcpy(str, ".");
    dirname = strdup(str);
    dp = opendir(dirname);
    if (!dp) {
	nct_puterror("could not open directory \"%s\": %s", dirname, strerror(errno));
	nct_return_error(NULL);
    }
    int dlen = strlen(dirname);
    /* find the matching files */
    if (chdir(dirname)) {
	nct_puterror("chdir(dirname) in nct__get_filenames: %s", strerror(errno));
	nct_return_error(NULL);
    }
    strcpy(str, args.regex+ind+1);
    i = regcomp(&reg, str, args.regex_cflags);
    if (i) {
	char er[700];
	regerror(i, &reg, er, 700);
	nct_puterror("regcomp error:\n    %s\n", er);
	nct_return_error(NULL);
    }

    num = 0;
    int room_groups = 0;
    regmatch_t **saved_groups = args.nmatch ? malloc((room_groups=1024) * sizeof(void*)) : NULL;
    /*regmatch_t regbuff[args.nmatch];
    regmatch_t *group_ptr = regbuff;*/
    regmatch_t* groups_next_ptr = malloc(args.nmatch * sizeof(regmatch_t));

    while ((entry = readdir(dp))) {
	if (regexec(&reg, entry->d_name, args.nmatch, groups_next_ptr, 0))
	    continue;
	if (args.nmatch) {
	    if (num >= room_groups)
		saved_groups = realloc(saved_groups, (room_groups += 1024) * sizeof(void*));
	    saved_groups[num] = groups_next_ptr;
	    groups_next_ptr = malloc(args.nmatch * sizeof(regmatch_t));
	}
	int len = dlen + 1 + strlen(entry->d_name) + 1;
	if (lmatch+len+1 > smatch) {
	    smatch = lmatch + len + 1024;
	    match = realloc(match, smatch);
	    /*if (args.fun)
		*args.dest = realloc(*args.dest, smatch*args.size1dest);*/
	}
	sprintf(match+lmatch, "%s/%s", dirname, entry->d_name);
	/*if (args.fun)
	    args.fun(entry->d_name, num, group_ptr, *args.dest);*/
	lmatch += len;
	num++;
    }
    free(groups_next_ptr); groups_next_ptr = NULL;
    closedir(dp);
    match[lmatch] = '\0'; // end with two null bytes;

    if (chdir(getenv("PWD"))) {
	nct_puterror("chdir in nct__get_filenames: %s", strerror(errno));
	nct_return_error(NULL);
    }

    char* sorted = malloc(lmatch+1);
    /*void* sorted_dest = args.fun ? malloc(lmatch*args.size1dest) : NULL;
    void* destarrbuff[] = {sorted_dest, args.dest ? *args.dest : NULL};
    void* destarr = args.fun ? destarrbuff : NULL;*/

    regmatch_t **saved_groups_sorted = args.nmatch ? malloc(num * sizeof(void*)) : NULL;
    void** ptrsbuff[] = {(void**)saved_groups_sorted, (void**)saved_groups};
    void*** ptrs = saved_groups ? ptrsbuff : NULL;
    nct__sort_str(sorted, match, num, NULL, 0, ptrs, args.strcmpfun_for_sorting? args.strcmpfun_for_sorting: strcmp);

    /*if (args.fun) {
	free(*args.dest);
	*args.dest = sorted_dest;
    }*/

    if (args.nmatch) {
	argsp->groups_out = saved_groups_sorted;
	free(saved_groups); // free ptrptr, all single ptrs were copied to sorted ptrptr
    }

    argsp->dirnamelen_out = dlen+1;

    free(match);
    free(dirname);
    regfree(&reg);
    return sorted;
}

int nct__read_timestr(const char *timestr, struct tm* timetm) {
    int year, month, day, hms[3]={0}, len;
    const char *str = timestr;
    for(; *str; str++)
	if('0' <= *str && *str <= '9') {
	    if(sscanf(str, "%d-%d-%d%n", &year, &month, &day, &len) != 3) {
		nct_puterror("Could not read timestring (%s)", timestr);
		return 1; }
	    break;
	}
    str += len;

    /* Optionally read hours, minutes, seconds. */
    for(int i=0; i<3 && *str; i++) {
	if(sscanf(str, "%2d", hms+i) != 1)
	    break;
	str += 2;
	while(*str && !('0' <= *str && *str <= '9')) str++;
    }
    *timetm = (struct tm){.tm_year=year-1900, .tm_mon=month-1, .tm_mday=day,
			  .tm_hour=hms[0], .tm_min=hms[1], .tm_sec=hms[2]};
    return 0;
}

static int isnumber(char a) {
    return '0' <= a && a <= '9';
}

static int digits_in_number(long a) {
    int n = 1;
    while (a > 9) {
	a /= 10;
	n++;
    }
    return n;
}

int nct__strcmp_numeric(const char *restrict a, const char *restrict b) {
    while (*a && *b) {
	if (isnumber(*a) && isnumber(*b)) {
	    int ia = atoi(a);
	    int ib = atoi(b);
	    if (ia != ib)
		return ia - ib;
	    a += digits_in_number(ia);
	    b += digits_in_number(ib);
	    continue;
	}
	if (*a != *b)
	    return (int)*a - (int)*b;
	a++;
	b++;
    }
    if (*a) return -1;
    if (*b) return 1;
    return 0;
}

/* Selection sort that does not change src.
 * Other is an optional array which is sorted like src. other[0] is dest and other[1] is src. */
char* nct__sort_str(char* dst, const char* restrict src, int n, void* other[2], int size1other, void** ptrs[2],
    int (*strcmpfun)(const char*, const char*)) {
    const char *sptr, *strptr;
    char *dptr = dst;
    if (n <= 0) {
	n = 0;
	nct__forstr(src, s) n++;
    }
    char used[n];
    memset(used, 0, n);
    int ind, indstr, breakflag, n_sorted=0;
    /* Each loop finds one element which is inserted at the start of the unsorted space. */
    while(1) {
	sptr = src;
	strptr = NULL;
	ind = indstr = 0;
	breakflag = 1;
	while(*sptr) {
	    if (!used[ind]) {
		breakflag = 0;
		if (!strptr || strcmpfun(sptr, strptr) < 0) {
		    strptr = sptr;
		    indstr = ind;
		}
	    }
	    sptr += strlen(sptr)+1;
	    ind++;
	}
	if (breakflag) // all members were used
	    goto out;
	strcpy(dptr, strptr);
	used[indstr] = 1;
	dptr += strlen(dptr)+1;
	if (other)
	    memcpy(other[0]+n_sorted*size1other, other[1]+indstr*size1other, size1other);
	if (ptrs)
	    ptrs[0][n_sorted] = ptrs[1][indstr];
	n_sorted++;
    }
out:
    *dptr = '\0';
    return dst;
}
