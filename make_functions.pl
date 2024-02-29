#!/bin/env perl
use warnings;
@enumtypes  = ('NC_BYTE', 'NC_UBYTE', 'NC_CHAR', 'NC_SHORT', 'NC_USHORT', 'NC_INT', 'NC_UINT',
	     'NC_INT64', 'NC_UINT64', 'NC_FLOAT', 'NC_DOUBLE');
@formats  = ('hhi', 'hhu', 'c', 'hi', 'hu', 'i', 'u', 'lli', 'llu', 'f', 'lf');
@ctypes   = ('char', 'unsigned char', 'char', 'short', 'unsigned short', 'int', 'unsigned',
             'long long', 'long long unsigned', 'float', 'double');
#@uctypes  = ('unsigned char', 'unsigned char', 'unsigned char', 'unsigned short', 'unsigned short', 'unsigned', 'unsigned',
#             'long long unsigned', 'long long unsigned', 'float', 'double');

sub make_wrapper_function {
    ($type, $name, $args) = @_;

    # create a function array
    $ntypes = @enumtypes;
    print file_out "static $type (*__$name"."[])($args) = {\n";
    for(my $j=0; $j<$ntypes; $j++) {
	print file_out "    [$enumtypes[$j]] = $name"."_$enumtypes[$j],\n"; }
    print file_out "};\n";

    # create a wrapper function
    print file_out "$type $name("; # now: type foo(
    my @arg = split(/,/, $args);
    my $nargs = @arg;
    my $nargs1 = $nargs - 1;
    our $hasvar = $nargs && ($arg[0] =~ /^([^,]*\s+)?(nct_var)|(nct_att)\*/);
    if ($hasvar) {
        for(my $i=0; $i<$nargs-1; $i++) {
            print file_out "$arg[$i] _$i, "; }
        print file_out "$arg[$nargs1] _$nargs1) {\n"; # now: type foo(type0 _0, type1 _1, type_n, _n) {
        print file_out "    return __$name"."[_0->dtype](";
        for($i=0; $i<$nargs-1; $i++) {
            print file_out "_$i, "; }
        print file_out "_$nargs1);\n}\n";
    }
    else {
        print file_out "nc_type nctype";
        for(my $i=0; $i<$nargs-1; $i++) {
            print file_out ", $arg[$i] _$i"; }
	if ($nargs) {
	    print file_out ", $arg[$nargs1] _$nargs1"; }
        print file_out ") {\n"; # now: type foo(type0 _0, type1 _1, type_n, _n) {
        print file_out "    return __$name"."[nctype](";
        if ($nargs) {
            print file_out "_0"; }
        for(my $i=1; $i<$nargs; $i++) {
            print file_out ", _$i"; }
        print file_out ");\n}\n";
    }
}

sub main {
    open file_in, "<functions.in.c" or die;
    open file_out, ">functions.c";

    # Copy only once until @startperl.
    $line = <file_in>;
    while (not $line =~ /^\@startperl\s*.*/) { #*
	print file_out $line;
	$line = <file_in>;
    }
    $startpos = tell file_in;

    for (my $i=0; $i<@enumtypes; $i++) {
	while (<file_in>) {
	    $_ =~ s/\@nctype/$enumtypes[$i]/g;
	    $_ =~ s/\@ctype/$ctypes[$i]/g;
	    $_ =~ s/\@form/$formats[$i]/g;
	    print file_out $_;
	}
	print file_out "\n";
	seek(file_in, $startpos, 0);
    }

    my $first = 1;

    while (<file_in>) {
	($type, $name, $args) = ($_ =~ /(\S+.*)\s+(\S+)_\@nctype*\((.*)\)\s*{/g); #+

	if (!$type || !$name) { next; }
	if (substr($name, 0, 1) =~ '_') { next; }
	if ($type =~ /^ctype\W*$/) { next; }

	# We later replace freely named arguments with _{number}.
	# This substitution omits argument names them and sets pointers (*) and commas (,) back
	$args =~ s/\s*(\**)\w+\s*(,|$)/$1$2/g;

	if (not $first) {
	    print file_out "\n"; }
	$first = 0;

	make_wrapper_function($type, $name, $args);
    }
    close file_in;
}

main();
