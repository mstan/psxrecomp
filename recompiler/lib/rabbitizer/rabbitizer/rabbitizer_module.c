/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "rabbitizer_module.h"

#include <stdbool.h>

#include "common/Utils.h"
#include "instructions/RabbitizerInstrId.h"
#include "common/RabbitizerVersion.h"


typedef enum ModuleAttributeCategory {
    MODULE_ATTRIBUTE_CAT_TYPE,
    MODULE_ATTRIBUTE_CAT_INIT, // submodules
    MODULE_ATTRIBUTE_CAT_GLOBAL,
} ModuleAttributeCategory;

typedef struct ModuleAttribute {
    union {
        PyTypeObject *type;
        PyObject *(*init)(void);
        PyTypeObject *global;
    };
    ModuleAttributeCategory cat;
    const char *name;
    bool isInstanced;
    PyObject *instance;
} ModuleAttributes;

#define MODULE_ATTRIBUTE_TYPE(name)   { {.type   = &rabbitizer_type_##name##_TypeObject},   MODULE_ATTRIBUTE_CAT_TYPE,   #name, false, NULL }
#define MODULE_ATTRIBUTE_INIT(name)   { {.init   = rabbitizer_submodule_##name##_Init},     MODULE_ATTRIBUTE_CAT_INIT,   #name, false, NULL }
#define MODULE_ATTRIBUTE_ENUM(name)   { {.init   = rabbitizer_enum_##name##_Init},          MODULE_ATTRIBUTE_CAT_INIT,   #name, false, NULL }
#define MODULE_ATTRIBUTE_GLOBAL(name) { {.global = &rabbitizer_global_##name##_TypeObject}, MODULE_ATTRIBUTE_CAT_GLOBAL, #name, false, NULL }

static ModuleAttributes rabbitizer_module_attributes[] = {
    MODULE_ATTRIBUTE_INIT(Utils),

    MODULE_ATTRIBUTE_GLOBAL(config),

    MODULE_ATTRIBUTE_TYPE(Enum),
    MODULE_ATTRIBUTE_ENUM(Abi),
    MODULE_ATTRIBUTE_ENUM(InstrCategory),
    MODULE_ATTRIBUTE_ENUM(InstrId),
    MODULE_ATTRIBUTE_ENUM(InstrIdType),
    MODULE_ATTRIBUTE_ENUM(OperandType),
    MODULE_ATTRIBUTE_ENUM(AccessType),

    MODULE_ATTRIBUTE_ENUM(RegGprO32),
    MODULE_ATTRIBUTE_ENUM(RegGprN32),
    MODULE_ATTRIBUTE_ENUM(RegCop1O32),
    MODULE_ATTRIBUTE_ENUM(RegCop1N32),
    MODULE_ATTRIBUTE_ENUM(RegCop1N64),

    MODULE_ATTRIBUTE_TYPE(Instruction),
    MODULE_ATTRIBUTE_TYPE(LoPairingInfo),
    MODULE_ATTRIBUTE_TYPE(TrackedRegisterState),
    MODULE_ATTRIBUTE_TYPE(RegistersTracker),
};

static int rabbitizer_module_attributes_Ready(void) {
    // Sanity checks and PyType_Ready
    for (size_t i = 0; i < ARRAY_COUNT(rabbitizer_module_attributes); i++) {
        if (rabbitizer_module_attributes[i].global == NULL || rabbitizer_module_attributes[i].name == NULL) {
            return -1;
        }
        switch (rabbitizer_module_attributes[i].cat) {
            case MODULE_ATTRIBUTE_CAT_TYPE:
            case MODULE_ATTRIBUTE_CAT_GLOBAL:
                if (PyType_Ready(rabbitizer_module_attributes[i].type) < 0) {
                    return -1;
                }
                break;

            case MODULE_ATTRIBUTE_CAT_INIT:
                break;

            default:
                return -1;
        }
    }

    return 0;
}

static int rabbitizer_module_attributes_Initialize(PyObject *module) {
    for (size_t i = 0; i < ARRAY_COUNT(rabbitizer_module_attributes); i++) {
        switch (rabbitizer_module_attributes[i].cat) {
            case MODULE_ATTRIBUTE_CAT_TYPE:
                rabbitizer_module_attributes[i].instance = (PyObject*) rabbitizer_module_attributes[i].type;
                Py_INCREF(rabbitizer_module_attributes[i].instance);
                break;

            case MODULE_ATTRIBUTE_CAT_INIT:
                rabbitizer_module_attributes[i].instance = rabbitizer_module_attributes[i].init();
                if (rabbitizer_module_attributes[i].instance == NULL) {
                    goto error;
                }
                break;

            case MODULE_ATTRIBUTE_CAT_GLOBAL:
                rabbitizer_module_attributes[i].instance = PyObject_CallObject((PyObject*)rabbitizer_module_attributes[i].global, NULL);
                if (rabbitizer_module_attributes[i].instance == NULL) {
                    goto error;
                }
                break;
        }

        rabbitizer_module_attributes[i].isInstanced = true;
        if (PyModule_AddObject(module, rabbitizer_module_attributes[i].name, rabbitizer_module_attributes[i].instance) < 0) {
            goto error;
        }
    }

    return 0;

error:
    for (size_t i = 0; i < ARRAY_COUNT(rabbitizer_module_attributes); i++) {
        if (rabbitizer_module_attributes[i].isInstanced) {
            Py_DECREF(rabbitizer_module_attributes[i].instance);
        }
        rabbitizer_module_attributes[i].isInstanced = false;
    }
    return -1;
}

#define rabbitizer_module_method___getattr___docs "Hacky way to get read-only global variables"

PyObject *rabbitizer_module_method___getattr__(UNUSED void *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = { "name", NULL };
    const char *attributeName = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist, &attributeName)) {
        return NULL;
    }

    if (attributeName == NULL) {
        PyErr_Format(PyExc_AssertionError, "%s: assert(attributeName != NULL)", __func__);
        return NULL;
    }

    if (RAB_STRCMP_LITERAL(attributeName, "__version_info__") == 0) {
        return Py_BuildValue("(iii)", RabVersion_Major, RabVersion_Minor, RabVersion_Patch);
    }
    if (RAB_STRCMP_LITERAL(attributeName, "__version__") == 0) {
        return PyUnicode_FromString(RabVersion_Str);
    }
    if (RAB_STRCMP_LITERAL(attributeName, "__author__") == 0) {
        return PyUnicode_FromString(RabVersion_Author);
    }

    PyErr_Format(PyExc_AttributeError, "module '%s' has no attribute '%s'",
                                        rabbitizer_module.m_name, attributeName);
    return NULL;
}

#define METHOD_NO_ARGS(name)  { #name, (PyCFunction) (void *) rabbitizer_module_method_##name, METH_NOARGS,                  PyDoc_STR(rabbitizer_module_method_##name##_docs) }
#define METHOD_ARGS(name)     { #name, (PyCFunction) (void *) rabbitizer_module_method_##name, METH_VARARGS | METH_KEYWORDS, PyDoc_STR(rabbitizer_module_method_##name##_docs) }

PyMethodDef rabbitizer_module_methods[] = {
    METHOD_ARGS(__getattr__),

    { 0 },
};


PyModuleDef rabbitizer_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "rabbitizer",
    .m_doc = "",
    .m_size = -1,
    .m_methods = rabbitizer_module_methods,
};

PyMODINIT_FUNC PyInit_rabbitizer(void) {
    PyObject *module;

    if (rabbitizer_module_attributes_Ready() < 0) {
        return NULL;
    }

    module = PyModule_Create(&rabbitizer_module);
    if (module == NULL) {
        return NULL;
    }

    if (rabbitizer_module_attributes_Initialize(module) < 0) {
        Py_DECREF(module);
        return NULL;
    }

    return module;
}
