/*[clinic input]
preserve
[clinic start generated code]*/

#if defined(Py_BUILD_CORE) && !defined(Py_BUILD_CORE_MODULE)
#  include "pycore_gc.h"          // PyGC_Head
#  include "pycore_runtime.h"     // _Py_ID()
#endif
#include "pycore_modsupport.h"    // _PyArg_UnpackKeywords()

PyDoc_STRVAR(_version_Version_from_parts__doc__,
"from_parts($type, /, *, release, epoch=0, pre=None, post=None,\n"
"           dev=None, local=None)\n"
"--\n"
"\n"
"Build a version directly from validated components.");

#define _VERSION_VERSION_FROM_PARTS_METHODDEF    \
    {"from_parts", _PyCFunction_CAST(_version_Version_from_parts), METH_FASTCALL|METH_KEYWORDS|METH_CLASS, _version_Version_from_parts__doc__},

static PyObject *
_version_Version_from_parts_impl(PyTypeObject *type, PyObject *release,
                                 PyObject *epoch, PyObject *pre,
                                 PyObject *post, PyObject *dev,
                                 PyObject *local);

static PyObject *
_version_Version_from_parts(PyObject *type, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *return_value = NULL;
    #if defined(Py_BUILD_CORE) && !defined(Py_BUILD_CORE_MODULE)

    #define NUM_KEYWORDS 6
    static struct {
        PyGC_Head _this_is_not_used;
        PyObject_VAR_HEAD
        Py_hash_t ob_hash;
        PyObject *ob_item[NUM_KEYWORDS];
    } _kwtuple = {
        .ob_base = PyVarObject_HEAD_INIT(&PyTuple_Type, NUM_KEYWORDS)
        .ob_hash = -1,
        .ob_item = { &_Py_ID(release), &_Py_ID(epoch), &_Py_ID(pre), &_Py_ID(post), &_Py_ID(dev), &_Py_ID(local), },
    };
    #undef NUM_KEYWORDS
    #define KWTUPLE (&_kwtuple.ob_base.ob_base)

    #else  // !Py_BUILD_CORE
    #  define KWTUPLE NULL
    #endif  // !Py_BUILD_CORE

    static const char * const _keywords[] = {"release", "epoch", "pre", "post", "dev", "local", NULL};
    static _PyArg_Parser _parser = {
        .keywords = _keywords,
        .fname = "from_parts",
        .kwtuple = KWTUPLE,
    };
    #undef KWTUPLE
    PyObject *argsbuf[6];
    Py_ssize_t noptargs = nargs + (kwnames ? PyTuple_GET_SIZE(kwnames) : 0) - 1;
    PyObject *release;
    PyObject *epoch = Py_None;
    PyObject *pre = Py_None;
    PyObject *post = Py_None;
    PyObject *dev = Py_None;
    PyObject *local = Py_None;

    args = _PyArg_UnpackKeywords(args, nargs, NULL, kwnames, &_parser,
            /*minpos*/ 0, /*maxpos*/ 0, /*minkw*/ 1, /*varpos*/ 0, argsbuf);
    if (!args) {
        goto exit;
    }
    release = args[0];
    if (!noptargs) {
        goto skip_optional_kwonly;
    }
    if (args[1]) {
        epoch = args[1];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[2]) {
        pre = args[2];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[3]) {
        post = args[3];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[4]) {
        dev = args[4];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    local = args[5];
skip_optional_kwonly:
    return_value = _version_Version_from_parts_impl((PyTypeObject *)type, release, epoch, pre, post, dev, local);

exit:
    return return_value;
}

PyDoc_STRVAR(_version_Version___replace____doc__,
"__replace__($self, /, *, epoch=..., release=..., pre=..., post=...,\n"
"            dev=..., local=...)\n"
"--\n"
"\n"
"Replace components; omitted components retain their values.");

#define _VERSION_VERSION___REPLACE___METHODDEF    \
    {"__replace__", _PyCFunction_CAST(_version_Version___replace__), METH_FASTCALL|METH_KEYWORDS, _version_Version___replace____doc__},

static PyObject *
_version_Version___replace___impl(PyObject *self, PyObject *epoch,
                                  PyObject *release, PyObject *pre,
                                  PyObject *post, PyObject *dev,
                                  PyObject *local);

static PyObject *
_version_Version___replace__(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *return_value = NULL;
    #if defined(Py_BUILD_CORE) && !defined(Py_BUILD_CORE_MODULE)

    #define NUM_KEYWORDS 6
    static struct {
        PyGC_Head _this_is_not_used;
        PyObject_VAR_HEAD
        Py_hash_t ob_hash;
        PyObject *ob_item[NUM_KEYWORDS];
    } _kwtuple = {
        .ob_base = PyVarObject_HEAD_INIT(&PyTuple_Type, NUM_KEYWORDS)
        .ob_hash = -1,
        .ob_item = { &_Py_ID(epoch), &_Py_ID(release), &_Py_ID(pre), &_Py_ID(post), &_Py_ID(dev), &_Py_ID(local), },
    };
    #undef NUM_KEYWORDS
    #define KWTUPLE (&_kwtuple.ob_base.ob_base)

    #else  // !Py_BUILD_CORE
    #  define KWTUPLE NULL
    #endif  // !Py_BUILD_CORE

    static const char * const _keywords[] = {"epoch", "release", "pre", "post", "dev", "local", NULL};
    static _PyArg_Parser _parser = {
        .keywords = _keywords,
        .fname = "__replace__",
        .kwtuple = KWTUPLE,
    };
    #undef KWTUPLE
    PyObject *argsbuf[6];
    Py_ssize_t noptargs = nargs + (kwnames ? PyTuple_GET_SIZE(kwnames) : 0) - 0;
    PyObject *epoch = NULL;
    PyObject *release = NULL;
    PyObject *pre = NULL;
    PyObject *post = NULL;
    PyObject *dev = NULL;
    PyObject *local = NULL;

    args = _PyArg_UnpackKeywords(args, nargs, NULL, kwnames, &_parser,
            /*minpos*/ 0, /*maxpos*/ 0, /*minkw*/ 0, /*varpos*/ 0, argsbuf);
    if (!args) {
        goto exit;
    }
    if (!noptargs) {
        goto skip_optional_kwonly;
    }
    if (args[0]) {
        epoch = args[0];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[1]) {
        release = args[1];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[2]) {
        pre = args[2];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[3]) {
        post = args[3];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    if (args[4]) {
        dev = args[4];
        if (!--noptargs) {
            goto skip_optional_kwonly;
        }
    }
    local = args[5];
skip_optional_kwonly:
    return_value = _version_Version___replace___impl(self, epoch, release, pre, post, dev, local);

exit:
    return return_value;
}
/*[clinic end generated code: output=fa85077a9c4bd35d input=a9049054013a1b77]*/
