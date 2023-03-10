#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include "nctietue3.h"
/* for multifile read using regex */
#include <regex.h>
#include <dirent.h>
#include <unistd.h>

#define other_error switch(nct_error_action) {	\
    case nct_auto:				\
    case nct_interrupt: asm("int $3"); break;	\
    default:;					\
}

#define return_error(val) switch(nct_error_action) {	\
    case nct_auto:					\
    case nct_interrupt: asm("int $3"); /* no break */	\
    default: return val;				\
}

#define nct_puterror(...) do {	fprintf(nct_stderr? nct_stderr: stderr, "%sError%s (%s: %i):\n", nct_error_color, nct_default_color, __FILE__, __LINE__);	\
    				fprintf(nct_stderr? nct_stderr: stderr, "    " __VA_ARGS__); } while(0)

#define ncerror(arg) fprintf(nct_stderr? nct_stderr: stderr, "%sNetcdf-error%s (%s: %i):\n    %s\n",	\
			     nct_error_color, nct_default_color, __FILE__, __LINE__, nc_strerror(arg))

#define ncfunk(fun, ...)			\
    do {					\
	if((nct_ncret = fun(__VA_ARGS__))) {	\
	    ncerror(nct_ncret);			\
	    other_error;			\
	}					\
    } while(0)

#define startpass			\
    int __nct_err = nct_error_action;	\
    if (!(nct_error_action = nct_auto))	\
	nct_error_action = nct_pass

#define endpass nct_error_action = __nct_err

#define print_varerror(var, indent)  do {	\
    FILE* tmp = stdout;				\
    stdout = nct_stderr? nct_stderr: stderr;	\
    nct_print_var(var, indent);			\
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
enum {nct_milliseconds, nct_seconds, nct_minutes, nct_hours, nct_days, nct_len_timeunits};
#define TIMEUNITS TIMEUNIT(milliseconds) TIMEUNIT(seconds) TIMEUNIT(minutes) TIMEUNIT(hours) TIMEUNIT(days)

static int timecoeff[] = {0, 1, 60, 3600, 84600};

#define ONE_TYPE(nctype,b,ctype) [nctype] = #ctype,
static const char* const nct_typenames[] = { ALL_TYPES };
#undef ONE_TYPE

#define TIMEUNIT(arg) [nct_##arg] = #arg,
static const char* const nct_timeunits[] = { TIMEUNITS };
#undef TIMEUNIT

int nct_nlinked_vars=0;
#define nct_nlinked_max 256
unsigned char nct_nusers[nct_nlinked_max] = {0};

int nct_readflags, nct_ncret, nct_error_action;
FILE* nct_stderr;

const char* nct_error_color   = "\033[1;91m";
const char* nct_varset_color  = "\033[1;35m";
const char* nct_varname_color = "\033[92m";
const char* nct_dimname_color = "\033[44;92m";
const char* nct_type_color    = "\033[93m";
const char* nct_default_color = "\033[0m";

static void     nct_free_var(nct_var* var);
static nct_set* nct_read_ncf_lazy_gd(nct_set* dest, const char* filename, int flags);
static nct_set* nct_read_ncf_lazy(const char* filename, int flags);
static nct_set* nct_after_lazyread(nct_set* s, int flags);

void* nct_getfun[] = {
    [NC_NAT]    = nc_get_var,
    [NC_BYTE]   = nc_get_var_schar,
    [NC_CHAR]   = nc_get_var_schar,
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
    [NC_CHAR]   = nc_get_vara_schar,
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

#include "internals.h"

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
	.super = set,
	.id    = id,
	.ncid  = -1,
	.name  = name,
	.len   = len,
    };
    return set->dims[id];
failed:
    nct_puterror("realloc failed in nct_add_dim: %s\n", strerror(errno));
    return_error(NULL);
}

nct_var* nct_add_var(nct_set* set, void* src, nc_type dtype, char* name,
		     int ndims, int* dimids) {
    if (set->varcapacity < set->nvars+1)
	if (!(set->vars = realloc(set->vars, (set->varcapacity=set->nvars+3)*sizeof(void*))))
	    goto failed;
    nct_var* var = set->vars[set->nvars] = malloc(sizeof(nct_var));
    *var = (nct_var) {
	.super       = set,
	.id          = set->nvars,
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
    return var;
failed:
    startpass;
    nct_puterror("(re/m)alloc failed in nct_add_var: %s\n", strerror(errno));
    return_error(NULL);
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

void nct_add_varatt_text(nct_var* var, char* name, char* value, unsigned freeable) {
    void* vp;
    if(var->attcapacity < var->natts+1) {
	if(!(vp=realloc(var->atts, (var->attcapacity=var->natts+3)*sizeof(nct_att))))
	    goto failed;
	var->atts = vp;
    }
    var->atts[var->natts++] = (nct_att){ .name     = name,
					 .value    = value,
					 .dtype    = NC_CHAR,
					 .len      = value? strlen(value)+1: 0,
					 .freeable = freeable };
    return;
failed:
    startpass;
    var->attcapacity = var->natts;
    nct_puterror("realloc failed in nct_add_varatt_text.\n");
    print_varerror(var, "    ");
    other_error;
    endpass;
}

nct_var* nct_add_vardim_first(nct_var* var, int dimid) {
    if(var->dimcapacity < var->ndims+1) {
	var->dimcapacity = var->ndims+2;
	var->dimids = realloc(var->dimids, var->dimcapacity*sizeof(int));
    }
    memmove(var->dimids+1, var->dimids, var->ndims*sizeof(int));
    var->dimids[0] = dimid;
    var->ndims++;
    return var;
}

/* Concatenation is not yet supported along other existing dimensions than the first one. */
nct_set* nct_concat(nct_set *vs0, nct_set *vs1, char* dimname, int howmany_left) {
    int dimid0, dimid1, varid0=0, varid1;
    if(!dimname)
	dimname = "-0";
    if(dimname[0] == '-') {
	/* A number tells which vardim to concatenate along. Defined based on dimensions of the first var. */
	if(sscanf(dimname+1, "%i", &dimid0) == 1) {
	    nct_var* tmpvar = nct_firstvar(vs0);
	    if(dimid0 < tmpvar->ndims)
		dimname = vs0->dims[tmpvar->dimids[dimid0]]->name;
	}
	/* Not a concatenation but can be useful.
	 * Plus this check is almost never made when user doesn't knowingly call this feature. */
	else if(!strcmp(dimname, "-v")) {
	    varid1=-1;
	    nct_foreach(vs1, var1) {
		nct_var* var = nct_copy_var(vs0, var1, 1);
		nct_ensure_unique_name(var);
	    }
	    return vs0;
	}
    }
    /* Now dimname is either existing dimension or one to be created. */
    dimid0 = nct_get_dimid(vs0, dimname);
    if(dimid0 < 0)
	dimid0 = nct_add_dim(vs0, 1, dimname)->id;
    /* The dimension exists now in vs0 but not necessarily in vs1. */
    dimid1 = nct_get_dimid(vs1, dimname);
    if(dimid1 < 0)
	vs0->dims[dimid0]->len++;
    else {
	vs0->dims[dimid0]->len += vs1->dims[dimid1]->len;
	/* If the dimension is also a variable, concat that */
	if((varid0=nct_get_varid(vs0, dimname)) >= 0 &&
	   (varid1=nct_get_varid(vs1, dimname)) >= 0)
	{
	    nct_att* att = nct_get_varatt(vs0->vars[varid0], "units"); // convert timeunits if the dimension is time
	    if(att)
		nct_convert_timeunits(vs1->vars[varid1], att->value);
	    _nct_concat_var(vs0->vars[varid0], vs1->vars[varid1], dimid0, howmany_left);
	}
    }
    /* Finally concatenate all variables */
    nct_foreach(vs0, var0) {
	nct_var* var1;
	if((var1=nct_get_var(vs1, var0->name)))
	    _nct_concat_var(var0, var1, dimid0, howmany_left);
    }
    return vs0;
}

nct_var* nct_convert_timeunits(nct_var* var, const char* units) {
    nct_att* att = nct_get_varatt(var, "units");
    if(!att)
	return NULL;
    if(!strcmp(att->value, units))
	return var; // already correct
    time_t sec0, sec1;
    nct_anyd time0_anyd = nct_mktime0(var, NULL);
    if(time0_anyd.d < 0)
	return NULL;
    sec0 = mktime(nct_localtime(1, time0_anyd)) - mktime(nct_localtime(0, time0_anyd)); // days -> 86400 etc.
    if(att->freeable & 1)
	free(att->value);

    /* change the attribute */
    att->value = strdup(units);
    att->freeable |= 1;
    nct_anyd time1_anyd = nct_mktime0(var, NULL); // different result than time0_anyd, since att has been changed
    if(time1_anyd.d < 0)
	return NULL;

    if (!var->data) {
	if nct_loadable(var)
	    nct_load(var);
	else {
	    if (!var->dtype)
		var->dtype = time1_anyd.d <= nct_seconds ? NC_INT64 : NC_INT;
	    nct_put_interval(var, 0, 1);
	}
    }

    sec1 = mktime(nct_localtime(1, time1_anyd)) - mktime(nct_localtime(0, time1_anyd)); // days -> 86400 etc.
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

nct_var* nct_copy_var(nct_set* dest, nct_var* src, int link) {
    nct_var* var;
    if (nct_iscoord(src)) {
	int dimid = nct_get_dimid(dest, src->name);
	if (dimid < 0) {
	    var = nct_add_dim(dest, src->len, strdup(src->name));
	    var->freeable_name = 1;
	}
	else
	    var = dest->dims[dimid];
	var = nct_dim2coord(var, NULL, src->dtype);
    }
    else {
	int n = src->ndims;
	int dimids[n];
	for(int i=0; i<n; i++) {
	    nct_var* vardim = src->super->dims[src->dimids[i]];
	    dimids[i] = nct_get_dimid(dest, vardim->name);
	    /* Create the dimension if not present in dest. */
	    if (dimids[i] < 0) {
	    	nct_var* dim = nct_add_dim(dest, vardim->len, strdup(vardim->name));
		dim->freeable_name = 1;
		dimids[i] = dim->id;
	    }
	    /* Create a new dimension if lengths mismatch in source and destination. */
	    else if (dest->dims[dimids[i]]->len != vardim->len) {
		nct_var* dim = nct_add_dim(dest, vardim->len, strdup(vardim->name));
		dim->freeable_name = 1;
		nct_ensure_unique_name(dim);
		dimids[i] = dim->id;
	    }
	}
	var = nct_add_var(dest, NULL, src->dtype, strdup(src->name), n, dimids);
	var->freeable_name = 1;
    }

    if (link)
	nct_link_data(var, src);
    else {
	int len = var->len;
	var->data = malloc(len*nctypelen(var->dtype));
	memcpy(var->data, src->data, len*nctypelen(var->dtype));
    }
    return var;
}

/* Compiler doesn't understand that we check elsewhere that ndims < 100
   and hence, sprintf(dimname+1, "%i" ndims++) is safe. */
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

/*
nct_var* nct_dim2coord(nct_var* dim, void* data, nc_type dtype) {
    int dimid = nct_id(dim->id);
    nct_var* restrict var = nct_add_var(dim->super, data, dtype, dim->name, 1, &dimid);
    var->freeable_name = dim->freeable_name;
    dim->freeable_name = 0;
    var->super->dims[dimid] = var;
    var->id = nct_coordid(var->id);
    nct_free_var(dim); free(dim);
    return var;
}*/

nct_var* nct_dim2coord(nct_var* var, void* src, nc_type dtype) {
    nct_set* set = var->super;
    if (set->varcapacity < set->nvars+1)
	if (!(set->vars = realloc(set->vars, (set->varcapacity=set->nvars+3)*sizeof(void*))))
	    goto failed;
    set->vars[set->nvars] = var;
    int dimid = var->id;
    *var = (nct_var){
	.super		= var->super,
	.id		= nct_coordid(set->nvars),
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
    *var->dimids = dimid;
    set->nvars++;
    return var;
failed:
    nct_puterror("(re/m)alloc failed in nct_add_var: %s\n", strerror(errno));
    return_error(NULL);
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
    int i, thisid = nct_id(var->id), nvars = var->super->nvars, ndims = var->super->ndims;
    nct_var **vars = var->super->vars, **dims = var->super->dims;
    const char* restrict name = var->name;

    /* variable names */
    for(i=0; i<thisid; i++)
	if (!strcmp(vars[i]->name, name))
	    goto makename;
    for(i=thisid+1; i<nvars; i++)
	if (!strcmp(vars[i]->name, name))
	    goto makename;

    /* dimension names */
    if (nct_iscoord(var)) {
	for(i=0; i<ndims; i++)
	    if (dims[i]->id != thisid && !strcmp(dims[i]->name, name))
		goto makename;
    }
    else
	for(i=0; i<ndims; i++)
	    if (!strcmp(dims[i]->name, name))
		goto makename;
    return var;

makename:
    char* newname = nct_find_unique_name_from(var->super, var->name, 0);
    if(var->freeable_name)
	free(var->name);
    var->name = newname;
    var->freeable_name = 1;
    return var;
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
    return_error(NULL);
}

nct_var* nct_firstvar(const nct_set* set) {
    int nvars = set->nvars;
    for(int i=0; i<nvars; i++)
	if(!nct_iscoord(set->vars[i]))
	    return set->vars[i];
    return NULL;
}

nct_var* nct_nextvar(const nct_var* var) {
    nct_set* set = var->super; // We assume that var is a valid variable.
    int nvars = set->nvars;
    for(int i=var->id+1; i<nvars; i++)
	if(!nct_iscoord(set->vars[i]))
	    return set->vars[i];
    return NULL;
}

nct_var* nct_prevvar(const nct_var* var) {
    nct_set* set = var->super; // We assume that var is a valid variable.
    for(int i=var->id-1; i>=0; i--)
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
    if(att->freeable & 1)
	free(att->value);
    if(att->freeable & 2)
	free(att->name);
    att->freeable = 0;
}

static void nct_free_var(nct_var* var) {
    free(var->dimids);
    for(int i=0; i<var->natts; i++)
	_nct_free_att(var->atts+i);
    free(var->atts);
    if(var->dtype == NC_STRING)
	for(int i=0; i<var->len; i++)
	    free(((char**)var->data)[i]);
    nct_unlink_data(var);
    var->capacity = 0;
    if(var->freeable_name)
	free(var->name);
}

void _nct_free(int _, ...) {
    va_list args;
    va_start(args, _);
    intptr_t addr;
    while((int)(addr = va_arg(args, intptr_t)) != -1)
	nct_free1((nct_set*)addr);
    va_end(args);
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
	nct_free_var(set->vars[i]);
	memset(set->vars[i], 0, sizeof(nct_var));
	free(set->vars[i]);
    }
    n = set->ndims;
    for(int i=0; i<n; i++) {
	nct_free_var(set->dims[i]);
	memset(set->dims[i], 0, sizeof(nct_var));
	free(set->dims[i]);
    }
    memset(set->vars, 0, sizeof(void*)*set->nvars);
    free(set->vars);
    memset(set->dims, 0, sizeof(void*)*set->ndims);
    free(set->dims);
    startpass;
    if (set->ncid > 0) {
	ncfunk(nc_close, set->ncid);
	set->ncid = 0;
    }
    endpass;
    if (set->owner) free(set);
}

nct_var* nct_get_dim(const nct_set* set, const char* name) {
    int ndims = set->ndims;
    for(int i=0; i<ndims; i++)
	if (!strcmp(name, set->dims[i]->name))
	    return set->dims[i];
    return NULL;
}

nct_var* nct_get_var(const nct_set* set, const char* name) {
    int nvars = set->nvars;
    for(int i=0; i<nvars; i++)
	if (!strcmp(name, set->vars[i]->name))
	    return set->vars[i];
    return NULL;
}

nct_att* nct_get_varatt(const nct_var* var, const char* name) {
    int natts = var->natts;
    for(int i=0; i<natts; i++)
	if(!strcmp(var->atts[i].name, name))
	    return var->atts+i;
    return NULL;
}

char* nct_get_varatt_text(const nct_var* var, const char* name) {
    int natts = var->natts;
    for(int i=0; i<natts; i++)
	if (!strcmp(var->atts[i].name, name))
	    return var->atts[i].value;
    return NULL;
}

int nct_get_vardimid(const nct_var* restrict var, int dimid) {
    dimid = nct_id(dimid);
    for(int i=0; i<var->ndims; i++)
	if (var->dimids[i] == dimid)
	    return i;
    return -1;
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
    for (int i=start; i<var->ndims; i++)
	len *= var->super->dims[var->dimids[i]]->len;
    return len;
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
    int year, month, day, hms[3]={0}, len;
    for(; *units; units++)
	if('0' <= *units && *units <= '9') {
	    if(sscanf(units, "%d-%d-%d%n", &year, &month, &day, &len) != 3) {
		nct_puterror("Could not read %s since WHEN (%s)", nct_timeunits[ui], units);
		return 1; }
	    break;
	}
    units += len;

    /* Optionally read hours, minutes, seconds. */
    for(int i=0; i<3 && *units; i++) {
	if(sscanf(units, "%2d", hms+i) != 1)
	    break;
	units += 2;
	while(*units && !('0' <= *units && *units <= '9')) units++;
    }
    *timetm = (struct tm){.tm_year=year-1900, .tm_mon=month-1, .tm_mday=day,
			  .tm_hour=hms[0], .tm_min=hms[1], .tm_sec=hms[2]};
    if (timeunit)
	*timeunit = ui;
    return 0;
}

int nct_link_data(nct_var* dest, nct_var* src) {
    if(!src->nusers) {
	if(nct_nlinked_vars >= nct_nlinked_max) {
	    nct_puterror("Maximum number of different links (%i) reached\n", nct_nlinked_max);
	    return_error(1); }
	src->nusers = nct_nusers+nct_nlinked_vars++;
    }
    ++*src->nusers;
    dest->data = src->data;
    dest->nusers = src->nusers;
    return 0;
}

void nct_allocate_varmem(nct_var* var) {
    if (var->capacity >= var->len)
	return;
    if cannot_free(var)
	var->data = malloc(var->len*nctypelen(var->dtype));
    else
	var->data = realloc(var->data, var->len*nctypelen(var->dtype));
    var->capacity = var->len;
    if (var->data)
	return;

    nct_puterror("memory allocation failed: %s\n", strerror(errno));
    print_varerror(var, "    ");
    var->capacity = 0;
    other_error;
}

nct_var* nct_load_as(nct_var* var, nc_type dtype) {
    if (dtype != NC_NAT) {
	if (var->dtype)
	    var->capacity *= nctypelen(dtype) / nctypelen(var->dtype);
	var->dtype = dtype;
    }
    nct_allocate_varmem(var);
    
    if (!(var->rules & (1<<nctrule_start | 1<<nctrule_length))) {
	nct_ncget_t getdata = nct_getfun[dtype];
	ncfunk(getdata, var->super->ncid, var->ncid, var->data);
	return var;
    }

    int ndims = var->ndims;
    nct_ncget_partial_t getdata = nct_getfun_partial[dtype];
    size_t start[ndims], count[ndims];
    memset(start, 0, ndims*sizeof(size_t));
    for(int i=0; i<ndims; i++)
	count[i] = var->super->dims[var->dimids[i]]->len;

    if (var->rules & (1<<nctrule_start)) {
	int vardimid = __nct_vardimid_from_getvararule(var->a[nctrule_start]);
	start[vardimid] = __nct_offset_from_getvararule(var->a[nctrule_start]);
    }
    /* arg in nctrule_length is not needed because length is read from the dimension
       which has been edited already.
       Maybe start should also be implemented this way. */

    ncfunk(getdata, var->super->ncid, var->ncid, start, count, var->data);
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
    nct_interpret_timeunit(var, timetm, &ui);
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

void nct_print_var(const nct_var* var, const char* indent) {
    printf("%s%s%s %s%s(%zu)%s:\n%s  %i dimensions: ( ",
	   indent, nct_type_color, nct_typenames[var->dtype],
	   nct_varname_color, var->name, var->len, nct_default_color,
	   indent, var->ndims);
    for(int i=0; i<var->ndims; i++) {
	nct_var* dim = var->super->dims[var->dimids[i]];
	printf("%s(%zu), ", dim->name, dim->len);
    }
    printf(")\n");
    printf("%s  [", indent);
    nct_print_data(var);
    puts("]");
}

void nct_print_dim(const nct_var* var, const char* indent) {
    printf("%s%s%s %s%s(%zu)%s:\n",
	   indent, nct_type_color, nct_typenames[var->dtype],
	   nct_dimname_color, var->name, var->len, nct_default_color);
    if(nct_iscoord(var)) {
	printf("%s  [", indent);
	nct_print_data(var);
	puts("]");
    }
}

void nct_print(const nct_set* set) {
    printf("%s%i variables, %i dimensions%s\n", nct_varset_color, set->nvars, set->ndims, nct_default_color);
    int n = set->ndims;
    for(int i=0; i<n; i++)
	nct_print_dim(set->dims[i], "  ");
    nct_foreach(set, var)
	nct_print_var(var, "  ");
}

static nct_set* nct_after_lazyread(nct_set* s, int flags) {
    if (flags & nct_rlazy)
	return s;
    int nvars = s->nvars;
    for(int i=0; i<nvars; i++)
	nct_load(s->vars[i]);
    ncfunk(nc_close, s->ncid);
    s->ncid = 0;
    return s;
}

void* nct_read_from_nc_as(const char* filename, const char* varname, nc_type nctype) {
    if (!varname) {
	nct_readm_ncf(v, filename, nct_rlazy);
	nct_var* var = nct_firstvar(&v);
	if (!var) {
	    nct_puterror("No variables in \"%s\"\n", filename);
	    nct_free1(&v);
	    return_error(NULL);
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
    ncfunk(nc_open, filename, NC_NOWRITE, &ncid);
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
	return_error(NULL);
    }
    nct_ncget_t func = nct_getfun[dtype];
    ncfunk(func, ncid, varid, ret);
    ncfunk(nc_close, ncid);
    return ret;
}

static nct_set* nct_read_ncf_lazy(const char* filename, int flags) {
    nct_set* s = malloc(sizeof(nct_set));
    nct_read_ncf_lazy_gd(s, filename, flags) -> owner=1;
    return s;
}

static nct_set* nct_read_ncf_lazy_gd(nct_set* dest, const char* filename, int flags) {
    int ncid, ndims, nvars;
    ncfunk(nc_open, filename, NC_NOWRITE, &ncid);
    ncfunk(nc_inq_ndims, ncid, &ndims);
    ncfunk(nc_inq_nvars, ncid, &nvars);
    *dest = (nct_set){
	.ncid = ncid,
	.ndims = ndims,
	.nvars = nvars,
	.dimcapacity = ndims + 1,
	.varcapacity = nvars + 3,
    };
    dest->dims = calloc(dest->dimcapacity, sizeof(void*));
    dest->vars = calloc(dest->varcapacity, sizeof(void*));
    for(int i=0; i<nvars; i++)
	_nct_read_var_info(dest, i, flags);
    for(int i=0; i<ndims; i++)
	/* If a variable is found with the same name,
	   this will make a pointer to that instead of creating a new object.
	   Id will then become nct_coordid(var_id). */
	_nct_read_dim(dest, i);
    nct_foreach(dest, var)
	var->len = nct_get_len_from(var, 0); // Lengths could not be calculated before reading dimensions.
    return dest;
}

nct_set* nct_read_ncf(const char* filename, int flags) {
    nct_set* s = nct_read_ncf_lazy(filename, flags);
    return nct_after_lazyread(s, flags);
}

nct_set* nct_read_ncf_gd(nct_set* s, const char* filename, int flags) {
    nct_read_ncf_lazy_gd(s, filename, flags);
    return nct_after_lazyread(s, flags);
}

nct_set* nct_read_mfnc_regex(const char* filename, int regex_cflags, char* dim) {
    nct_set* s = malloc(sizeof(nct_set));
    nct_read_mfnc_regex_gd(s, filename, regex_cflags, dim) -> owner = 1;
    return s;
}

nct_set* nct_read_mfnc_regex_gd(nct_set* s0, const char* filename, int regex_cflags, char* dim) {
    char* names = nct__get_filenames(filename, 0);
    int num = (intptr_t)nct__get_filenames(NULL, 0); // returns the number of files read on previous call
    nct_read_mfnc_ptr_gd(s0, names, num, dim);
    free(names);
    return s0;
}

nct_set* nct_read_mfnc_ptr(const char* filename, int n, char* dim) {
    nct_set* s = malloc(sizeof(nct_set));
    nct_read_mfnc_ptr_gd(s, filename, n, dim) -> owner = 1;
    return s;
}

/* filenames has the form of "name1\0name2\0name3\0last_name\0\0" */
nct_set* nct_read_mfnc_ptr_gd(nct_set* vs0, const char* filenames, int nfiles, char* dim) {
    nct_set vs1;
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
	    return_error(vs0); // error if user told to count files and result == 0
	}
    }
    else if (!nfiles)
	return vs0; // no error if user wanted to read 0 files
    ptr = filenames;
    nct_read_ncf_gd(vs0, ptr, nct_readflags);
    ptr += strlen(ptr)+1;
    nfiles--;
    while (*ptr) {
	nct_read_ncf_gd(&vs1, ptr, nct_readflags|nct_ratt); // concatenation needs attributes to convert time units
	nct_concat(vs0, &vs1, dim, --nfiles);
	nct_free1(&vs1); // TODO: read files straight to vs0 to avoid unnecessarily allocating and freeing memory
	ptr += strlen(ptr)+1;
    }
    return vs0;
}

nct_var* nct_rename(nct_var* var, char* name, int freeable) {
    if (var->freeable_name)
	free(var->name);
    var->name = name;
    var->freeable_name = freeable;
    return var;
}

nct_var* nct_set_length(nct_var* coord, int offset) {
    if (coord->data)
	;
    else
	_nct_setrule_length(coord, 0, offset);
    coord->len = offset;

    /* Each variable has to be edited according to the change in this coordinate. */
    nct_foreach(coord->super, var) {
	int dimid = nct_get_vardimid(var, coord->id);
	if (dimid < 0)
	    continue;
	if (var->data)
	    //_nct_select(var, dimid, offset, offset+coord->len);
	    puts("Set the rule before loading the variable or implement _nct_select");
	else
	    _nct_setrule_length(var, dimid, offset);
    }
    return coord;
}

nct_var* nct_set_start(nct_var* coord, int offset) {
    int size = nctypelen(coord->dtype);
    if (coord->data)
	memmove(coord->data, coord->data+offset*size, (coord->len-offset)*size);
    else
	_nct_setrule_start(coord, 0, offset);
    coord->len -= offset;

    /* Each variable has to be edited according to the change in this coordinate. */
    nct_foreach(coord->super, var) {
	int dimid = nct_get_vardimid(var, coord->id);
	if (dimid < 0)
	    continue;
	if (var->data)
	    //_nct_select(var, dimid, offset, offset+coord->len);
	    puts("Set the start before loading the variable or implement _nct_select");
	else
	    _nct_setrule_start(var, dimid, offset);
    }
    return coord;
}

time_t nct_timediff(const nct_var* var1, const nct_var* var0) {
    struct tm tm1, tm0;
    int unit1, unit0;
    if (nct_interpret_timeunit(var1, &tm1, &unit1) | nct_interpret_timeunit(var0, &tm0, &unit0))
	return_error(1<<30);
    if (unit1 != unit0) {
	nct_puterror("timeunit mismatch (%s, %s)\n", nct_timeunits[unit1], nct_timeunits[unit0]);
	return_error(1<<30); }
    if (!memcmp(&tm1, &tm0, sizeof(tm1)))
	return 0;
    time_t diff = mktime(&tm1) - mktime(&tm0);
    return diff / timecoeff[unit0];
}

void nct_unlink_data(nct_var* var) {
    if (!cannot_free(var))
	free(var->data);
    if(var->nusers)
	(*var->nusers)--;
    var->data = var->nusers = NULL;
    var->capacity = 0;
}

void nct_write_nc(const nct_set* src, const char* name) {
    int ncid, id;
    startpass;

    ncfunk(nc_create, name, NC_NETCDF4|NC_CLOBBER, &ncid);
    ncfunk(nc_set_fill, ncid, NC_NOFILL, NULL);
    for(int i=0; i<src->ndims; i++)
	ncfunk(nc_def_dim, ncid, src->dims[i]->name, src->dims[i]->len, &id);
    for(int i=0; i<src->nvars; i++) {
	nct_var* v = src->vars[i];
	int unlink = 0;
	if(!v->data) {
	    if(v->super->ncid > 0) {
		nct_load(v);
		unlink = 1;
	    }
	    else continue;
	}
	ncfunk(nc_def_var, ncid, v->name, v->dtype, v->ndims, v->dimids, &id);
	ncfunk(nc_put_var, ncid, id, v->data);
	for(int a=0; a<v->natts; a++)
	    ncfunk(nc_put_att_text, ncid, i, v->atts[a].name,
		   v->atts[a].len, v->atts[a].value);
	if(unlink)
	    nct_unlink_data(v);
    }
    ncfunk(nc_close, ncid);
    endpass;
}

char* nct__get_filenames(const char* restrict filename, int regex_cflags) {
    static int num;
    if(!filename)
	return (char*)(intptr_t)num;
    /* find the name of the directory */
    int i, ind = 0;
    DIR* dp;
    struct dirent *entry;
    regex_t reg;
    char* dirname = NULL;
    int smatch = 2048, lmatch=0;
    char* match = malloc(smatch);
    for (i=0; filename[i]; i++)
	if (filename[i] == '/')
	    ind = i;
    /* open the directory */
    int p2 = strlen(filename) - ind;
    char str[(p2>ind? p2: ind) + 2];
    if (ind) { strncpy(str, filename, ind); str[ind] = '\0'; }
    else strcpy(str, ".");
    dirname = strdup(str);
    dp = opendir(dirname);
    if (!dp) {
	nct_puterror("could not open directory \"%s\": %s", dirname, strerror(errno));
	return_error(NULL);
    }
    int dlen = strlen(dirname);
    /* find the matching files */
    if(chdir(dirname)) {
	nct_puterror("chdir(dirname) in nct__get_filenames: %s", strerror(errno));
	return_error(NULL);
    }
    strcpy(str, filename+ind+1);
    i = regcomp(&reg, str, regex_cflags);
    if(i) {
	char er[700];
	regerror(i, &reg, er, 700);
	nct_puterror("regcomp error:\n    %s\n", er);
	return_error(NULL);
    }
    num = 0;
    while ((entry = readdir(dp))) {
	if(regexec(&reg, entry->d_name, 0, NULL, 0)) continue;
	int len = dlen + 1 + strlen(entry->d_name) + 1;
	if(lmatch+len+1 > smatch) {
	    smatch = lmatch + len + 1024;
	    match = realloc(match, smatch);
	}
	sprintf(match+lmatch, "%s/%s", dirname, entry->d_name);
	lmatch += len;
	num++;
    }
    closedir(dp);
    match[lmatch] = '\0'; // end with two null bytes;
    if(chdir(getenv("PWD"))) {
	nct_puterror("chdir in nct_get_filenames: %s", strerror(errno));
	return_error(NULL);
    }
    char* sorted = malloc(lmatch+1);
    nct__sort_str(sorted, match, num);
    free(match);
    free(dirname);
    regfree(&reg);
    return sorted;
}

/* Selection sort that does not change src. */
char* nct__sort_str(char* dst, const char* restrict src, int n) {
    const char *sptr, *mptr;
    char *dptr = dst;
    if(n<=0) n = 4096;
    char used[n];
    memset(used, 0, n);
    int ind, mind, breakflag;
    while(1) {
	sptr = src;
	mptr = NULL;
	ind = mind = 0;
	breakflag = 1;
	while(*sptr) {
	    if(!used[ind]) {
		breakflag = 0;
		if(!mptr || strcmp(sptr, mptr) < 0) {
		    mptr = sptr;
		    mind = ind;
		}
	    }
	    sptr += strlen(sptr)+1;
	    ind++;
	}
	if(breakflag)
	    goto out;
	strcpy(dptr, mptr);
	used[mind] = 1;
	dptr += strlen(dptr)+1;
    }
out:
    *dptr = '\0';
    return dst;
}
