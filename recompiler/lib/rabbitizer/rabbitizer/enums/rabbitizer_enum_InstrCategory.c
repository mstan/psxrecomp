/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "enums_utils.h"
#include "instructions/RabbitizerInstruction.h"


#define RABBITIZER_DEF_INSTR_CATEGORY(name) { "InstrCategory", #name, RABBITIZER_INSTRCAT_##name, false, NULL },

RabbitizerEnumMetadata rabbitizer_enum_InstrCategory_enumvalues[] = {
    #include "tables/InstrCategory.inc"
    RABBITIZER_DEF_INSTR_CATEGORY(MAX)

    { 0 },
};

#undef RABBITIZER_DEF_INSTR_CATEGORY


static PyObject *rabbitizer_enum_InstrCategory_fromStr(UNUSED PyObject *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = { "name", NULL };
    const char *name = NULL;
    RabbitizerInstrCategory instrCategory;
    PyObject *ret;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "z", kwlist, &name)) {
        return NULL;
    }

    instrCategory = RabbitizerInstrCategory_fromStr(name);
    if ((int)instrCategory < 0) {
        Py_RETURN_NONE;
    }

    ret = rabbitizer_enum_InstrCategory_enumvalues[instrCategory].instance;
    Py_INCREF(ret);
    return ret;
}


#define METHOD_NO_ARGS(name, docs)  { #name, (PyCFunction) (void *) rabbitizer_enum_InstrCategory_##name, METH_NOARGS,                  PyDoc_STR(docs) }
#define METHOD_ARGS(name, docs)     { #name, (PyCFunction) (void *) rabbitizer_enum_InstrCategory_##name, METH_VARARGS | METH_KEYWORDS, PyDoc_STR(docs) }

static PyMethodDef rabbitizer_enum_InstrCategory_methods[] = {
    METHOD_ARGS(fromStr, ""),

    { 0 },
};

DEF_ENUM(InstrCategory, "")
