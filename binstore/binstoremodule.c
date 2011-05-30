/* $Id: binstoremodule.c 6459 2010-05-21 14:54:18Z wheirman $ */

#include <Python.h>
#include <stdint.h>
#include <structmember.h>
#include "binstore.h"


/**************************************************************************
 ***** binstore                                                       *****
 **************************************************************************/

typedef struct {
        PyObject_HEAD
        BINSTORE * bs;
} binstoreObject;


static int
binstore_init(binstoreObject *self, PyObject *args, PyObject *kwds)
{
        char * filename;

        static char *kwlist[] = {"filename", NULL};
        if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist, &filename))
                return -1;

        if (strcmp(filename, "-") == 0)
                filename = "/dev/stdout";

        self->bs = binstore_open(filename, "w");
        if (!self->bs) {
                PyErr_SetString(PyExc_IOError, "Can't open file!");
                return -1;
        }

        return 0;
}


int pybinstore_store_item(binstoreObject* self, PyObject *item)
{
        if (PyInt_Check(item)) {
                binstore_store_items(self->bs, "i", PyInt_AS_LONG(item));

        } else if (PyLong_Check(item)) {
                binstore_store_items(self->bs, "i", PyLong_AsLongLong(item));
                if (PyErr_Occurred()) return 0;

        } else if (PyString_Check(item)) {
                size_t size = PyString_GET_SIZE(item);
                if (size == 1)
                        binstore_store_items(self->bs, "c", PyString_AS_STRING(item)[0]);
                else
                        binstore_store_items(self->bs, "s", PyString_AS_STRING(item));

        } else if (PyTuple_Check(item)) {
                int i;
                binstore_store_items(self->bs, "(");
                for(i = 0; i < PyTuple_GET_SIZE(item); ++i)
                        if (pybinstore_store_item(self, PyTuple_GET_ITEM(item, i)) == 0)
                                return 0;
                binstore_store_items(self->bs, ")");

        } else if (PyList_Check(item)) {
                int i;
                binstore_store_items(self->bs, "(");
                for(i = 0; i < PyList_GET_SIZE(item); ++i)
                        if (pybinstore_store_item(self, PyList_GET_ITEM(item, i)) == 0)
                                return 0;
                binstore_store_items(self->bs, ")");

        } else {
                PyErr_SetString(PyExc_ValueError, "Invalid arg (must be string, int, long, tuple, list).");
                return 0;
        }

        return 1;
}


static PyObject *
pybinstore_store(binstoreObject* self, PyObject *args)
{
        if (!PyTuple_Check(args)) {
                PyErr_SetString(PyExc_ValueError, "Need tuple arg");
                return NULL;
        }

        int i;
        for(i = 0; i < PyTuple_GET_SIZE(args); ++i)
                if (pybinstore_store_item(self, PyTuple_GET_ITEM(args, i)) == 0)
                        return NULL;
        binstore_store_end(self->bs);

        Py_RETURN_NONE;
}


static void
binstore_dealloc(binstoreObject* self)
{
        binstore_close(self->bs);
        self->ob_type->tp_free(self);
}


static PyMethodDef binstore_methods[] = {
        { "store", (PyCFunction)pybinstore_store, METH_VARARGS, "Store a record" },
        {NULL}  /* Sentinel */
};


static PyTypeObject binstoreType = {
        PyObject_HEAD_INIT(NULL)
        tp_name:                "binstore.binstore",
        tp_basicsize:           sizeof(binstoreObject),
        tp_flags:               Py_TPFLAGS_DEFAULT,
        tp_doc:                 "Binstore object",
        tp_new:                 PyType_GenericNew,
        tp_init:                (initproc)binstore_init,
        tp_dealloc:             (destructor)binstore_dealloc,
        tp_methods:             binstore_methods,
        //tp_members:             binstore_members,
};


/**************************************************************************
 ***** binload                                                        *****
 **************************************************************************/

typedef struct {
        PyObject_HEAD
        BINSTORE * bs;
} binloadObject;


static int
binload_init(binloadObject *self, PyObject *args, PyObject *kwds)
{
        char * filename;

        static char *kwlist[] = {"filename", NULL};
        if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist, &filename))
                return -1;

        if (strcmp(filename, "-") == 0)
                filename = "/dev/stdin";

        self->bs = binstore_open(filename, "r");
        if (!self->bs) {
                PyErr_SetString(PyExc_IOError, "Can't open file!");
                return -1;
        }

        return 0;
}


static PyObject *
binload_iter(binloadObject* self)
{
        Py_INCREF(self);
        return (PyObject *)self;
}


static PyObject *
binload_next(binloadObject* self)
{
        const void * ptr;
        int max_parts = 1024;
        PyObject ** objs = NULL;
        int partnum = 0;

        objs = malloc(max_parts * sizeof(PyObject *));

        while(1) {
                if (partnum >= max_parts) {
                        max_parts *= 2;
                        objs = realloc(objs, max_parts * sizeof(PyObject *));
                }
                const char type = binstore_load(self->bs, &ptr);
                if (!type) {
					    /* EOF */
					    if (partnum)
					            break; 			/* return tuple below */
					    else
					            return NULL;	/* no data */
				}
				if (type == ')')
                        break;					/* end of tuple: always return (possibly empty) tuple */
                switch(type) {
                        case 'c':
                                objs[partnum++] = PyString_FromStringAndSize((const char *)ptr, 1);
                                break;
                        case 'i':
                                objs[partnum++] = PyInt_FromLong(*(uint32_t *)ptr);
                                break;
                        case 'l':
                                objs[partnum++] = PyLong_FromLongLong(*(uint64_t *)ptr);
                                break;
                        case 's':
                                objs[partnum++] = PyString_FromString((const char *)ptr);
                                break;
                        case '(': {
                                objs[partnum++] = binload_next(self);
                                break;
					    }
                        default:
                                fprintf(stderr, "binstore: unknown record type %c (%02x)\n", type, type);
                                assert(0);
                }
        };

		int i;
		PyObject * result = PyTuple_New(partnum);
		for(i = 0; i < partnum; ++i)
				PyTuple_SET_ITEM(result, i, objs[i]);
		free(objs);
		return result;
}


static void
binload_dealloc(binloadObject* self)
{
        if (self->bs)
                binstore_close(self->bs);
        self->ob_type->tp_free(self);
}


static PyTypeObject binloadType = {
        PyObject_HEAD_INIT(NULL)
        tp_name:                "binstore.binload",
        tp_basicsize:           sizeof(binloadObject),
        tp_flags:               Py_TPFLAGS_DEFAULT,
        tp_doc:                 "Binload object",
        tp_new:                 PyType_GenericNew,
        tp_init:                (initproc)binload_init,
        tp_dealloc:             (destructor)binload_dealloc,
        tp_iter:                (getiterfunc)binload_iter,
        tp_iternext:            (iternextfunc)binload_next,
};


/**************************************************************************
 ***** module                                                         *****
 **************************************************************************/

static PyMethodDef BinstoreMethods[] = {
        { NULL, NULL, 0, NULL }
};


PyMODINIT_FUNC
init_binstore(void)
{
        PyObject* m;

        if (PyType_Ready(&binstoreType) < 0)
                return;
        if (PyType_Ready(&binloadType) < 0)
                return;

        m = Py_InitModule("_binstore", BinstoreMethods);
        if (m == NULL)
                return;

        Py_INCREF(&binstoreType);
        PyModule_AddObject(m, "binstore", (PyObject *)&binstoreType);
        Py_INCREF(&binloadType);
        PyModule_AddObject(m, "binload", (PyObject *)&binloadType);
}
