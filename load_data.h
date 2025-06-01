#define MIN(a, b) ((a) <= (b) ? a : (b))
#define MAX(a, b) ((a) >= (b) ? a : (b))

typedef struct {
	/* constants after initialization */
	int size1, dtype;
	size_t ndata;
	nct_ncget_partial_t getfun;
	nct_ncget_t getfun_full;
	long len_from_1; // how many data in a step in the first dimension
	/* mutable variables */
	size_t *fstart, *fcount;
	long start, dim0startdiff;
	/* restricted: only incremented at loading */
	size_t pos;
	void* data;
	int iread; // only used for printing progress (nct_verbose)
} loadinfo_t;

static size_t get_fileind_from_coords(const nct_var* var, const size_t* coords) {
	size_t ind = 0;
	size_t cum = 1;
	for (int i=var->nfiledims-1; i>=0; i--) {
		ind += cum * coords[i];
		cum *= nct_get_vardim(var, i)->len;
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
		nct_verbose_line_ending();
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
		if (info->fcount[idim] < nct_get_vardim(var, idim)->len)
			return load_stream_partially(var, info, info->fstart, idim, 0, &nloaded);
	if ((nread = fread(info->data, info->size1, var->len, stream)) != var->len)
		nct_puterror("fread returned %zu instead of %zu in %s (%i)\n", nread, var->len, __func__, __LINE__);
	return;
}

static long make_coordinates(size_t* arr, const size_t* dims, int ndims) {
	long carry = 0, num, dimlen = 1;
	for (int i=ndims-1; i>=0; i--) {
		num = arr[i]*dimlen + carry;
		long next_dimlen = dimlen * dims[i];
		long num_thisdim = num % next_dimlen;
		arr[i] = num_thisdim / dimlen;
		dimlen = next_dimlen;
		carry = num - num_thisdim;
	}
	return carry / dimlen;
}

static int get_filenum(loadinfo_t *info, nct_var* var) {
	if (!var->ndims) {
		if (info->start == 0)
			return 0;
		else
			goto error;
	}
	if (var->concatlist.n == 0)
		return 0;

	long start_dim0 = info->start / info->len_from_1 + info->dim0startdiff;
	for (int ifile=0; ifile<var->concatlist.n; ifile++)
		if (var->concatlist.coords[ifile][0] > start_dim0)
			return ifile; // ifile-1 will be read

	if (var->concatlist.n && var->concatlist.coords[var->concatlist.n-1][1] > start_dim0)
		return var->concatlist.n;

error: __attribute__((cold));
	   nct_puterror("Startlocation (%li: %li) not found from %s\n", info->start, start_dim0, nct_get_filename_var(var));
	   nct_return_error(-1);
}

static void load_for_real(nct_var* var, loadinfo_t* info) {
	if (var->stream)
		return load_stream(var, info);
	int ncid = perhaps_open_the_file(var);
	if (var->nfiledims == 0)
		ncfunk(info->getfun_full, ncid, var->ncid, info->data);
	else
		ncfunk(info->getfun, ncid, var->ncid, info->fstart, info->fcount, info->data);
	perhaps_close_the_file(var);
}

static size_t limit_rectangle(size_t* rect, int ndims, size_t maxsize) {
	int idim = ndims - 1;
	size_t len = 1;
	for (; idim>=0; idim--) {
		size_t try = len * rect[idim];
		if (try > maxsize)
			goto Break;
		len = try;
	}
	return len;
Break:
	for (int i=0; i<idim; i++)
		rect[i] = 1;
	size_t n_idim = maxsize / len;
	rect[idim] = n_idim;
	len *= n_idim;
	return len;
}

/* fstart, fcount */
static size_t set_info(const nct_var* var, loadinfo_t* info) {
	if (var->nfiledims == 0) {
		info->fstart[0] = 0;
		info->fcount[0] = 1;
		return 1;
	}

	/* fstart and fcount omitting startpos and endpos */
	int n_extra = var->ndims - var->nfiledims;
	for (int i=var->nfiledims-1; i>=0; i--) {
		nct_var* dim = nct_get_vardim(var, i+n_extra);
		info->fstart[i] = dim->startdiff;
		info->fcount[i] = dim->len;
	}
	if (var->concatlist.n && !n_extra) {
		size_t max = var->concatlist.coords[0][0];
		if (max < info->fcount[0])
			info->fcount[0] = max;
	}

	/* correct fstart and fcount with startpos */
	size_t move[var->nfiledims];
	memset(move, 0, sizeof(move));
	/* startpos is the index in the flattened array
	   move is the index vector in the file array */
	move[var->ndims-1] = info->start;
	size_t result;
	if ((result = make_coordinates(move, info->fcount, var->nfiledims)))
		nct_puterror("Overflow in make_coordinates: %zu, %s: %s\n", result, nct_get_filename_var(var), var->name);
	for (int i=var->nfiledims-1; i>=0; i--) {
		if (move[i]) {
			info->fstart[i] += move[i];
			info->fcount[i] -= move[i];
			/* We can only read rectangles, and hence, we must stop at the first such dimension from behind,
			   where startpos is in the middle of the used area. */
			for (int j=0; j<i; j++)
				info->fcount[j] = 1;
			break;
		}
	}

	/* Make sure not to read more than asked. */
	size_t len = limit_rectangle(info->fcount, var->nfiledims, info->ndata - info->pos);
	return len;
}

static void print_progress(const nct_var* var, const loadinfo_t* info, size_t len) {
	printf("%i. Loading %zu %% (%zu / %zu); n=%zu; start={%li", info->iread,
		(info->pos+len)*100/info->ndata, info->pos+len, info->ndata, len, info->fstart[0]);
	for (int i=1; i<var->nfiledims; i++)
		printf(", %li", info->fstart[i]);
	printf("}; count={%li", info->fcount[0]);
	for (int i=1; i<var->nfiledims; i++)
		printf(", %li", info->fcount[i]);
	printf("}; %s: %s", nct_get_filename_var(var), var->name);
	nct_verbose_line_ending();
}

static nct_var* _nct_load_partially_as(nct_var* var, long start, long end, nc_type dtype, int *Nread);

static int next_load(nct_var* var, loadinfo_t* info) {
	int filenum;
	if (info->pos >= info->ndata)
		return 2; // All data are ready.
	if (info->start >= var->len)
		return 1; // This virtual file is ready.
	if ((filenum = get_filenum(info, var)) < 0)
		return -1; // error
	if (filenum == 0) {
		size_t len = set_info(var, info);
		if (nct_verbose)
			print_progress(var, info, len);
		load_for_real(var, info);
		if (nct_after_load)
			nct_after_load(var, info->data, len, info->fstart, info->fcount);
		info->data += len * info->size1;
		info->pos += len;
		info->start += len;
		info->iread++;
		return 0;
	}

	/* a variable from the concatlist */
	nct_var* var1 = var->concatlist.list[filenum-1];
	long relstart_var1 = (var->concatlist.coords[filenum-1][0] - info->dim0startdiff) * info->len_from_1;
	long relend_var1 = (var->concatlist.coords[filenum-1][1] - info->dim0startdiff) * info->len_from_1;
	if (relend_var1 - relstart_var1 != var1->len) {
		nct_puterror("concatvar size mismatch\n");
		nct_return_error(-1);
	}

	long len1 = Min(info->ndata-info->pos, var1->len);
	var1->data = info->data;
	var1->capacity = len1;
	var1->not_freeable = 1;
	var1->dtype = var->dtype; // var->dtype may have been manually changed after nct_set_concat
	int nread;
	long start1 = info->start - relstart_var1;
	_nct_load_partially_as(var1, start1, start1+len1, var->dtype, &nread);
	if (var1->data != info->data) {
		nct_puterror("jotain outoa");
		free(var1->data);
	}
	info->data += len1 * info->size1;
	info->pos += len1;
	info->start += len1;
	info->iread += nread;

	return 0;
}

static nct_var* load_coordinate_var(nct_var* var) {
	nct_ncget_t getfulldata = nct_getfun[var->dtype];
	int size1 = nctypelen(var->dtype);
	size_t len = var->len + var->startdiff - var->enddiff;
	if (!var->data) {
		var->data = malloc(len*size1);
		var->capacity = len;
	}
	else if (var->capacity < len) {
		var->data -= var->startdiff * size1;
		var->data = realloc(var->data, len*size1);
		var->capacity = len;
	}
	int ncid = perhaps_open_the_file(var);
	ncfunk(getfulldata, ncid, var->ncid, var->data);
	perhaps_close_the_file(var);
	var->startpos = 0;
	var->endpos = var->len;
	var->data += var->startdiff * size1;
	return var;
}

struct loadthread_args {
	nct_var *var;
	long start, end;
	nc_type dtype;
};

static void* nct_load_async(void *vargs) {
	struct loadthread_args *args = vargs;
	args->var->load_async = 0;
	void *ret = nct_load_partially_as(args->var, args->start, args->end, args->dtype);
	args->var->load_async = 1;
	free(vargs);
	return ret;
}

nct_var* nct_load_partially_as(nct_var* var, long start, long end, nc_type dtype) {
	if (!nct_loadable(var))
		return NULL;
	if (var->load_async) {
		struct loadthread_args *args = malloc(sizeof(args[0]));
		args->var = var;
		args->start = start;
		args->end = end;
		args->dtype = dtype;
		pthread_create(&var->loadthread, NULL, nct_load_async, args);
		return var;
	}

	if ((var->dtype == NC_CHAR) + (dtype == NC_CHAR) + 2*(dtype == NC_NAT) == 1) {
		nct_puterror("Cannot convert to or from NC_CHAR. Variable %s%s%s in %s%s%s\n",
			nct_varname_color, var->name, nct_default_color, nct_varset_color, nct_get_filename_var(var), nct_default_color);
		nct_return_error(NULL);
	}
	if (dtype != NC_NAT) {
		if (var->dtype)
			var->capacity *= nctypelen(dtype) / nctypelen(var->dtype);
		var->dtype = dtype;
	}
	if (nct_iscoord(var))
		return load_coordinate_var(var);
	else if (!var->ndims) {
		nct_allocate_varmem(var);
		return ((nct_ncget_t)nct_getfun[var->dtype])(var->super->ncid, var->ncid, var->data), var;
	}

	size_t old_length = var->len;
	var->len = end-start; // huono, jos load_async
	nct_allocate_varmem(var);
	var->len = old_length;

	int nread;
	return _nct_load_partially_as(var, start, end, dtype, &nread);
}

static nct_var* _nct_load_partially_as(nct_var* var, long start, long end, nc_type dtype, int *Nread) {
	size_t fstart[var->nfiledims], fcount[var->nfiledims]; // start and count in a real file
	loadinfo_t info = {
		.size1	= nctypelen(var->dtype),
		.fstart	= fstart,
		.fcount	= fcount,
		.ndata	= end - start,
		.data	= var->data,
		.getfun	= nct_getfun_partial[var->dtype],
		.getfun_full = nct_getfun[var->dtype],
		.start	= start,
		.dim0startdiff = nct_get_vardim(var, 0)->startdiff,
		.len_from_1 = nct_get_len_from(var, 1),
		.dtype  = dtype,
	};
	while (!next_load(var, &info));
	if (info.ndata != info.pos)
		nct_puterror("%s: Loaded %zu instead of %zu\n", var->name, info.pos, info.ndata);
	var->startpos = start;
	var->endpos = end;
	*Nread = info.iread;
	return var;
}

nct_var* nct_load_as(nct_var* var, nc_type dtype) {
	return nct_load_partially_as(var, 0, var->len, dtype);
}

/* Uses private nct_set.fileinfo. */
int nct_loadable(const nct_var* var) {
	return
		(var->ncid>=0 && ((var)->super->ncid > 0 || var->super->fileinfo || var->fileinfo)) ||
		var->stream;
}

#undef MIN
