#include <proj.h>
#include <errno.h>
#include <err.h>
#include <string.h>
#include <float.h>	// DBL_MAX, DBL_MIN
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
//#include <nctplot.h>

#define MIN(a,b) ((a) < (b) ? a : (b))

typedef struct {
    void* src;
    long len_old, xylen_old, pos, len, xlen, ylen;
    int* whence, size1;
    char nan[8];
} nctproj_cookie;

static int iround(double a) {
    int i = a;
    i += ((a-i) >= 0.5) - ((a-i) <= -0.5);
    return i;
}

static void get_coordinate_limits(double xlim[2], double ylim[2], const nct_var* xvar, const nct_var* yvar, PJ* pj) {
    double x0 = nct_getg_floating(xvar, 0);
    double xdiff = nct_getg_floating(xvar, 1) - x0;
    double y0 = nct_getg_floating(yvar, 0);
    double ydiff = nct_getg_floating(yvar, 1) - y0;
    int xlen_old = xvar->len;
    xlim[0] = ylim[0] = +DBL_MAX;
    xlim[1] = ylim[1] = -DBL_MAX;
    PJ_COORD pjc = {0};
    for(int j=yvar->len-1; j>=0; j--) {
	pjc.xy.y = y0 + j*ydiff;
	for(int i=xlen_old-1; i>=0; i--) {
	    pjc.xy.x = x0 + i*xdiff;
	    PJ_XY xy = proj_trans(pj, 1, pjc).xy;
	    if (isnormal(xy.x) || xy.x == 0) {
		if (xy.x < xlim[0])
		    xlim[0] = xy.x;
		if (xy.x > xlim[1])
		    xlim[1] = xy.x;
	    }
	    if (isnormal(xy.y) || xy.y == 0) {
		if (xy.y < ylim[0])
		    ylim[0] = xy.y;
		if (xy.y > ylim[1])
		    ylim[1] = xy.y;
	    }
	}
    }
}

static int* mkconversion(const nct_var* var, const char* from, const char* to, nctproj_cookie* cookie, nctproj_args_t* args) {
    int* whence = NULL;
    PJ_CONTEXT* ctx;
    ctx = proj_context_create();

    nct_var* xvar = var->super->dims[var->dimids[var->ndims-1]];
    nct_var* yvar = var->super->dims[var->dimids[var->ndims-2]];
    int xlen_old = xvar->len;
    int ylen_old = yvar->len;
    int xylen_old = xlen_old * ylen_old;
    cookie->xylen_old = xylen_old;

    /* Conversion from old coordinates to new the ones is needed to get the new spacing and limits of the new coordinates.
       The conversion itself will be done from the new coordinates to the old ones. */
    PJ* pj = proj_create_crs_to_crs(ctx, from, to, NULL);
    if (!pj) {
	nct_puterror("proj_create_crs_to_crs: \"%s\" to \"%s\": %s\n", from, to, strerror(errno));
	return NULL;
    }
    get_coordinate_limits(args->new_xlim, args->new_ylim, xvar, yvar, pj);
    proj_destroy(pj);

    /* The new coordinate spacing is defined such that the total frame size stays approximately the same. */
    for(int i=0; i<2; i++) {
	if (args->new_xlim[i] && !isnormal(args->new_xlim[i])) {
	    nct_puterror("%s xlimit is %lf\n", !i? "lower": "higher", args->new_xlim[i]);
	    return NULL;
	}
	if (args->new_ylim[i] && !isnormal(args->new_ylim[i])) {
	    nct_puterror("%s ylimit is %lf\n", !i? "lower": "higher", args->new_xlim[i]);
	    return NULL;
	}
    }
    double new_x_per_y = (args->new_xlim[1] - args->new_xlim[0]) / (args->new_ylim[1] - args->new_ylim[0]);
    if (new_x_per_y < 0)
	new_x_per_y = -new_x_per_y;
    /*
     * new_ylen * new_xlen = old_xylen
     * new_xlen = new_ylen * new_x_per_y
     * ==>
     * new_ylen * new_ylen * new_x_per_y = old_xylen
     * ==>
     * new_ylen = sqrt(old_xylen/new_x_per_y)
     */
    double help = sqrt(xylen_old / new_x_per_y);
    args->new_ylen = iround(help);
    args->new_xlen = iround(help * new_x_per_y);
    args->new_ydiff = (args->new_ylim[1] - args->new_ylim[0]) / args->new_ylen;
    args->new_xdiff = (args->new_xlim[1] - args->new_xlim[0]) / args->new_xlen;
    if (!args->unequal_xydiff) {
	args->new_ydiff = args->new_xdiff = MIN(args->new_ydiff, args->new_xdiff);
	args->new_ylen = iround((args->new_ylim[1] - args->new_ylim[0]) / args->new_ydiff);
	args->new_xlen = iround((args->new_xlim[1] - args->new_xlim[0]) / args->new_xdiff);
    }
    size_t new_xylen = args->new_ylen * args->new_xlen;
    cookie->xlen = args->new_xlen;
    cookie->ylen = args->new_ylen;

    /* Now we can initialize the new coordinates. */
    whence = malloc(new_xylen*sizeof(int));
    if (!whence) {
	warn("malloc %zu failed", new_xylen*sizeof(int));
	goto out;
    }
    pj = proj_create_crs_to_crs(ctx, to, from, NULL);

    double x0 = nct_getg_floating(xvar, 0);
    double xdiff = nct_getg_floating(xvar, 1) - x0;
    double y0 = nct_getg_floating(yvar, 0);
    double ydiff = nct_getg_floating(yvar, 1) - y0;
    PJ_COORD pjc = {0};
    for(int j=args->new_ylen-1; j>=0; j--) {
	pjc.xy.y = args->new_ylim[0] + j*args->new_ydiff;
	for(int i=args->new_xlen-1; i>=0; i--) {
	    pjc.xy.x = args->new_xlim[0] + i*args->new_xdiff;
	    PJ_XY xy = proj_trans(pj, 1, pjc).xy;
	    int xind_old = iround((xy.x - x0) / xdiff);
	    int yind_old = iround((xy.y - y0) / ydiff);
	    int point;
	    if (yind_old < 0 || yind_old >= ylen_old ||
		    xind_old < 0 || xind_old >= xlen_old)
		point = -1;
	    else
		point = yind_old*xlen_old + xind_old;
	    whence[j*args->new_xlen+i] = point;
	}
    }
    proj_destroy(pj);

    /*
    int* apu = malloc(new_xylen*sizeof(int));
    for(int i=0; i<new_xylen; i++)
	apu[i] = whence[i] / xlen_old;
    nct_set* set = nct_create_simple(apu, NC_INT, args->new_ylen, args->new_xlen);
    nctplot(set);
    nct_free1(set);
    apu = malloc(new_xylen*sizeof(int));
    for(int i=0; i<new_xylen; i++)
	apu[i] = whence[i] % xlen_old;
    set = nct_create_simple(apu, NC_INT, args->new_ylen, args->new_xlen);
    nctplot(set);
    nct_free1(set);
    set = nct_create_simple(whence, NC_INT, args->new_ylen, args->new_xlen);
    nctplot(set);
    nct_firstvar(set)->not_freeable = 1;
    nct_free1(set);
    */

out:
    proj_context_destroy(ctx);
    return whence;
}

__ssize_t read_converted(void* vc, char* dst, size_t nbytes) {
    nctproj_cookie* c = vc;
    int size1 = c->size1;
    void* src = c->src;
    int* whence = c->whence;
    /* Ensure that count is within the array limits. */
    size_t count = nbytes / size1;
    if (c->pos+count > c->len)
	count = c->len - c->pos;
    if (c->pos == c->len)
	return EOF;
    long current = 0;
    /* Now we don't have to worry about reaching the end of the array. */
    int new_xylen = c->xlen * c->ylen;
    while (current < count) {
	int frame = c->pos / new_xylen;
	int old_offset = frame * c->xylen_old;
	int startcell = c->pos % new_xylen;
	int howmany = MIN(count-current, new_xylen-startcell); // until the wanted count or the end of the frame is reached
	for(int i=0; i<howmany; i++) {
	    if (whence[startcell+i] < 0)
		memcpy(dst, c->nan, size1);
	    else
		memcpy(dst, src+(old_offset+whence[startcell+i])*size1, size1);
	    dst += size1;
	}
	c->pos += howmany;
	current += howmany;
    }
    return current*size1;
}

int seek_converted(void* vc, __off64_t* pos, int seek) {
    nctproj_cookie* c = vc;
    switch(seek) {
	case SEEK_SET:
	    *pos = c->pos = *pos;
	    break;
	case SEEK_CUR:
	    *pos = c->pos += *pos;
	    break;
	case SEEK_END:
	    *pos = c->pos = c->len + *pos;
	    break;
	default:
	    return 1;
    }
    if (c->pos < 0) {
	*pos = c->pos = 0;
	return 2;
    }
    if (c->pos >= c->len) {
	*pos = c->pos = c->len;
	return EOF;
    }
    return 0;
}

int close_converted(void* vc) {
    nctproj_cookie* c = vc;
    c->whence = (free(c->whence), NULL);
    free(c);
    return 0;
}

__ssize_t write_converted(void* vc, const char* buf, size_t len) {
    return 0;
}

cookie_io_functions_t nctproj_functions = {
    .read = read_converted,
    .write = write_converted,
    .seek = seek_converted,
    .close = close_converted,
};

/* Things above are only meant for internal use.
   Things below are meant for public use. */

FILE* nctproj_open_converted(const nct_var* var, const char* from, const char* to, nctproj_args_t* args) {
    if (var->ndims < 2)
	return NULL;
    nctproj_cookie *c = malloc(sizeof(nctproj_cookie));
    *c = (nctproj_cookie){
	.pos	 = 0,
	.len_old = var->len,
	.size1 = nctypelen(var->dtype),
    };
    c->whence = mkconversion(var, from, to, c, args);
    c->len = var->ndims > 2 ?
	var->len / nct_get_len_from(var, var->ndims-2) * c->xlen * c->ylen :
	c->xlen * c->ylen;
    c->src = var->data;

    /* fill value */
    if (var->dtype == NC_FLOAT) {
	float nan = 0.0/0.0;
	memcpy(c->nan, &nan, 4);
    }
    else if (var->dtype == NC_DOUBLE) {
	double nan = 0.0/0.0;
	memcpy(c->nan, &nan, 8);
    }
    else
	memset(c->nan, 0, sizeof(c->nan));

    return fopencookie(c, "r", nctproj_functions);
}

nct_var* nctproj_open_converted_var(const nct_var* var, const char* from, const char* to, nctproj_args_t* args) {
    nctproj_args_t _args = {0};
    if (!args)
	args = &_args;
    FILE* f = nctproj_open_converted(var, from, to, args);
    if (!f)
	return NULL;
    nct_var* newvar;
    int dimids[var->ndims];
    memcpy(dimids, var->dimids, var->ndims*sizeof(int));

    newvar = nct_dim2coord(nct_ensure_unique_name(nct_add_dim(var->super, args->new_xlen, "x_transform")), NULL, NC_DOUBLE);
    nct_put_interval(newvar, args->new_xlim[0], args->new_xdiff);
    dimids[var->ndims-1] = nct_dimid(newvar);

    newvar = nct_dim2coord(nct_ensure_unique_name(nct_add_dim(var->super, args->new_ylen, "y_transform")), NULL, NC_DOUBLE);
    nct_put_interval(newvar, args->new_ylim[0], args->new_ydiff);
    dimids[var->ndims-2] = nct_dimid(newvar);

    char name[strlen(var->name)+15];
    sprintf(name, "%s_transform", var->name);
    newvar = nct_add_var(var->super, NULL, var->dtype, strdup(name), var->ndims, dimids);
    newvar->freeable_name = 1;
    nct_ensure_unique_name(newvar);
    nct_set_stream(newvar, f);
    return newvar;
}

#undef MIN
