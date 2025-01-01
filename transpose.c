#include <stdarg.h>

	static void
_makedim(void* new, const void* old, size_t size1,
	const size_t* strides_old, const size_t* strides_new, const size_t* lengths_new)
{
	if (strides_new[0] == 1) {
		for(int i=0; i<lengths_new[0]; i++) {
			memcpy(new, old, size1);
			new += size1;
			old += size1*strides_old[0];
		}
		return;
	}
	for(int i=0; i<lengths_new[0]; i++) {
		_makedim(new, old, size1,
			strides_old+1, strides_new+1, lengths_new+1);
		new += strides_new[0]*size1;
		old += strides_old[0]*size1;
	}
}

nct_var* nct_transpose_order_ptr(nct_var* var, const int* order) {
	for (int i=0; i<var->ndims; i++)
		if (order[i] != i)
			goto action_needed;
	return var;

action_needed:
	int size1 = nctypelen(var->dtype);
	void* new = malloc(var->len*size1);
	if (!new) {
		nct_puterror("malloc failed: %s\n", strerror(errno));
		return NULL;
	}

	size_t lengths_new[var->ndims], strides_old[var->ndims], strides_new[var->ndims];
	for (int i=var->ndims-1; i>=0; i--) {
		if (i == var->ndims-1) {
			strides_new[var->ndims-1] = 1;
			lengths_new[var->ndims-1] = nct_get_vardim(var, order[var->ndims-1])->len;
		}
		else {
			lengths_new[i] = nct_get_vardim(var, order[i])->len;
			strides_new[i] = strides_new[i+1] * lengths_new[i+1];
		}
		strides_old[i] = nct_get_len_from(var, order[i]+1);
	}

	_makedim(new, var->data, size1,
		strides_old, strides_new, lengths_new);
	nct_unlink_data(var);
	var->data = new;
	var->capacity = var->len;
	int help[var->ndims];
	memcpy(help, var->dimids, var->ndims*sizeof(int));
	for (int i=var->ndims-1; i>=0; i--)
		var->dimids[i] = help[order[i]];
	return var;
}

nct_var* nct_transpose_names_ptr(nct_var* var, const char* const* names) {
	int order[var->ndims];
	for (int i=var->ndims-1; i>=0; i--) {
		int j;
		for (j=var->ndims-1; j>=0; j--)
			if (!strcmp(names[i], nct_get_vardim(var, j)->name))
				goto found;
		nct_puterror("dimension %s not found\n", names[i]);
		return NULL;
found:
		order[i] = j;
	}
	return nct_transpose_order_ptr(var, order);
}

nct_var* nct_transpose_order(nct_var* var, ...) {
	int order[var->ndims];
	va_list valist;
	va_start(valist, var);
	for (int i=0; i<var->ndims; i++)
		order[i] = va_arg(valist, int);
	va_end(valist);
	return nct_transpose_order_ptr(var, order);
}

nct_var* nct_transpose_names(nct_var* var, ...) {
	const char* names[var->ndims];
	va_list valist;
	va_start(valist, var);
	for (int i=0; i<var->ndims; i++)
		names[i] = va_arg(valist, const char*);
	va_end(valist);
	return nct_transpose_names_ptr(var, names);
}
