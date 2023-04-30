#include <nctietue3.h>
#include <stdlib.h>

/* Creating data to write to netcdf files. */
int* make_some_data(int t, int y, int x, int (*fun)(int,int,int)) {
    int* data = malloc(t*y*x*sizeof(int));
    for(int k=0; k<t; k++)
	for(int j=0; j<y; j++)
	    for(int i=0; i<x; i++)
		data[k*y*x + j*x + i] = fun(k, j, i);
    return data;
}
/* These are passed as an argument to the function above. */
int fun1(int k, int j, int i) { return (k*k + j*200) % (i+1); }
int fun2(int k, int j, int i) { int a = fun1(k,j,i); return a*i+j*100; }

int main() {
    /* Creating a simple netcdf file with automatic variable and dimension names
       and without coordinate variables assosiated with the dimensions. */
    {
	int* data = make_some_data(30, 400, 600, fun1);
	nct_set* set = nct_create_simple(data, NC_INT, 30, 400, 600);
	nct_write_nc(set, "simple_file.nc");
	nct_free1(set); // Don't call free(data) separately. This frees the data.
    }

    /* Creating a more serious netcdf file. */
    int xlen = 300,
	ylen = 150,
	tlen = 40;
    int* data1 = make_some_data(1, xlen, ylen, fun1);
    int* data2 = make_some_data(tlen, xlen, ylen, fun2);
    /* In an empty nct_set all fields are zeros. Initializing a file is hence easy: */
    nct_set set = {0};
    /* Create the time dimension. */
    nct_var* timecoord = nct_add_dim(&set, tlen, "time");
    /* Similarily create x- and y- dimensions. */
    nct_var* ycoord = nct_add_dim(&set, ylen, "lat");
    nct_var* xcoord = nct_add_dim(&set, xlen, "lon");
    /* Now we can add the variables. */
    int dimids[] = {1,2};	// data1 does not have time coordinate (length is 1)
    				// Dimensions get ids in the order in which they are added:
    				// 	time has id 0, y has 1 and x has 2
    				// dimids therefore tell that we are using the y and x dimension
    nct_add_var(&set, data1, NC_INT, "data1", 2, dimids); // 2 tells how many dimensions the variable has
    /* We could add data2 similarily by creating:
       		int dimids2[] = {0,1,2};
       but since we are using all the dimensions in the order from first to last,
       there is a shortcut: */
    nct_add_var_alldims(&set, data2, NC_INT, "data2");

    /* Let's say that the frames in data2 are observations with 5 days interval starting from 5th of July 2005. */
    nct_add_varatt_text(timecoord, "units", "days since 2005-07-05", 0); // last argument tells that neither string is freeable
    /* There is only one data type for all kinds of variables: nct_var.
     * It (nct_var) has however three different meanings:
     * 	1. A dimension:		Has only name, length and possibly attributes.
     *	2. A variable:		Has data and must be linked to dimensions to determine its shape and length.
     *	3. A coordinate: 	Is a dimension which has data. Unlike a variable it cannot be linked to other dimensions.
     *
     * To turn time interval into 5 days instead of 1,
     * we must include data {0, 5, 10, 15, ...} to tell how many days have passed since epoch.
     * First convert the dimension into a coordinate to be able to add data. */
    nct_dim2coord(timecoord, NULL, NC_INT); // NULL, since we don't yet have the data, NC_INT tells the data type
    /* We could create the data ourselves and add it to timecoord->data.
       There is however a shortcut: */
    nct_put_interval(timecoord, 0, 5); // start is 0, interval is 5

    /* Similarily add latitudes from 10°S to 80°N into ycoord.
       Let's create the data ourselves this time for example purposes. */
    float latdata[xlen];
    float yinterval = (80.0 - -10.0) / ylen;
    for(int i=0; i<ylen; i++)
	latdata[i] = -10 + i*yinterval;
    nct_dim2coord(ycoord, latdata, NC_FLOAT) -> not_freeable = 1;	// not freeable since latdata is in stack
    									// by default nct_free1 would try to free latdata
    /* Lastly add longitudes into xcoord. */
    float xinterval = (180.0 - -180.0) / xlen;
    nct_dim2coord(xcoord, NULL, NC_FLOAT);
    nct_put_interval(xcoord, -180, xinterval);

    /* The file is ready be written. */
    nct_write_nc(&set, "example.nc");
    nct_print(&set); // To see what we did.
    nct_free1(&set);
}
