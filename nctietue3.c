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

nct_var*	nct_set_concat(nct_var* var0, nct_var* var1, int howmany_left);

#define nct_rnoall (nct_rlazy | nct_rcoord) // if (nct_readflags & nct_rnoall) ...

#define setrule(a, r) ((a)->rules |= 1<<(r))
#define hasrule(a, r) ((a)->rules & 1<<(r))

#define startpass			\
    int __nct_err = nct_error_action;	\
    if (nct_error_action == nct_auto)	\
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

int nct_readflags, nct_ncret, nct_error_action, nct_verbose;
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

void* nct_getfun_1[] = {
    [NC_NAT]    = nc_get_var1,
    [NC_BYTE]   = nc_get_var1_schar,
    [NC_CHAR]   = nc_get_var1_schar,
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
    if(var->dimcapacity < var->ndims+1) {
	var->dimcapacity = var->ndims+2;
	var->dimids = realloc(var->dimids, var->dimcapacity*sizeof(int));
    }
    memmove(var->dimids+1, var->dimids, var->ndims*sizeof(int));
    var->dimids[0] = dimid;
    var->ndims++;
    return var;
}

/* Concatenation is not yet supported along other existing dimensions than the first one.
   Coordinates will be loaded if aren't already.
   Variable will not be loaded, except if the first is loaded and the second is not.
   When unloaded sets are concatenated, they must not be freed before loading the data.
   The following is hence an error:
   *	concat(set0, set1) // var n is not loaded
   *	free(set1)
   *	load(set0->vars[n])
   */
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
	/* Not a concatenation but useful. */
	else if(!strcmp(dimname, "-v")) {
	    varid1=-1;
	    nct_foreach(vs1, var1) {
		nct_var* var = nct_copy_var(vs0, var1, 1);
		nct_ensure_unique_name(var);
	    }
	    return vs0;
	}
	else
	    dimname++; // don't include the hyphen
    }
    /* Now dimname is either an existing dimension or one to be created. */
    dimid0 = nct_get_dimid(vs0, dimname);
    if (dimid0 < 0)
	dimid0 = nct_dimid(nct_add_dim(vs0, 1, dimname));
    /* The dimension exists now in vs0 but not necessarily in vs1. */
    dimid1 = nct_get_dimid(vs1, dimname);
    if (dimid1 < 0)
	vs0->dims[dimid0]->len++;
    else {
	vs0->dims[dimid0]->len += vs1->dims[dimid1]->len;
	/* If the dimension is also a variable, concat that */
	if((varid0=nct_get_varid(vs0, dimname)) >= 0 &&
	   (varid1=nct_get_varid(vs1, dimname)) >= 0)
	{
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
    /* Finally concatenate all variables.
       Called concat-functions change var->len but not vardim->len which has already been changed here
       so that changes would not cumulate when having multiple variables. */
    nct_foreach(vs0, var0) {
	nct_var* var1 = nct_get_var(vs1, var0->name);
	if (nct_get_vardimid(var0, dimid0) < 0)
	    nct_add_vardim_first(var0, dimid0);
	if (!var1)
	    continue;
	if (!var0->data)
	    nct_set_concat(var0, var1, howmany_left);
	else {
	    if(!var1->data)
		nct_load(var1); // Now data is written here and copied to var0. Not good.
	    _nct_concat_var(var0, var1, dimid0, howmany_left);
	}
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

nct_att* nct_copy_att(nct_var* var, const nct_att* src) {
    long len = src->len * nctypelen(src->dtype);
    nct_att att = {
	.name	= strdup(src->name),
	.value	= malloc(len),
	.dtype	= src->dtype,
	.len	= src->len,
	.freeable = 3,
    };
    memcpy(att.value, src->value, len);
    return nct_add_varatt(var, &att);
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
		dimids[i] = nct_dimid(dim);
	    }
	    /* Create a new dimension if lengths mismatch in source and destination. */
	    else if (dest->dims[dimids[i]]->len != vardim->len) {
		nct_var* dim = nct_add_dim(dest, vardim->len, strdup(vardim->name));
		dim->freeable_name = 1;
		nct_ensure_unique_name(dim);
		dimids[i] = nct_dimid(dim);
	    }
	}
	var = nct_add_var(dest, NULL, src->dtype, strdup(src->name), n, dimids);
	var->freeable_name = 1;
    }

    for(int a=0; a<src->natts; a++)
	nct_copy_att(var, src->atts+a);

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

size_t nct_find_sorted_(const nct_var* var, double value, int right) {
    double (*getfun)(const nct_var*, size_t) = var->data ? nct_get_floating : nct_getl_floating;
    long long sem[] = {0, var->len-1, (var->len)/2}; // start, end, mid
    while (1) {
	if (sem[1]-sem[0] <= 1) {
	    double v0 = getfun(var, sem[0]),
		   v1 = getfun(var, sem[1]);
	    return value<v0 ? sem[0] : value<v1 ? sem[1] : value>v1 ? sem[1]+1 : sem[1]+!!right;
	}
	double try = getfun(var, sem[2]);
	sem[try>value] = sem[2]; // if (try>value) end = mid; else start = mid+1;
	sem[2] = (sem[0]+sem[1]) / 2;
    }
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
    if (var->dtype == NC_STRING)
	for(int i=0; i<var->len; i++)
	    free(((char**)var->data)[i]);
    nct_unlink_data(var);
    var->capacity = 0;
    if (var->freeable_name)
	free(var->name);
    if hasrule(var, nct_r_concat)
	free(var->rule[nct_r_concat].arg.v);
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
    free(set->filename);
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

nct_var* nct_get_vardim(const nct_var* var, int num) {
    return var->super->dims[var->dimids[num]];
}

int nct_get_vardimid(const nct_var* restrict var, int dimid) {
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
    int ndims = var->ndims;
    for (int i=start; i<ndims; i++)
	len *= var->super->dims[var->dimids[i]]->len;
    return len;
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
	    nct_return_error(1); }
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
    nct_other_error;
}

static void perhaps_close_the_file(nct_set* set) {
    if (set->ncid > 0 && !(nct_readflags & nct_rkeep)) {
	ncfunk(nc_close, set->ncid);
	set->ncid = 0;
    }
}

static void perhaps_open_the_file(nct_var* var) {
    if (var->super->ncid <= 0 && var->super->filename)
	ncfunk(nc_open, var->super->filename, NC_NOWRITE, &var->super->ncid);
}

#include "load_data.h" // nct_load_as

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
    if (flags & nct_rlazy) {
	perhaps_close_the_file(s);
	return s;
    }
    unsigned old = nct_readflags;
    nct_readflags = flags | nct_rkeep; // don't close and reopen on each variable
    if (flags & nct_rcoord) {
	int ndims = s->ndims;
	for(int i=0; i<ndims; i++)
	    if (nct_iscoord(s->dims[i]))
		nct_load(s->dims[i]);
	nct_readflags = old;
	perhaps_close_the_file(s);
	return s;
    }
    int nvars = s->nvars;
    for(int i=0; i<nvars; i++)
	nct_load(s->vars[i]);
    nct_readflags = old;
    perhaps_close_the_file(s);
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
	nct_return_error(NULL);
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
	.filename = strdup(filename),
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

nct_set* nct_read_ncf(const char* filename, int flags) {
    nct_set* s = nct_read_ncf_lazy(filename, flags);
    return nct_after_lazyread(s, flags);
}

nct_set* nct_read_ncf_gd(nct_set* s, const char* filename, int flags) {
    nct_read_ncf_lazy_gd(s, filename, flags);
    return nct_after_lazyread(s, flags);
}

/* Reading multiple files as if they were one file.
   If data shouldn't be loaded according to readflags,
   these create null-terminated list nct_set* and return pointer to the first member.
   */

nct_set* nct_read_mfnc_regex(const char* filename, int regex_cflags, char* dim) {
    return nct_read_mfnc_regex_(filename, regex_cflags, dim, NULL, 0, 0, NULL);
}

nct_set* nct_read_mfnc_regex_(const char* filename, int regex_cflags, char* dim,
	void (*matchfun)(const char* restrict, int, regmatch_t*, void*), int size1, int nmatch, void** matchdest) {
    char* names = nct__get_filenames_(filename, 0, matchfun, size1, nmatch, matchdest);
    int num = nct__getn_filenames(); // returns the number of files read on previous call
    if (num == 0) {
	free(names);
	nct_puterror("No files match \"%s\"\n", filename);
	nct_return_error(NULL);
    }
    nct_set* s = nct_read_mfnc_ptr(names, num, dim);
    free(names);
    return s;
}

/* filenames has the form of "name1\0name2\0name3\0last_name\0\0" */
nct_set* nct_read_mfnc_ptr(const char* filenames, int nfiles, char* dim) {
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
    ptr = filenames;

    nct_set *set, *setptr;
    if (!(nct_readflags & nct_rnoall))
	goto read_and_load;

    /* No loading data. Making list of nct_set* and setting right loading rules. */
    set = setptr = calloc(nfiles+1, sizeof(nct_set)); // +1 for NULL-termination
    nct_read_ncf_gd(set, ptr, nct_readflags|nct_ratt); // concatenation needs attributes to convert time units
    set->owner = 1;
    nfiles--;
    setrule(set, nct_r_list); // so that nct_free knows to free other members as well
    ptr += strlen(ptr)+1;
    while (*ptr) {
	nct_read_ncf_gd(++setptr, ptr, nct_readflags|nct_ratt);
	nct_concat(set, setptr, dim, --nfiles);
	ptr += strlen(ptr)+1;
    }
    return set;

read_and_load:
    set = malloc(sizeof(nct_set));
    nct_read_ncf_gd(set, ptr, nct_readflags|nct_ratt); // concatenation needs attributes to convert time units
    set->owner = 1;
    nfiles--;
    ptr += strlen(ptr)+1;
    while (*ptr) {
	nct_readm_ncf(set1, ptr, nct_readflags|nct_ratt);
	nct_concat(set, &set1, dim, --nfiles);
	nct_free1(&set1); // TODO: read files straight to vs0 to avoid unnecessarily allocating and freeing memory
	ptr += strlen(ptr)+1;
    }
    return set;
}

nct_set* nct_read_mfnc_ptrptr(char** filenames, int nfiles, char* dim) {
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
    nct_set* set = nct_read_mfnc_ptr(files, n, dim);
    free(files);
    return set;
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

    nct_var* var1 = get_var_from_filenum(var0, fileno);
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
    return var0;
}

nct_var* nct_set_length(nct_var* dim, size_t arg) {
    dim->len = arg;
    setrule(dim->super, nct_r_start); // start is used to mark length as well
    nct_foreach(dim->super, var)
	var->len = nct_get_len_from(var, 0);
    return dim;
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

time_t nct_timediff(const nct_var* var1, const nct_var* var0) {
    struct tm tm1, tm0;
    int unit1, unit0;
    if (nct_interpret_timeunit(var1, &tm1, &unit1) | nct_interpret_timeunit(var0, &tm0, &unit0))
	nct_return_error(1<<30);
    if (unit1 != unit0) {
	nct_puterror("timeunit mismatch (%s, %s)\n", nct_timeunits[unit1], nct_timeunits[unit0]);
	nct_return_error(1<<30); }
    if (!memcmp(&tm1, &tm0, sizeof(tm1)))
	return 0;
    time_t diff = mktime(&tm1) - mktime(&tm0);
    return diff / timecoeff[unit0];
}

void nct_unlink_data(nct_var* var) {
    if (!cannot_free(var))
	free(nct_rewind(var)->data);
    if (var->nusers)
	(*var->nusers)--;
    var->data = var->nusers = NULL;
    var->capacity = 0;
}

enum {_createwhole, _createcoords};
static int _nct_create_nc(const nct_set* src, const char* name, int what) {
    int ncid, id;
    startpass;

    ncfunk(nc_create, name, NC_NETCDF4|NC_CLOBBER, &ncid);
    ncfunk(nc_set_fill, ncid, NC_NOFILL, NULL);
    int n = src->ndims;
    for(int i=0; i<n; i++)
	ncfunk(nc_def_dim, ncid, src->dims[i]->name, src->dims[i]->len, &id);
    n = src->nvars;
    for(int i=0; i<n; i++) {
	nct_var* v = src->vars[i];
	if (what==_createcoords && !nct_iscoord(v))
	    continue;
	int unlink = 0;
	if(!v->data) {
	    if(v->super->ncid > 0 || v->super->filename) {
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
    endpass;
    return ncid;
}

int nct_create_nc(const nct_set* src, const char* name) {
    return _nct_create_nc(src, name, _createwhole);
}

int nct_createcoords_nc(const nct_set* src, const char* name) {
    return _nct_create_nc(src, name, _createcoords);
}

void nct_write_nc(const nct_set* src, const char* name) {
    startpass;
    ncfunk(nc_close, nct_create_nc(src, name));
    endpass;
}

char* nct__get_filenames(const char* restrict filename, int regex_cflags) {
    return nct__get_filenames_(filename, regex_cflags, NULL, 0, 0, NULL);
}

char* nct__get_filenames_(const char* restrict filename, int regex_cflags,
	void (*fun)(const char* restrict, int, regmatch_t* pmatch, void*), int size1dest, int nmatch, void** dest) {
    static int num;
    if (!filename)
	return (char*)(intptr_t)num;
    /* find the name of the directory */
    int i, ind = 0;
    DIR* dp;
    struct dirent *entry;
    regex_t reg;
    char* dirname = NULL;
    int smatch = 2048, lmatch=0; // space-of-match, length-of-match
    char* match = malloc(smatch);
    if (fun)
	*dest = malloc(smatch*size1dest);
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
	nct_return_error(NULL);
    }
    int dlen = strlen(dirname);
    /* find the matching files */
    if (chdir(dirname)) {
	nct_puterror("chdir(dirname) in nct__get_filenames: %s", strerror(errno));
	nct_return_error(NULL);
    }
    strcpy(str, filename+ind+1);
    i = regcomp(&reg, str, regex_cflags);
    if (i) {
	char er[700];
	regerror(i, &reg, er, 700);
	nct_puterror("regcomp error:\n    %s\n", er);
	nct_return_error(NULL);
    }
    num = 0;
    nmatch *= !!fun;
    regmatch_t pmatch[nmatch];
    while ((entry = readdir(dp))) {
	if (regexec(&reg, entry->d_name, nmatch, pmatch, 0))
	    continue;
	int len = dlen + 1 + strlen(entry->d_name) + 1;
	if (lmatch+len+1 > smatch) {
	    smatch = lmatch + len + 1024;
	    match = realloc(match, smatch);
	    if (fun)
		*dest = realloc(*dest, smatch*size1dest);
	}
	sprintf(match+lmatch, "%s/%s", dirname, entry->d_name);
	if (fun)
	    fun(entry->d_name, num, pmatch, *dest);
	lmatch += len;
	num++;
    }
    closedir(dp);
    match[lmatch] = '\0'; // end with two null bytes;
    if (chdir(getenv("PWD"))) {
	nct_puterror("chdir in nct__get_filenames: %s", strerror(errno));
	nct_return_error(NULL);
    }
    char* sorted = malloc(lmatch+1);
    void* sorted_dest = fun ? malloc(lmatch*size1dest) : NULL;
    void* destarr[2] = {sorted_dest};
    if (fun)
	destarr[1] = *dest;
    nct__sort_str(sorted, match, num, fun? destarr: NULL, size1dest);
    if (fun) {
	free(*dest);
	*dest = sorted_dest;
    }
    free(match);
    free(dirname);
    regfree(&reg);
    return sorted;
}

/* Selection sort that does not change src.
 * Other is an optional array which is sorted like src. other[0] is dest and other[1] is src. */
char* nct__sort_str(char* dst, const char* restrict src, int n, void* other[2], int size1other) {
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
		if (!strptr || strcmp(sptr, strptr) < 0) {
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
	n_sorted++;
    }
out:
    *dptr = '\0';
    return dst;
}
