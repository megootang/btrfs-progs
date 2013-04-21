
#include <Python.h>

/* ----------------------------------------------------------------------
 * Method tables and other bureaucracy
 */

static PyMethodDef btrfs_methods[] = {
	/* LVM methods */
//	{ "getVersion",		(PyCFunction)liblvm_library_get_version, METH_NOARGS },
//	{ "vgOpen",		(PyCFunction)liblvm_lvm_vg_open, METH_VARARGS },
	{ NULL,	     NULL}	   /* sentinel */
};


PyMODINIT_FUNC
initbtrfs(void)
{
	PyObject *m;

	m = Py_InitModule3("btrfs", btrfs_methods, "Btrfs module");
	if (m == NULL)
		return;
}
