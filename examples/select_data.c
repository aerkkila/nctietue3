/* limits latitudes above 50°
 * and longitudes between -100° and 100°
 */

#include <nctietue3.h>

int main() {
    nct_var* var;
    /* Variables should not be loaded before set_length / set_start, hence nct_rlazy. */
    nct_set* set = nct_read_ncf("example.nc", nct_rlazy);

    var = nct_get_var(set, "lat");
    nct_set_start(var, nct_find_sorted(var, 50));

    var = nct_get_var(set, "lon");
    int start = nct_find_sorted(var, -100);
    int end = nct_find_sorted(var, 100); // do this before calling nct_set_start
    nct_set_start(var, start);
    nct_set_length(var, end-start);

    nct_write_nc(set, "select_data.nc");
    nct_free1(set);
}
