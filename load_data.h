#define from_concatlist(var, num) (((nct_var**)(var)->rule[nct_r_concat].arg.v)[num])
#define MIN(a, b) ((a) <= (b) ? a : (b))

typedef struct {
    int size1;
    size_t *fstart, *fcount, pos, ndata, start;
    void* data;
    nct_ncget_partial_t getfun;
} loadinfo_t;

static long make_coordinates(size_t* arr, const size_t* dims, int ndims) {
    long carry = 0, num, dimlen = 1;
    for(int i=ndims-1; i>=0; i--) {
	num = arr[i]*dimlen + carry;
	long next_dimlen = dimlen * dims[i];
	long consumes = num % next_dimlen;
	arr[i] = consumes / dimlen;
	dimlen = next_dimlen;
	carry = num - consumes;
    }
    return carry;
}

static nct_var* get_var_from_filenum(nct_var* var, int num) {
    if (num)
	return from_concatlist(var, num-1);
    return var;
}

static size_t get_filelen(const nct_var* var) {
    size_t len = 1;
    for(int i=var->ndims-1; i>=0; i--)
	len *= var->filedimensions[i];
    return len;
}

static size_t var_offset(const nct_var* var) {
    size_t offset = 0, cumlen = 1;
    for(int i=var->ndims-1; i>=0; i--) {
	nct_var* dim = nct_get_vardim(var, i);
	offset += dim->rule[nct_r_start].arg.lli * cumlen;
	cumlen *= dim->len;
    }
    return offset;
}

static int get_filenum(long start, nct_var* var, int* farg, size_t* parg) {
    int f = 0, nfiles = var->rule[nct_r_concat].n + 1;
    long p = 0; // must be signed: if offset > filelen; then ptry_0 < 0
    while (f < nfiles) {
	long ptry = p + (f ? get_var_from_filenum(var, f)->len : get_filelen(var) - var_offset(var));
	if (ptry > start) {
	    *parg = start - p; // where to start reading this file
	    *farg = f;
	    return 0;
	}
	p = ptry;
	f++;
    }
    nct_puterror("Didn't find starting location\n");
    return 1;
}

static void load_for_real(nct_var* var, loadinfo_t* info) {
    perhaps_open_the_file(var);
    ncfunk(info->getfun, var->super->ncid, var->ncid, info->fstart, info->fcount, info->data);
    perhaps_close_the_file(var->super);
}

static size_t limit_rectangle(size_t* rect, int ndims, size_t maxsize) {
    int i = ndims - 1;
    size_t len = 1;
    for(; i>=0; i--) {
	size_t try = len * rect[i];
	if (try > maxsize)
	    break;
	len = try;
    } 
    memset(rect, 0, (i+1)*sizeof(rect[0]));
    return len;
}

/* fstart, fcount */
static size_t set_info(const nct_var* var, loadinfo_t* info, size_t startpos) {
    for(int i=var->ndims-1; i>=0; i--) {
	nct_var* dim = nct_get_vardim(var, i);
	info->fstart[i] = dim->rule[nct_r_start].arg.lli;
	info->fcount[i] = MIN(var->filedimensions[i] - info->fstart[i], dim->len);
    }
    /* startpos */
    size_t move[nct_maxdims] = {0};
    move[var->ndims-1] = startpos;
    make_coordinates(move, info->fcount, var->ndims);
    int first = 1;
    for(int i=var->ndims-1; i>=0; i--) {
	if (move[i]) {
	    info->fstart[i] += move[i];
	    if (first)
		info->fcount[i] -= move[i];
	    first = 0;
	    continue;
	}
	if (!first)
	    info->fcount[i] = 0;
    }
    /* Make sure not to read more than asked. */
    size_t len = limit_rectangle(info->fcount, var->ndims, info->ndata - info->pos);
    return len;
}

static int next_load(nct_var* var, loadinfo_t* info) {
    size_t start_thisfile;
    int filenum;
    if (info->start >= var->len)
	return 1;
    if (get_filenum(info->start, var, &filenum, &start_thisfile))
	return 1; // an error which shouldn't happen
    if (filenum == 0) {
	size_t len = set_info(var, info, start_thisfile);
	load_for_real(var, info);
	info->data += len * info->size1;
	info->pos += len;
	info->start += len;
	return 0;
    }
    /* If this wasn't the first file, we call this function again to handle
       concatenation rules correctly on this file. */
    nct_var* var1 = get_var_from_filenum(var, filenum);
    size_t old_start = info->start;
    info->start	= start_thisfile;
    while(!next_load(var1, info));
    size_t readlen = info->start-start_thisfile;
    info->start = old_start + readlen;
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
    var->data += var->rule[nct_r_start].arg.lli * size1;
    return var;
}

nct_var* nct_load_as(nct_var* var, nc_type dtype) {
    size_t fstart[nct_maxdims], fcount[nct_maxdims]; // start and count in a real file
    if (dtype != NC_NAT) {
	if (var->dtype)
	    var->capacity *= nctypelen(dtype) / nctypelen(var->dtype);
	var->dtype = dtype;
    }
    if (nct_iscoord(var))
	return load_coordinate_var(var);
    nct_allocate_varmem(var);
    loadinfo_t info = {
	.size1	= nctypelen(var->dtype),
	.fstart	= fstart,
	.fcount	= fcount,
	.ndata	= var->len,
	.data	= var->data,
	.getfun	= nct_getfun_partial[var->dtype],
    };
    while (!next_load(var, &info));
    if (var->len != info.pos)
	nct_puterror("%s: Loaded %zu instead of %zu\n", var->name, info.pos, var->len);
    return var;
}

#undef MIN
#undef from_concatlist
