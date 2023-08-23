#include <nctietue3.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Returns true if yr is a leap year in the gregorian calendar. */
int leap_year(int yr) { return !(yr%4) && (yr%100 || !(yr%400)); }

/* Creates some random data which pretends to be daily observations starting from 1st of January 2005.
   Each year goes into its own file which will be named as multifile_yyyy.nc. */
void create_the_files() {
    int xlen = 150,
	ylen = 100,
	nfiles = 10,
	year0 = 2005;
    int len = xlen*ylen*366;
    short* data = malloc(len*sizeof(short));
    srand(1);
    char str[40];
    for(int n=0; n<nfiles; n++) {
	for(int i=0; i<len; i++)
	    data[i] = rand() % ((i % 50+n*5) + 1);
	nct_set* set = nct_create_simple(data, NC_SHORT, 365+leap_year(n+2005), ylen, xlen);
	/* Attributes cannot be linked to dimensions so convert into coordinates.
	   This is a restriction of the netcdf library and not of this library. */
	nct_var* var = nct_dim2coord(set->dims[0], NULL, NC_INT);
	nct_put_interval(var, 0, 1);
	sprintf(str, "days since %i-01-01", n+year0);
	/* str is overwritten later, hence it must be copied when used as an attribute.
	   The last argument tells that the value of the attribute is freeable since we used strdup. */
	nct_add_varatt_text(set->dims[0], "units", strdup(str), 1);
	nct_firstvar(set)->not_freeable = 1; // data is freed manually after each file has been written
	sprintf(str, "multifile_%i.nc", n+year0);
	nct_write_nc(set, str);
	nct_free1(set);
    }
    free(data);
}

int main() {
    create_the_files();
    /* All previously written files can be read as if they were one file.
       Names are matched using a regular expression.
       The second argument is regex_flags. See posix regex documentation for details.
       The last argument tells to concatenate along the first dimension, index 0. For other options, see nct_concat.
       Time variable is automatically converted into correct values.
       That is possible since we added the attribute "units": "days since yyyy-01-01", which this library recognizes. */
    nct_set* set = nct_read_mfnc_regex("multifile_[0-9]*\\.nc", 0, "-0");
    nct_print(set);
    nct_write_nc(set, "multifile_combined.nc");
    nct_free1(set);

    /* A region can be selected from multifile set as follows. */
    nct_readflags = nct_rcoord; // Do not load the data yet.
    set = nct_read_mfnc_regex("multifile_[0-9]*\\.nc", 0, "-0");
    /* The region must be selected individually from each file using a loop.
       Concatenation is variable-wise, not dataset-wise, hence we must pick a variable.
       This will affect all the other variables as well if they share the same dimensions. */
    nct_var* var = nct_firstvar(set);
    nct_for_concatlist(var, concatvar) {
	nct_set_start(nct_get_vardim(concatvar, concatvar->ndims-1), 80);
	nct_set_length(nct_get_vardim(concatvar, concatvar->ndims-2), 30);
    }
    /* The first dimension is used in concatenation in this case.
       If we would like to omit n days from each year, we would need a loop as above.
       If we want to omit 500 days from the start, we can just do it as follows,
       even if a single file is not 500 days long. */
    nct_set_start(nct_get_vardim(var, 0), 500);
    /* Now we can load the data */
    nct_verbose = nct_verbose_newline; // This helps to see that the correct data are loaded.
    nct_load(var);
    nct_print(set);
    nct_write_nc(set, "multifile_regions_combined.nc");
    nct_free1(set);
}
