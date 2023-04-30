/* limits latitudes above 50°
 * and longitudes between -100° and 100°
 * example.nc can be created for example with create_netcdf_file.c
 */

#include <nctietue3.h>

int main() {
    nct_var* var;
    /* Variables should not be loaded before set_length / set_start, hence nct_rcoord.
       It only loads coordinate data but not variable data. */
    nct_set* set = nct_read_ncf("example.nc", nct_rcoord);

    /* nct_find_sorted returns the first index where data[index] is above the argument.
       var->data must be sorted. */
    var = nct_get_var(set, "lat");
    nct_set_start(var, nct_find_sorted(var, 50));

    var = nct_get_var(set, "lon");
    int start = nct_find_sorted(var, -100);
    int end = nct_find_sorted(var, 100); // do this before calling nct_set_start
    nct_set_start(var, start);
    nct_set_length(var, end-start);

    /* Now the data can be loaded. */
    nct_foreach(set, var)
	nct_load(var);
    nct_print(set); // to see what we did
    nct_write_nc(set, "selected_region.nc");
    nct_free1(set);
}
