#include <nctietue3.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int opt, brief = 0;
    char *varname = NULL;
    while ((opt = getopt(argc, argv, "bv:")) >= 0)
	switch (opt) {
	    case 'b': brief = 1; break;
	    case 'v': varname = optarg; break;
	}
    if (optind >= argc)
	return -1;
    nct_set *set = nct_read_ncf(argv[optind], nct_rcoord);

    if (varname) {
	nct_var *var = nct_get_var(set, varname);
	if (var) {
	    if (brief)
		nct_print_var_meta(var, "");
	    else
		nct_print_var(var, "");
	}
	else
	    nct_print_meta(set);
    }
    else if (brief)
	nct_print_meta(set);
    else
	nct_print(set);

    nct_free1(set);
}
