#ifndef __NCTIETUE__
#define __NCTIETUE__

#include <netcdf.h>
#include <time.h>
#include <stdio.h>
#include <stddef.h>
#include <regex.h> // regmatch_t

typedef struct nct_set nct_set;
typedef struct nct_var nct_var;
typedef struct nct_att nct_att;
typedef struct nct_anyd nct_anyd;
typedef union  nct_any nct_any;
typedef int (*nct_ncget_t)(int,int,void*);
typedef int (*nct_ncget_partial_t)(int, int, const size_t*, const size_t*, void*);
typedef int (*nct_ncget_1_t)(int, int, const size_t*, void*);

/* Use this to redirect error messages elsewhere than to stderr:
 *	char errormsg[512];
 *	nct_stderr = fmemopen(errormsg, 512, "w");
 */
extern FILE* nct_stderr;

#define nct_other_error switch(nct_error_action) {	\
    case nct_auto:				\
    case nct_interrupt: asm("int $3"); break;	\
    default:;					\
}

#define nct_return_error(val) switch(nct_error_action) {	\
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
	    nct_other_error;			\
	}					\
    } while(0)

extern int nct_ncret;
extern const char* nct_error_color;
extern const char* nct_default_color;

/* What happens on error besides writing an error message.
 * auto (default):
 *	interrupt in those functions which are likely used before the real work.
 *	pass      in those functions which are likely used after  the real work.
 * interrupt:
 *	asm("int $3");
 * pass:
 *	don't do anything
 */
enum {nct_auto, nct_interrupt, nct_pass};
extern int nct_error_action;

union nct_any {
    char hhi;
    char c;
    unsigned char hhu;
    short hi;
    unsigned short hu;
    int i;
    unsigned u;
    long long lli;
    long long unsigned llu;
    float f;
    double lf;
    void* v;
    time_t t;
};

/* These are for internal use but nct_r_nrules is needed in this header. */
typedef enum {
    nct_r_start, nct_r_concat, nct_r_list, nct_r_nrules,
} nct_rule_e;
typedef struct {
    nct_any arg;
    int n;		// if arg.v is list
    int capacity;	// if arg.v is list
} nct_rule;

struct nct_att {
    char*    name;
    void*    value;
    nc_type  dtype;
    int      len;
    unsigned freeable;
};

#define nct_maxdims 5
struct nct_var {
    nct_set* super;
    int      id,   // nct_id(id) is location of this in set->vars, if exists there, otherwise in set->dims
	     ncid; // ncid of the variable if this is a coordinate (both dim and var)
    char*    name;
    char     freeable_name;
    int      ndims, dimcapacity;
    int*     dimids; // staattinen määrä muistia kelvanneisi
    int      natts, attcapacity;
    nct_att* atts;
    size_t   len, capacity;
    int      filedimensions[nct_maxdims]; // set when reading and not changed by any function
    nc_type  dtype;
    int      not_freeable;
    unsigned char* nusers; // the first user is not counted
    void*    data;
    unsigned  rules; // a bitmask of rules which are in use
    nct_rule rule[nct_r_nrules];
};

struct nct_set {
    int       nvars, varcapacity;
    nct_var** vars;
    int       ndims, dimcapacity;
    nct_var** dims; // points to a variable with the same name if available
    int       natts, attcapacity;
    nct_att*  atts;
    int       ncid, owner;
    unsigned  rules; // a bitmask of rules which are in use
};

struct nct_anyd {
    nct_any a;
    long    d;
};

#define nct_isset(set) (sizeof(set)==sizeof(nct_set)) // whether this is nct_var or nct_set
#define nct_loadable(var) ((var)->super->ncid > 0)

/* nct_var has negative id if the variable is dimension or coordinate */
#define nct_coordid(id) (-(id)-1)
#define nct_id(id) ((id)<0 ? -(id)-1 : (id))
#define nct_iscoord(var) (((var)->id)<0)

nct_var* nct_add_dim(nct_set* set, size_t len, char* name);
nct_var* nct_add_var(nct_set* set, void* src, nc_type dtype, char* name,
		     int ndims, int* dimids);
/* Calls nct_add_var with ndims = set->ndims and dimids = {0,1,2,3,...,ndims-1} */
nct_var* nct_add_var_alldims(nct_set* set, void* src, nc_type dtype, char* name);

nct_att* nct_add_varatt_text(nct_var* var, char* name, char* value, unsigned freeable);
nct_var* nct_add_vardim_first(nct_var* var, int dimid);

nct_set* nct_concat(nct_set *vs0, nct_set *vs1, char* dimname, int howmany_left);

/* see nct_mktime0 */
nct_var* nct_convert_timeunits(nct_var* var, const char* units);

/* Like nct_write_nc but returns ncid without closing it. */
int nct_create_nc(const nct_set* src, const char* name);
/* Like nct_create_nc but writes only coordinates and not other variables. */
int nct_createcoords_nc(const nct_set* src, const char* name);

nct_var* nct_copy_var(nct_set*, nct_var*, int link);

#define nct_create_simple(...) _nct_create_simple(__VA_ARGS__, 0)
#define nct_create_simple_gd(...) _nct_create_simple_gd(__VA_ARGS__, 0)
nct_set* _nct_create_simple(void* dt, int dtype, ...);
nct_set* _nct_create_simple_gd(nct_set* s, void* dt, int dtype, ...);

nct_var* nct_dim2coord(nct_var*, void*, nc_type);

/* Nothing is calculated nor data moved.
   The dimension is just simply dropped.
   This is used after doing something else,
   for example in nct_mean_first after calculating the mean.
   Argument shrink tells whether to realloc to the smaller size if possible.
   */
nct_var* nct_drop_vardim(nct_var* var, int dim, int shrink);
nct_var* nct_drop_vardim_first(nct_var*) __THROW __attribute__((deprecated));

/* Fill value won't be correct if some variables have floating point data and other variables integer data.
   To expand in start: start0_end1 == 0.
   To expand in end:   start0_end1 == 1. */
nct_var* nct_expand_dim(nct_var* dim, int howmuch, int start0_end1, nct_any fill);
#define nct_expandg_dim(set, name, howmuch, start0_end1, fill) nct_expand_dim(nct_get_var(set, name), howmuch, start0_end1, fill)

void nct_finalize(); // calls nc_finalize to free memory used by netcdf

/* If name is not unique, uses nct_find_unique_name_from to replace it. */
nct_var* nct_ensure_unique_name(nct_var* var);
char* nct_find_unique_name_from(nct_set* set, const char* initname, int num);

/* Inefficient if var is not loaded. Meant for light use.
   If right, then returns n+1 if index n matches.
   nct_find_sorted (without underscore) is meant to be used as the function,
   right is an optional argument with default value zero. */
size_t nct_find_sorted_(const nct_var* var, double value, int right);
#define _nct_find_sorted(var, value, right, ...) nct_find_sorted_(var, value, right)
#define nct_find_sorted(...) _nct_find_sorted(__VA_ARGS__, 0)

nct_var* nct_firstvar(const nct_set*);
nct_var* nct_nextvar(const nct_var*);
nct_var* nct_prevvar(const nct_var*);
nct_var* nct_lastvar(const nct_set*);

#define nct_foreach(set, var) \
    for(nct_var* var=nct_firstvar(set); var; var=nct_nextvar(var))

#define nct_free(...) _nct_free(0, __VA_ARGS__, -1)
void _nct_free(int _, ...); // first argument is meaningless
void nct_free1(nct_set*);

void nct_get_coords_from_ind(const nct_var* var, size_t* out, size_t ind); // coordinates of the index
nct_att* nct_get_varatt(const nct_var* var, const char* name);
char* nct_get_varatt_text(const nct_var*, const char*);
nct_var* nct_get_dim(const nct_set* set, const char* name);
nct_var* nct_get_var(const nct_set* set, const char* name);
nct_var* nct_get_vardim(const nct_var* var, int num);
int nct_get_vardimid(const nct_var* restrict var, int dimid);
int nct_get_varid(const nct_set* restrict, const char* restrict);
int nct_get_dimid(const nct_set* restrict, const char* restrict);
size_t nct_get_len_from(const nct_var*, int startdim);
double nct_getg_floating(const nct_var* var, size_t ind); // general: calls either getl or get
double nct_getg_integer(const nct_var* var, size_t ind);  // general: calls either getl or get
double    nct_getl_floating(const nct_var*, size_t); // If the value has to be loaded.
long long nct_getl_integer(const nct_var*, size_t);  // If the value has to be loaded.

/* Reads attribute "units" from $var
   and fills timetm according to that
   and unit with enumeration of time unit (days, seconds, etc.).
   Returns nonzero if interpreting fails, 0 on success.
   For form of the attribute, see nct_mktime0. */
int nct_interpret_timeunit(const nct_var* var, struct tm* timetm, int* unit);

int nct_link_data(nct_var*, nct_var*);

/* Whether data has to be loaded separately depends on the used readflags: nct_rlazy, nct_rcoord etc.
 * To load data without conversion:
 *	nct_load(var);
 * To load and convert data to short, see netcdf.h for other data types:
 *	nct_load_as(var, NC_SHORT);
 */
#define nct_load(var) nct_load_as(var, NC_NAT)
#define nct_loadg(set, name) nct_load_as(nct_get_var(set, name), NC_NAT)
#define nct_loadg_as(set, name, type) nct_load_as(nct_get_var(set, name), type)
nct_var* nct_load_as(nct_var*, nc_type);

struct tm* nct_localtime(long timevalue, nct_anyd epoch);

nct_anyd nct_mktime(const nct_var* var, struct tm* tm, nct_anyd* epoch, size_t ind);

/*
 * See also nct_localtime and nct_mktime.
 * Arguments:
 *	var: a variable which must contain attribute "units" with form of "$units since $epoch"
 *		$units is seconds, days, etc.
 *		$epoch has the form of yyyy-mm-dd [hhmmss]
 *	tm: This will become (struct tm)$epoch. Can be NULL.
 * Returns:
 *	ret.d = Enumeration of the recognized time unit or -1 on error.
 *	ret.a.t = unix time of $epoch.
 */
nct_anyd nct_mktime0(const nct_var* var, struct tm* tm);
#define nct_mktime0g(set, name, tm) nct_mktime0(nct_get_var(set, name), tm)
nct_anyd nct_mktime0_nofail(const nct_var* var, struct tm* tm); // Failing is not an error in this function.

void nct_print_var(const nct_var*, const char* indent);
void nct_print_dim(const nct_var*, const char* indent);
void nct_print(const nct_set*);

nct_var* nct_put_interval(nct_var* var, double i0, double gap);

char*			nct_range_NC_BYTE  (char i0, char i1, char gap);
unsigned char*		nct_range_NC_UBYTE (unsigned char i0, unsigned char i1, unsigned char gap);
short*			nct_range_NC_SHORT (short i0, short i1, short gap);
unsigned short*		nct_range_NC_USHORT(unsigned short i0, unsigned short i1, unsigned short gap);
int*			nct_range_NC_INT   (int i0, int i1, int gap);
unsigned*		nct_range_NC_UINT  (unsigned i0, unsigned i1, unsigned gap);
long long*		nct_range_NC_INT64 (long long i0, long long i1, long long gap);
unsigned long long*	nct_range_NC_UINT64(unsigned long long i0, unsigned long long i1, unsigned long long gap);
double*			nct_range_NC_DOUBLE(double i0, double i1, double gap);
float*			nct_range_NC_FLOAT (float i0, float i1, float gap);

/* How nct_read_nc behaves:
 * nct_rlazy:
 *	Data is not loaded nor memory allocated.
 *	Data must be loaded with nct_load or similar function.
 *      File is left open for reading, ncid stays valid, nct_free will close the file.
 * nct_rcoord:
 *	Like nct_rlazy but coordinate variables are loaded.
 *	nct_rlazy | nct_rcoord is undefined behaviour.
 * default:
 *      Everything is read at once.
 *      File is closed, ncid becomes invalid.
 *
 * nct_ratt:
 *	Read attributes.
 * default:
 *	Ignore attributes.
 */
enum {nct_rlazy=1<<0, nct_ratt=1<<1, nct_rcoord=1<<2,};
extern int nct_readflags;

/* Read data from netcdf. See also nct_read_ncf.
 * varname:
 *	Variable name or NULL:
 *	If NULL, reads the first noncoordinate variable.
 * type:
 *	See netcdf.h for options.
 *	If NC_NAT (=0), no conversion is done, otherwise data is converted to the type.
 */
#define nct_read_from_nc(a, b) nct_read_from_nc_as(a, b, NC_NAT)
void* nct_read_from_nc_as(const char* filename, const char* varname, nc_type type);

/* Reading netcdf files. See also nct_read_from_nc_as.
 * To allocate nct_set* into heap:
 *	nct_set* set = nct_read_nc("some.nc");
 * To allocate nct_set into stack (child objects will go to heap anyway):
 *	nct_set set;
 *      nct_read_nc_gd(&set, "some.nc");
 *  --equals--
 *	nct_readm_nc(set, "some.nc");
 */
#define  nct_read_nc(name) nct_read_ncf(name, nct_readflags)
#define  nct_read_nc_gd(set, name) nct_read_ncf_gd(set, name, nct_readflags)
#define  nct_readm_ncf(set, name, flags) nct_set set; nct_read_ncf_gd(&set, name, flags)
#define  nct_readm_nc(set, name) nct_readm_ncf(set, name, nct_readflags)
nct_set* nct_read_ncf(const char*, int flags);
nct_set* nct_read_ncf_gd(nct_set*, const char*, int flags);

nct_set* nct_read_mfnc_regex(const char* filename_regex, int regex_cflags, char* concatdim);
/* See nct__get_filenames. */
nct_set* nct_read_mfnc_regex_(const char* filename, int regex_cflags, char* concatdim,
	void (*matchfun)(const char* restrict, int, regmatch_t*, void*), int size1, int nmatch, void** matchdest);
/* filenames must be in the form of the result of nct__get_filenames. */
nct_set* nct_read_mfnc_ptr(const char* filenames, int n, char* concatdim);
/* If n is negative, then filenames must be null-terminated.
   This calls nct_read_mfnc_ptr after having converted filenames into form of its argument. */
nct_set* nct_read_mfnc_ptrptr(char** filenames, int n, char* concatdim);

nct_var* nct_rename(nct_var*, char*, int freeable);

nct_var* nct_rewind(nct_var* var); // back to start if nct_set_start has moved data pointer away from start

/* Should be used before loading the variables.
 * The coordinate can be loaded before these functions.
 */
nct_var* nct_set_length(nct_var* coord, size_t length);
nct_var* nct_set_start(nct_var* coord, size_t offset);

/* Untested!
   time0(var1) - time0(var0) in their time unit.
   Both must have the same time unit. */
time_t nct_timediff(const nct_var* var1, const nct_var* var0);

/* This frees the data unless used by another variable (see nct_copy_var) or flagged as not_freeable.
 * Can be used to limit RAM usage:
 *      nct_readmf(set, "some.nc", nct_rlazy);
 *	nct_foreach(set, var) {
 *		nct_load(var);
 *		do_stuff(var);
 *		nct_unlink_data(var);
 *	}
 *	nct_free1(&set);
 *
 *  --same functionality but larger RAM usage below--
 *
 *	nct_readm(set, "some.nc");
 *	nct_foreach(set, var)
 *		do_stuff(var);
 *	nct_free1(&set);
 */
void nct_unlink_data(nct_var*);

void nct_write_nc(const nct_set*, const char*);

/* Data functions:
   When data is accessed, a different function for each variable type is needed.
   These check the type (nct_var->dtype) and call the right function
   which is autogenerated from functions.in.c.
   User only has to tell whether the desired return value is floating point or integer. */
void      nct_print_data(const nct_var*);
double    nct_get_floating(const nct_var*, size_t ind);		// returns var->data[ind]
long long nct_get_integer(const nct_var*, size_t ind);		// returns var->data[ind]
double    nct_get_floating_last(const nct_var*, size_t ind);	// returns var->data[var->len-ind]
long long nct_get_integer_last(const nct_var*, size_t ind);	// returns var->data[var->len-ind]
double    nct_max_floating(const nct_var*);
long long nct_max_integer(const nct_var*);
nct_anyd  nct_max_anyd(const nct_var*);				// returns (nct_anyd){.d=argmax, .type=max}
double    nct_min_floating(const nct_var*);
long long nct_min_integer(const nct_var*);
nct_anyd  nct_min_anyd(const nct_var*);				// returns (nct_anyd){.d=argmin, .type=min}
void*     nct_minmax(const nct_var*, void* result);		// returns result which has to be initialized
nct_var*  nct_mean_first(nct_var*);
nct_var*  nct_meannan_first(nct_var*);


/* These functions work independently from this library (except using the error handling) but are needed in some functions. */

/* Returns existing filenames that match a regular expression.
 * regex:
 *	A regular expression.
 *	In regex = "dir/file", dir is matched literally and file as a regular expression but preceeded with ^ (start of line).
 * regex_cflags:
 *	See cflags in man regex. If unsure, say 0. If things don't work, say REGEX_EXTENDED.
 *
 * Rest of the arguments are for extracting data from the matched filenames.
 * fun:
 *	arg0 (const char* restrict):	A filename which matched regex.
 *	arg1 (int): 			number of found matches before this one. i.e. a growing number from zero.
 *	arg2 (regmatch_t*): 		Result from regexec. See man regex for details.
 *	arg3 (void*):			An array whither fun can write some data.
 * size1: 	how much space should be allocated per filename to be used in fun as arg3
 * nmatch:	how many matches (regmatch_t) from one filename.
 *		First match the whole matched expression and then capture groups i.e. \([0-9]*\).
 * dest (out):	pointer to the array where data was written in fun. Space is allocated into heap in this function.
 *
 * Returns:
 * 	Matched filenames in form of "name1\0name2\0name3\0last_name\0\0"
 * 	If name == NULL, returns numbers of names matched on previous call as intptr_t.
 */
char* nct__get_filenames(const char* restrict regex, int regex_cflags);
char* nct__get_filenames_(
	const char* restrict regex,
	int regex_cflags,
	void (*fun)(const char* restrict, int, regmatch_t*, void*),
	int size1,
	int nmatch,
	void** dest
	);
#define nct__getn_filenames() ((intptr_t)nct__get_filenames(NULL, 0))
#define nct__forstr(names, str) for(char* str=((char*)names); *str; str+=strlen(str)+1) // iterating over the result of nct__get_filenames

/* src has the form of result of nct__get_filenames.
   n is optional: if -1 if given, n is calculated.
   other is an optional array which will be sorted like src, if given. */
char* nct__sort_str(char* dest, const char* restrict src, int n, void* other, int size1other);

#endif
