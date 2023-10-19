#define MIN(a, b) ((a) <= (b) ? a : (b))
#define MAX(a, b) ((a) >= (b) ? a : (b))

typedef struct {
    /* constants after initialization */
    int size1;
    size_t ndata;
    nct_ncget_partial_t getfun;
    /* mutable variables */
    size_t *fstart, *fcount, start;
    /* restricted: only incremented at loading */
    size_t pos;
    void* data;
    int ifile; // only used for printing progress (nct_verbose)
} loadinfo_t;

static size_t get_fileind_from_coords(const nct_var* var, const size_t* coords) {
    size_t ind = 0;
    size_t cum = 1;
    for (int i=var->nfiledims-1; i>=0; i--) {
	ind += cum * coords[i];
	cum *= var->filedimensions[i];
    }
    return ind;
}

static void load_stream_partially(nct_var* var, loadinfo_t* info, const size_t* start, int cutdim, int nowdim, size_t* nloaded) {
    size_t mutstart[var->ndims];
    memcpy(mutstart, start, sizeof(mutstart));

    if (nowdim < cutdim) {
	for (int i=0; i<info->fcount[nowdim]; i++) {
	    load_stream_partially(var, info, mutstart, cutdim, nowdim+1, nloaded);
	    mutstart[nowdim]++;
	    /* count needs not to be changed along start. */
	}
	return;
    }
    else if (nowdim > cutdim) {
	nct_puterror("nowdim (%i) should be less or equal than cutdim (%i) in %s (%s).\n", nowdim, cutdim, __func__, __FILE__);
	nct_other_error;
    }

    long len = nct_get_len_from(var, cutdim+1);
    long pos = get_fileind_from_coords(var, start);
    long wants = len*info->fcount[cutdim];

    if (nct_verbose) {
	printf("    fseek %zu (%zu", pos, start[0]);
	for (int i=1; i<var->nfiledims; i++)
	    printf(" %zu", start[i]);
	printf("), fread %zu (%zu", wants, 0<cutdim ? 1 : info->fcount[0]);
	for (int i=1; i<var->nfiledims; i++)
	    printf(" %zu", i<cutdim ? 1 : info->fcount[i]);
	printf(")");
	verbose_line_ending();
    }

    long ret;
    FILE* stream = nct_get_stream(var);
    if ((ret = fseek(stream, pos*info->size1, SEEK_SET)))
	nct_puterror("fseek returned %li\n", ret);
    if ((ret = fread(info->data + *nloaded*info->size1, info->size1, wants, stream)) != wants)
	nct_puterror("fread returned %li instead of %zu in %s (%i)\n", ret, wants, __func__, __LINE__);
    *nloaded += wants;
}

static void load_stream(nct_var* var, loadinfo_t* info) {
    FILE* stream = nct_get_stream(var);
    size_t nread, nloaded=0;
    for (int idim=var->ndims-1; idim>=0; idim--)
	if (info->fcount[idim] < var->filedimensions[idim])
	    return load_stream_partially(var, info, info->fstart, idim, 0, &nloaded);
    if ((nread = fread(info->data, info->size1, var->len, stream)) != var->len)
	nct_puterror("fread returned %zu instead of %zu in %s (%i)\n", nread, var->len, __func__, __LINE__);
    return;
}

static long make_coordinates(size_t* arr, const size_t* dims, int ndims) {
    long carry = 0, num, dimlen = 1;
    for(int i=ndims-1; i>=0; i--) {
	num = arr[i]*dimlen + carry;
	long next_dimlen = dimlen * dims[i];
	long num_thisdim = num % next_dimlen;
	arr[i] = num_thisdim / dimlen;
	dimlen = next_dimlen;
	carry = num - num_thisdim;
    }
    return carry / dimlen;
}

/* How many data will be read from the file. */
static size_t get_read_length(const nct_var* var) {
    long length = 1;
    int ndims = var->nfiledims;
    for(int i=0; i<ndims; i++) {
	long omitted, omit0, omit1;
	long flen = var->filedimensions[ndims-i-1];
	const nct_var* dim = nct_get_vardim(var, var->ndims-i-1);
	omit0 = dim->rule[nct_r_start].arg.lli;
	if (omit0 >= flen)
	    omitted = flen;
	else {
	    omit1 = flen - (dim->len + omit0);
	    if (omit1 < 0)
		omit1 = 0;
	    omitted = omit0 + omit1;
	}
	length *= flen-omitted;
    }
    return length;
}

static int get_filenum(long start, nct_var* var, int* ifile_out, size_t* start_out) {
    if (!var->ndims) {
	if (start == 0)
	    return 0;
	else
	    goto error;
    }
    int ifile = 0, nfiles = var->rule[nct_r_concat].n + 1;
    long length = 0, length_diff = 0;
    long add = get_read_length(var);

    nct_var* dim0 = nct_get_vardim(var, 0);
    long carried_dimstart = dim0->rule[nct_r_start].arg.lli - var->filedimensions[0];
    carried_dimstart *= (carried_dimstart > 0);

    while (1) {
	long new_length = length + add;
	if (new_length > start) {
	    *start_out = start - length + length_diff; // where to start reading this file
	    *ifile_out = ifile;
	    return 0;
	}
	length = new_length;
	if (++ifile >= nfiles)
	    break;
	nct_var* cvar = from_concatlist(var, ifile-1);

	if (!cvar->ndims) {
	    length_diff = 0;
	    add = 1;
	    continue;
	}

	nct_var* dim0 = nct_get_vardim(cvar, 0);
	long old_start = dim0->rule[nct_r_start].arg.lli;
	long old_length = cvar->len;
	nct_set_start(dim0, old_start + carried_dimstart);
	length_diff = old_length - cvar->len;
	carried_dimstart = dim0->rule[nct_r_start].arg.lli - cvar->filedimensions[0];
	carried_dimstart *= (carried_dimstart > 0);
	if (carried_dimstart)
	    add = 0;
	else
	    add = cvar->len;
	nct_set_start(dim0, old_start);
    }
error:
    nct_puterror("Didn't find starting location (%li) from %s\n", start, nct_get_filename(var->super));
    return 1;
}

static void load_for_real(nct_var* var, loadinfo_t* info) {
    if (hasrule(var, nct_r_stream))
	return load_stream(var, info);
    perhaps_open_the_file(var);
    ncfunk(info->getfun, var->super->ncid, var->ncid, info->fstart, info->fcount, info->data);
    perhaps_close_the_file(var->super);
}

static size_t limit_rectangle(size_t* rect, int ndims, size_t maxsize) {
    int idim = ndims - 1;
    size_t len = 1;
    for(; idim>=0; idim--) {
	size_t try = len * rect[idim];
	if (try > maxsize)
	    goto Break;
	len = try;
    }
    return len;
Break:
    for(int i=0; i<idim; i++)
	rect[i] = 1;
    size_t n_idim = maxsize / len;
    rect[idim] = n_idim;
    len *= n_idim;
    return len;
}

/* fstart, fcount */
static size_t set_info(const nct_var* var, loadinfo_t* info, size_t startpos) {
    int n_extra = var->ndims - var->nfiledims;
    /* fstart and fcount omitting startpos */
    if (var->nfiledims == 0) {
	info->fstart[0] = 0;
	info->fcount[0] = 1;
	return 1;
    }
    for(int i=var->nfiledims-1; i>=0; i--) {
	nct_var* dim = nct_get_vardim(var, i+n_extra);
	info->fstart[i] = dim->rule[nct_r_start].arg.lli;
	info->fcount[i] = MIN(var->filedimensions[i] - info->fstart[i], dim->len);
    }
    /* correct fstart and fcount with startpos */
    size_t move[nct_maxdims] = {0};
    /* startpos is the index in the flattened array
       move is the index vector in the file array */
    move[var->ndims-1] = startpos;
    size_t result;
    if ((result = make_coordinates(move, info->fcount, var->nfiledims)))
	nct_puterror("Overflow in make_coordinates: %zu, %s: %s\n", result, nct_get_filename(var->super), var->name);
    for(int i=var->nfiledims-1; i>=0; i--) {
	if (move[i]) {
	    info->fstart[i] += move[i];
	    info->fcount[i] -= move[i];
	    /* We can only read rectangles, and hence, we must stop at the first such dimension from behind,
	       where startpos is in the middle of the used area. */
	    for(int j=0; j<i; j++)
		info->fcount[j] = 1;
	    break;
	}
    }
    /* Make sure not to read more than asked. */
    size_t len = limit_rectangle(info->fcount, var->nfiledims, info->ndata - info->pos);
    return len;
}

static void print_progress(const nct_var* var, const loadinfo_t* info, size_t len) {
    printf("Loading %zu %% (%zu / %zu); n=%zu; start={%li",
	    (info->pos+len)*100/info->ndata, info->pos+len, info->ndata, len, info->fstart[0]);
    for(int i=1; i<var->nfiledims; i++)
	printf(", %li", info->fstart[i]);
    printf("}; count={%li", info->fcount[0]);
    for(int i=1; i<var->nfiledims; i++)
	printf(", %li", info->fcount[i]);
    printf("}; from %i: %s", info->ifile, nct_get_filename(var->super));
    verbose_line_ending();
}

static int next_load(nct_var* var, loadinfo_t* info) {
    size_t start_thisfile;
    int filenum;
    if (info->pos >= info->ndata)
	return 2; // All data are ready.
    if (info->start >= var->len)
	return 1; // This virtual file is ready.
    if (get_filenum(info->start, var, &filenum, &start_thisfile))
	return -1; // an error which shouldn't happen
    if (filenum == 0) {
	size_t len = set_info(var, info, start_thisfile);
	if (nct_verbose)
	    print_progress(var, info, len);
	load_for_real(var, info);
	if (nct_after_load)
	    nct_after_load(var, info->data, len, info->fstart, info->fcount);
	info->data += len * info->size1;
	info->pos += len;
	info->start += len;
	info->ifile++;
	return 0;
    }
    /* If this wasn't the first file, we call this function again to handle
       concatenation rules correctly on this file. */
    nct_var* var1 = from_concatlist(var, filenum-1);
    long old_start = info->start;
    info->start = start_thisfile;
    while(!next_load(var1, info));
    info->start += old_start - start_thisfile;
    return 0;
}

static nct_var* load_coordinate_var(nct_var* var) {
    nct_ncget_t getfulldata = nct_getfun[var->dtype];
    int size1 = nctypelen(var->dtype);
    size_t len = var->filedimensions[0];
    if (!var->data) {
	var->data = malloc(len*size1);
	var->capacity = len;
    }
    else if (var->capacity < len) {
	var->data -= var->rule[nct_r_start].arg.lli * size1;
	var->data = realloc(var->data, len*size1);
	var->capacity = len;
    }
    perhaps_open_the_file(var);
    ncfunk(getfulldata, var->super->ncid, var->ncid, var->data);
    perhaps_close_the_file(var->super);
    var->startpos = 0;
    var->endpos = var->len;
    var->data += var->rule[nct_r_start].arg.lli * size1;
    return var;
}

nct_var* nct_load_partially_as(nct_var* var, long start, long end, nc_type dtype) {
    if (!nct_loadable(var))
	return NULL;
    size_t fstart[nct_maxdims], fcount[nct_maxdims]; // start and count in a real file
    if ((var->dtype == NC_CHAR) + (dtype == NC_CHAR) + 2*(dtype == NC_NAT) == 1) {
	nct_puterror("Cannot convert to or from NC_CHAR. Variable %s%s%s in %s%s%s\n",
		nct_varname_color, var->name, nct_default_color, nct_varset_color, nct_get_filename(var->super), nct_default_color);
	nct_return_error(NULL);
    }
    if (dtype != NC_NAT) {
	if (var->dtype)
	    var->capacity *= nctypelen(dtype) / nctypelen(var->dtype);
	var->dtype = dtype;
    }
    if (nct_iscoord(var))
	return load_coordinate_var(var);
    size_t old_length = var->len;
    var->len = end-start;
    nct_allocate_varmem(var);
    var->len = old_length;
    loadinfo_t info = {
	.size1	= nctypelen(var->dtype),
	.fstart	= fstart,
	.fcount	= fcount,
	.ndata	= end - start,
	.data	= var->data,
	.getfun	= nct_getfun_partial[var->dtype],
	.start	= start,
    };
    while (!next_load(var, &info));
    if (info.ndata != info.pos)
	nct_puterror("%s: Loaded %zu instead of %zu\n", var->name, info.pos, info.ndata);
    var->startpos = start;
    var->endpos = end;
    return var;
}

nct_var* nct_load_as(nct_var* var, nc_type dtype) {
    return nct_load_partially_as(var, 0, var->len, dtype);
}

/* Uses private nct_set.fileinfo. */
int nct_loadable(const nct_var* var) {
    return
	(var->ncid>=0 && ((var)->super->ncid > 0 || var->super->fileinfo)) ||
	hasrule(var, nct_r_stream);
}

#undef MIN
