
#include <Python.h>
#include <uuid/uuid.h>
#include "list.h"
#include "utils.h"
#include "volumes.h"

/* ----------------------------------------------------------------------
 * Method tables and other bureaucracy
 */

PyObject*
btrfs_list_subvolumes(PyObject *module)
{
	struct list_head *all_uuids;
	struct btrfs_fs_devices *fs_devices;
	struct list_head *cur_uuid;
	char uuidbuf[37];
	int ret;
	PyObject *list;
	PyObject *str;

	ret = btrfs_scan_one_dir("/dev", 0);
	if (ret) {
		PyErr_SetString(PyExc_RuntimeError, "Could not scan /dev");
		return NULL;
	}

	list = PyList_New(0);
	if (!list)
		return NULL;

	all_uuids = btrfs_scanned_uuids();
	list_for_each(cur_uuid, all_uuids) {
		fs_devices = list_entry(cur_uuid, struct btrfs_fs_devices,
					list);
		uuid_unparse(fs_devices->fsid, uuidbuf);

		str = PyString_FromString(uuidbuf);
		if (!str) {
			Py_DECREF(list);
			return PyErr_NoMemory();
		}

		if (PyList_Append(list, str) < 0) {
			Py_DECREF(str);
			Py_DECREF(list);
			return PyErr_NoMemory();
		}
		Py_DECREF(str);
	}

	return list;
}


static PyMethodDef btrfs_methods[] = {
	/* LVM methods */
	{ "list_subvolumes",	(PyCFunction)btrfs_list_subvolumes, METH_NOARGS },
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
