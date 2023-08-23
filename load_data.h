#define MIN(a, b) ((a) <= (b) ? a : (b))
#define MAX(a, b) ((a) >= (b) ? a : (b))

typedef struct {
    /* constants after initialization */
    int size1;
    size_t ndata;
    nct_ncget_partial_t getfun;
    /* mutable variables */
    size_t *fstart, *fcount, *f_used, varstart, filestart;
    /* restricted: only incremented at loading */
    size_t pos;
    void* data;
    int ifile; // only used for printing progress (nct_verbose)
} loadinfo_t;

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

static nct_var* get_var_from_filenum(nct_var* var, int num) {
    if (num)
	return from_concatlist(var, num-1);
    return var;
}

static size_t get_filelen(const nct_var* var) {
    size_t len = 1;
    for(int i=var->nfiledims-1; i>=0; i--)
	len *= var->filedimensions[i];
    return len;
}

static size_t var_offset(const nct_var* var) { // only in the first dimension
    size_t length = 1;
    for(int i=var->ndims-1; i>0; i--)
	length *= nct_get_vardim(var, i)->len;
    return nct_get_vardim(var, 0)->rule[nct_r_start].arg.lli * length;
}

static int get_filenum(long start, nct_var* var, int* ifile_out, size_t* start_out) {
    int f = 0, nfiles = var->rule[nct_r_concat].n + 1;
    long length = 0; // must be signed if offset > filelen
    while (f < nfiles) {
	long add;
	if (f) {
	    nct_var* varnow = get_var_from_filenum(var, f);
	    long try1 = varnow->len,
		 try2 = get_filelen(varnow);
	    add = MAX(try1, try2);
	}
	else
	    add = get_filelen(var) - var_offset(var);
	long new_length = length + add;
	if (new_length > start) {
	    *start_out = start - length; // where to start reading this file
	    *ifile_out = f;
	    return 0;
	}
	length = new_length;
	f++;
    }
    nct_puterror("Didn't find starting location (%li) from %s\n", start, var->super->filename);
    return 1;
}

static void load_for_real(nct_var* var, loadinfo_t* info) {
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

/* fstart, fcount, f_used
   f_used tells how many data we move forward in the file at the load function.
   It can be larger than fcount because we can omit data on the way. */
static size_t set_info(const nct_var* var, loadinfo_t* info, size_t startpos) {
    int n_extra = var->ndims - var->nfiledims;
    /* fstart and fcount omitting startpos */
    for(int i=var->nfiledims-1; i>=0; i--) {
	nct_var* dim = nct_get_vardim(var, i+n_extra);
	info->fstart[i] = dim->rule[nct_r_start].arg.lli;
	info->fcount[i] = MIN(var->filedimensions[i] - info->fstart[i], dim->len);
	info->f_used[i] = var->filedimensions[i];
    }
    /* correct fstart and fcount with startpos */
    size_t move[nct_maxdims] = {0};
    /* startpos is the index in the flattened array
       move is the index vector in the file array */
    move[var->ndims-1] = startpos;
    size_t result;
    if ((result = make_coordinates(move, info->fcount, var->nfiledims)))
	nct_puterror("Overflow in make_coordinates: %zu, %s: %s\n", result, var->super->filename, var->name);
    for(int i=var->nfiledims-1; i>=0; i--) {
	if (move[i]) {
	    info->fstart[i] += move[i];
	    info->fcount[i] -= move[i];
	    info->f_used[i] -= move[i];
	    /* We can only read rectangles, and hence, we must stop at the first such dimension from behind,
	       where startpos is in the middle of the used area. */
	    for(int j=0; j<i; j++) {
		info->fcount[j] = 1;
		info->f_used[j] = 1;
	    }
	    break;
	}
    }
    /* Make sure not to read more than asked. */
    size_t len = limit_rectangle(info->fcount, var->nfiledims, info->ndata - info->pos);
    /* Limiting f_used as well should not be needed. */
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
    printf("}; from %i: %s", info->ifile, var->super->filename);
    if (nct_verbose == nct_verbose_newline)
	putchar('\n');
    else if (nct_verbose == nct_verbose_overwrite)
	printf("\033[K\r"), fflush(stdout);
}

static size_t get_rect_size(const size_t* sizes, int ndims) {
    size_t cumdimlen = 1;
    for (int i=ndims-1; i>=0; i--)
	cumdimlen *= sizes[i];
    return cumdimlen;
}

static int next_load(nct_var* var, loadinfo_t* info) {
    size_t start_thisfile;
    int filenum;
    if (info->pos >= info->ndata)
	return 2; // All data are ready.
    if (info->varstart >= var->len)
	return 1; // This virtual file is ready.
    if (get_filenum(info->filestart, var, &filenum, &start_thisfile))
	return -1; // an error which shouldn't happen
    if (filenum == 0) {
	size_t len = set_info(var, info, start_thisfile);
	if (nct_verbose)
	    print_progress(var, info, len);
	load_for_real(var, info);
	info->data += len * info->size1;
	info->pos += len;
	info->varstart += len;
	info->filestart += get_rect_size(info->f_used, var->nfiledims);
	info->ifile++;
	return 0;
    }
    /* If this wasn't the first file, we call this function again to handle
       concatenation rules correctly on this file. */
    nct_var* var1 = get_var_from_filenum(var, filenum);
    size_t old_start[] = {info->varstart, info->filestart};
    info->varstart = info->filestart = start_thisfile;
    while(!next_load(var1, info));
    info->varstart = info->varstart - start_thisfile + old_start[0];
    info->filestart = info->filestart - start_thisfile + old_start[1];
    /*
    size_t readlen = info->filestart - start_thisfile;
    info->start = old_start + readlen;*/
    return 0;
}

static nct_var* load_coordinate_var(nct_var* var) {
    nct_ncget_t getfulldata = nct_getfun[var->dtype];
    int size1 = nctypelen(var->dtype);
    size_t len = get_filelen(var);
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
    size_t fstart[nct_maxdims], fcount[nct_maxdims], f_used[nct_maxdims]; // start and count in a real file
    if ((var->dtype == NC_CHAR) + (dtype == NC_CHAR) + 2*(dtype == NC_NAT) == 1) {
	nct_puterror("Cannot convert to or from NC_CHAR. Variable %s%s%s in %s%s%s\n",
		nct_varname_color, var->name, nct_default_color, nct_varset_color, var->super->filename, nct_default_color);
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
	.f_used = f_used,
	.ndata	= end - start,
	.data	= var->data,
	.getfun	= nct_getfun_partial[var->dtype],
	.varstart	= start,
	.filestart	= start,
    };
    while (!next_load(var, &info));
    if (info.ndata != info.pos)
	nct_puterror("%s: Loaded %zu instead of %zu\n", var->name, info.pos, info.ndata);
    var->startpos = start;
    var->endpos = end;
    if (nct_verbose == nct_verbose_overwrite)
	printf("\033[K"), fflush(stdout);
    return var;
}

nct_var* nct_load_as(nct_var* var, nc_type dtype) {
    return nct_load_partially_as(var, 0, var->len, dtype);
}

#undef MIN
