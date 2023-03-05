#!/bin/env perl
# In case you wander, what this does, run this and check the created file function_wrappers.c.

@nctypes  = ('NC_BYTE', 'NC_UBYTE', 'NC_CHAR', 'NC_SHORT', 'NC_USHORT', 'NC_INT', 'NC_UINT',
	     'NC_INT64', 'NC_UINT64', 'NC_FLOAT', 'NC_DOUBLE');
$ntypes = @nctypes;

$functions = '
void      nct_print_data_@nctype(const nct_var*);
nct_var*  nct_put_interval_@nctype(nct_var*, double, double);
double    nct_get_floating_@nctype(const nct_var*, size_t);
long long nct_get_integer_@nctype(const nct_var*, size_t);
double    nct_get_floating_last_@nctype(const nct_var*, size_t);
long long nct_get_integer_last_@nctype(const nct_var*, size_t);
double    nct_max_floating_@nctype(const nct_var*);
long long nct_max_integer_@nctype(const nct_var*);
nct_anyd  nct_max_anyd_@nctype(const nct_var*);
double    nct_min_floating_@nctype(const nct_var*);
long long nct_min_integer_@nctype(const nct_var*);
nct_anyd  nct_min_anyd_@nctype(const nct_var*);
void*     nct_minmax_@nctype(const nct_var*, void*);
nct_var*  nct_mean_first_@nctype(nct_var*);
nct_var*  nct_meannan_first_@nctype(nct_var*);
';
@funs = split("\n", substr $functions, 1);

open out, ">function_wrappers.c";
foreach(@funs) {
    # split the prototype into type, name, args
    $_ =~ /(.+[^ ]) +(.+)_\@nctype\((.+)\);/;
    $type = $1; $name = $2; $args = $3;

    # create a function array
    print out "static $type (*__$name"."[])($args) = {\n";
    for($j=0; $j<$ntypes; $j++) {
	print out "    [@nctypes[$j]] = $name"."_@nctypes[$j],\n"; }
    print out "};\n";

    # create a wrapper function
    print out "$type $name(";
    @arg = split(/,/, $args);
    $len = @arg;
    $len--;
    for($i=0; $i<$len; $i++) {
	print out "@arg[$i] _$i, "; }
    print out "@arg[$len] _$len) {\n";
    print out "    return __$name"."[_0->dtype](";
    for($i=0; $i<$len; $i++) {
	print out "_$i, "; }
    print out "_$len);\n}\n\n";
}

close in;
close out;
