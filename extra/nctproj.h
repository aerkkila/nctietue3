typedef struct {
    size_t new_xlen, new_ylen;		// currently, only out
    double new_xdiff, new_ydiff;	// currently, only out
    double new_xlim[2], new_ylim[2];	// currently, only out
    int unequal_xydiff;			// only in
} nctproj_args_t;

/* To and from are coordinate system descriptions which the proj-library recognizes.
 * For help, see "man proj" and these examples:
 * (x,y) in ease2-projection (lambert azimutal equal area), where the north pole is the center:
 *	"+proj=laea +lat_0=90"
 * (longitude, latitude) in degrees:
 *	"+proj=longlat"
 */
FILE* nctproj_open_converted(const nct_var* var, const char* from, const char* to, nctproj_args_t*);

/* Like nctproj_open_converted above but additionally creates a new variable whose data comes from the stream.
   Data is loaded by nct_load_stream but also the FILE can be accessed with nct_get_stream.
   Use of nct_load with this variable is undefined.
   The stream will be closed when nct_free_var is called. */
nct_var* nctproj_open_converted_var(const nct_var* var, const char* from, const char* to, nctproj_args_t*);

/* If geod_a <= 0, WGS84 is used as the ellipsoid.
   If lon is NULL, 1° is used as londiff. */
double* nctproj_get_areas_lat_regular(const nct_var* lat, const nct_var *lon, double geod_a, double geod_rf)
    __attribute__((malloc));

double* nctproj__get_areas_lat_regular(double lat0_lower, double latdiff, long n, double londiff, double *out, double geod_a, double geod_rf);
