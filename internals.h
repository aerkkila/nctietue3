/* The dimension has to exist on v0->super and its length has to be already increased.
   Currently, the dimension has to also be the first one. */
static nct_var* _nct_concat_var(nct_var* v0, const nct_var* v1, int dimid0, int howmany_left) {
    v0->len = nct_get_len_from(v0, 0); // recalculate length since dimensions have been changed
    if(!v0->ndims || v0->dimids[0] != dimid0)
	nct_add_vardim_first(v0, dimid0);
    long len = nct_get_len_from(v0, 1) * (v0->super->dims[v0->dimids[0]]->len + howmany_left); // how much is needed
    if(v0->len < v1->len) {
	nct_puterror("wrong dimensions in _nct_concat_var\n");
	print_varerror(v0, "    ");
	print_varerror(v1, "    ");
	nct_return_error(NULL);
    }
    size_t s1 = nctypelen(v0->dtype);
    if(len >= v0->capacity) {
	if(v0->not_freeable || (v0->nusers && *v0->nusers))
	    v0->data = malloc(len*s1);
	else
	    v0->data = realloc(v0->data, len*s1);
	v0->capacity = len;
    }
    void* ptr = v0->data + (v0->len - v1->len) * s1;
    memcpy(ptr, v1->data, v1->len*s1);
    return v0;
}

/* The dimension must be already expanded. */
static nct_var* _nct_expand_var(nct_var* var, int vardim, int howmuch, int start0_end1, nct_any fill) {
    size_t vlen0, vlen1, dlen0, dlen1, block0, block1, fillsize, nblocks;
    vlen0 = var->len;
    vlen1 = var->len = nct_get_len_from(var, 0);
    dlen1 = nct_get_vardim(var, vardim)->len;
    dlen0 = dlen1 - howmuch;
    block1 = nct_get_len_from(var, vardim);
    block0 = block1 / dlen1 * dlen0;
    fillsize = block1 - block0;
    nblocks = vlen0 / block0;
    void *old, *now, *now0;
    old = var->data;
    int size1 = nctypelen(var->dtype);
    now = now0 = malloc(vlen1 * size1);
    if (!now)
	nct_return_error(NULL);

    if (start0_end1 == 1)	// expand in end
	if (size1 == 1)
	    for (int _i=0; _i<nblocks; _i++) {
		memcpy(now, old, block0*size1);
		old += block0*size1;
		now += block0*size1;
		memset(now, fill.hhu, fillsize);
		now += fillsize;
	    }
	else
	    for (int _i=0; _i<nblocks; _i++) {
		memcpy(now, old, block0*size1);
		old += block0*size1;
		now += block0*size1;
		for(int _j=0; _j<fillsize; _j++, now+=size1)
		    memcpy(now, &fill, size1);
	    }
    else			// expand in start
	if (size1 == 1)
	    for (int _i=0; _i<nblocks; _i++) {
		memset(now, fill.hhu, fillsize);
		now += fillsize;
		memcpy(now, old, block0*size1);
		old += block0*size1;
		now += block0*size1;
	    }
	else
	    for (int _i=0; _i<nblocks; _i++) {
		for(int _j=0; _j<fillsize; _j++, now+=size1)
		    memcpy(now, &fill, size1);
		memcpy(now, old, block0*size1);
		old += block0*size1;
		now += block0*size1;
	    }

    nct_unlink_data(var);
    var->data = now0;
    return var;
}

/* _nct_read_var_info must be called first for all variables */
static void _nct_read_dim(nct_set* set, int dimid) {
    char name[256];
    size_t len;
    int varid;
    nct_var** v = set->dims + dimid;
    ncfunk(nc_inq_dim, set->ncid, dimid, name, &len);
    if ((varid = nct_get_varid(set, name)) >= 0) {
	(*v = set->vars[varid]) -> len = len;
	(*v)->id = nct_coordid((*v)->id);
	return;
    }
    *v = malloc(sizeof(nct_var));
    **v = (nct_var) {
	.super         = set,
	.id            = dimid,
	.ncid          = dimid,
	.name          = strdup(name),
	.freeable_name = 1,
	.len           = len,
    };
    (**v).filedimensions[0] = len;
    return;
}

static nct_set* _nct_read_var_info(nct_set *set, int varid, int flags) {
    int ndims, dimids[128], natts;
    nc_type dtype;
    size_t len;
    char name[512];
    ncfunk(nc_inq_var, set->ncid, varid, name, &dtype, &ndims, dimids, &natts);
    nct_var* dest = set->vars[varid] = malloc(sizeof(nct_var));
    *dest = (nct_var) {
	.super         = set,
	.id            = varid,
	.ncid          = varid,
	.name          = strdup(name),
	.freeable_name = 1,
	.ndims         = ndims,
	.dimcapacity   = ndims+1,
	.natts         = natts,
	.attcapacity   = natts,
	.dtype         = dtype,
    };
    dest->dimids = malloc(dest->dimcapacity*sizeof(int));
    memcpy(dest->dimids, dimids, ndims*sizeof(int));

    if (!(flags & nct_ratt)) {
	dest->natts = dest->attcapacity = 0;
	return set;
    }

    dest->atts = malloc(dest->attcapacity*sizeof(nct_att));
    for(int i=0; i<natts; i++) {
	ncfunk(nc_inq_attname, set->ncid, varid, i, name);
	ncfunk(nc_inq_att, set->ncid, varid, name, &dtype, &len);
	nct_att* att  = dest->atts+i;
	att->name     = strdup(name);
	att->value    = malloc(len*nctypelen(dtype) + (dtype==NC_CHAR));
	att->dtype    = dtype;
	att->len      = len;
	att->freeable = 3;
	ncfunk(nc_get_att, set->ncid, varid, name, att->value);
	if (att->dtype == NC_CHAR) {
	    if (!att->len) {
		att->value = '\0';
		continue;
	    }
	    if (((char*)att->value)[len-1] != '\0')
		att->len++;
	    ((char*)att->value)[att->len-1] = '\0';
	}
    }
    return set;
}
