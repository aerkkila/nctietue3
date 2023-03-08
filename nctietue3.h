#ifndef __NCTIETUE__
#define __NCTIETUE__

#include <netcdf.h>
#include <time.h>
#include <stdio.h>
#include <stddef.h>

typedef struct nct_set nct_set;
typedef struct nct_var nct_var;
typedef struct nct_att nct_att;
typedef struct nct_anyd nct_anyd;
typedef union  nct_any nct_any;
typedef int (*nct_ncget_t)(int,int,void*);

/* Use this to redirect error messages elsewhere than to stderr:
 *	char errormsg[512];
 *	nct_stderr = fmemopen(errormsg, 512, "w");
 */
extern FILE* nct_stderr;

#define nct_puterror(...) do {	fprintf(nct_stderr? nct_stderr: stderr, "%sError%s (%s: %i):\n", nct_error_color, nct_default_color, __FILE__, __LINE__);	\
    				fprintf(nct_stderr? nct_stderr: stderr, "    " __VA_ARGS__); } while(0)
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

struct nct_att {
    char*    name;
    void*    value;
    nc_type  dtype;
    int      len;
    unsigned freeable;
};

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
    nc_type  dtype;
    int      not_freeable;
    unsigned char* nusers; // the first user is not counted
    void*    data;
};

struct nct_set {
    int       nvars, varcapacity;
    nct_var** vars;
    int       ndims, dimcapacity;
    nct_var** dims; // points to a variable with the same name if available
    int       natts, attcapacity;
    nct_att*  atts;
    int       ncid, owner;
};

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
    char* s;
    time_t t;
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

void nct_add_varatt_text(nct_var* var, char* name, char* value, unsigned freeable);
nct_var* nct_add_vardim_first(nct_var* var, int dimid);

nct_set* nct_concat(nct_set *vs0, nct_set *vs1, char* dimname, int howmany_left);

/* see nct_mktime0 */
nct_var* nct_convert_timeunits(nct_var* var, const char* units);

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

/* If name is not unique, uses nct_find_unique_name_from to replace it. */
nct_var* nct_ensure_unique_name(nct_var* var);
char* nct_find_unique_name_from(nct_set* set, const char* initname, int num);

nct_var* nct_firstvar(const nct_set*);
nct_var* nct_nextvar(const nct_var*);
nct_var* nct_prevvar(const nct_var*);
nct_var* nct_lastvar(const nct_set*);

#define nct_foreach(set, var) \
    for(nct_var* var=nct_firstvar(set); var; var=nct_nextvar(var))

#define nct_free(...) _nct_free(0, __VA_ARGS__, -1)
void _nct_free(int _, ...); // first argument is meaningless
void nct_free1(nct_set*);

nct_att* nct_get_varatt(const nct_var* var, const char* name);
char* nct_get_varatt_text(const nct_var*, const char*);
nct_var* nct_get_dim(const nct_set* set, const char* name);
nct_var* nct_get_var(const nct_set* set, const char* name);
int nct_get_varid(const nct_set* restrict, const char* restrict);
int nct_get_dimid(const nct_set* restrict, const char* restrict);
size_t nct_get_len_from(const nct_var*, int startdim);

int nct_link_data(nct_var*, nct_var*);

/* Data has to be loaded separately only if nct_set was read with nct_rlazy:
 * Simple usage:
 *	nct_load(var);
 * To load and convert data to short, see netcdf.h for other data types:
 *	nct_load_as(var, NC_SHORT);
 *
 * In macros nct_loadm and nct_loadm_as, two syntaxes are allowed:
 *	nct_loadm(nct_var* var)			OR	nct_loadm(nct_set* set, const char* varname)
 *	nct_loadm_as(nct_var* var, int flags)	OR	nct_loadm_as(nct_set* set, const char* varname, int flags)
 * Note, that var is evaluated multiple times in the macros.
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
 *
 * In the macro, argument var can also be nct_set* which contains variable "time".
 * Note, that var is evaluated multiple times in the macro.
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
 * default:
 *      Everything is read at once.
 *      File is closed, ncid becomes invalid.
 *
 * nct_ratt:
 *	Read attributes.
 * default:
 *	Ignore attributes.
 */
enum {nct_rlazy=1<<0, nct_ratt=1<<1};
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
#define  nct_readm_ncf(set, name, flags) nct_set set; nct_read_ncf_gd(&set, name, flags)
#define  nct_readm_nc(set, name) nct_readm_ncf(set, name, nct_readflags)
nct_set* nct_read_ncf(const char*, int flags);
nct_set* nct_read_ncf_gd(nct_set*, const char*, int flags);

nct_set* nct_read_mfnc_regex(const char* filename, int regex_cflags, char* concatdim);
nct_set* nct_read_mfnc_ptr(const char* filename, int n, char* concatdim);

nct_set* nct_read_mfnc_regex_gd(nct_set* s0, const char* filename, int regex_cflags, char* concatdim);
nct_set* nct_read_mfnc_ptr_gd(nct_set* vs0, const char* filenames, int nfiles, char* concatdim);

nct_var* nct_rename(nct_var*, char*, int freeable);

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
   These check the type (nct_var->dtype) and call the right function. */
void      nct_print_data(const nct_var*);
double    nct_get_floating(const nct_var*, size_t);
long long nct_get_integer(const nct_var*, size_t);
double    nct_get_floating_last(const nct_var*, size_t);
long long nct_get_integer_last(const nct_var*, size_t);
double    nct_max_floating(const nct_var*);
long long nct_max_integer(const nct_var*);
nct_anyd  nct_max_anyd(const nct_var*);
double    nct_min_floating(const nct_var*);
long long nct_min_integer(const nct_var*);
nct_anyd  nct_min_anyd(const nct_var*);
void*     nct_minmax(const nct_var*, void* result);
nct_var*  nct_mean_first(nct_var*);
nct_var*  nct_meannan_first(nct_var*);

/* These functions work independently from this library but are needed in some functions. */
char* nct__get_filenames(const char* restrict, int regex_cflags); // result is in form "name1\0name2\0name3\0last_name\0\0"
char* nct__sort_str(char* dest, const char* restrict src, int n); // separated by '\0' and ending with '\0\0'. n can be -1.

#endif
