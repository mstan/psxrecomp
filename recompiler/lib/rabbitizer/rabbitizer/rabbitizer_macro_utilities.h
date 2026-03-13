/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#ifndef PYRABBITIZER_MACRO_UTILITIES_H
#define PYRABBITIZER_MACRO_UTILITIES_H
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "structmember.h"


#define RAB_STRCMP_LITERAL(var, literal) strncmp(var, literal, sizeof(literal) - 1)


#define DECL_RAB_TYPE(typeName, memberName) \
    typedef struct PyRabbitizer##typeName { \
        PyObject_HEAD \
        Rabbitizer##typeName memberName; \
    } PyRabbitizer##typeName; \
    \
    extern PyTypeObject rabbitizer_type_##typeName##_TypeObject; \
    \
    int rabbitizer_type_##typeName##_TypeObject_Check(PyObject *object); \
    int rabbitizer_type_##typeName##_Converter_Optional(PyObject *object, PyRabbitizer##typeName **address);

#define DEF_RAB_TYPE(typeName) \
    /* Returns positive if the argument is an instance, 0 if it isn't or negative on error */ \
    int rabbitizer_type_##typeName##_TypeObject_Check(PyObject *object) { \
        int isInstance = PyObject_IsInstance(object, (PyObject*)&rabbitizer_type_##typeName##_TypeObject); \
        \
        if (isInstance < 0) { \
            /* An error happened */ \
            /* PyObject_IsInstance already sets an exception, so nothing else to do here */ \
            return -1; \
        } \
        \
        if (isInstance == 0) { \
            /* `object` isn't an instance */ \
            return 0; \
        } \
        \
        return 1; \
    } \
    \
    int rabbitizer_type_##typeName##_Converter_Optional(PyObject *object, PyRabbitizer##typeName **address) { \
        int instanceCheck; \
        \
        if ((object == NULL) || (address == NULL)) { \
            PyErr_Format(PyExc_RuntimeError, "%s: Internal error", __func__); \
            return 0; /* fail */ \
        } \
        \
        if (object == Py_None) { \
            *address = NULL; \
            return 1; /* successful */ \
        } \
        \
        instanceCheck = rabbitizer_type_##typeName##_TypeObject_Check(object); \
        if (instanceCheck < 0) { \
            return 0; /* fail */ \
        } \
        if (instanceCheck > 0) { \
            *address = (PyRabbitizer##typeName *)object; \
            return 1; /* successful */ \
        } \
        \
        PyErr_Format(PyExc_TypeError, "argument must be %s or None, not %s", rabbitizer_type_##typeName##_TypeObject.tp_name, object->ob_type->tp_name); \
        return 0; /* fail */ \
    }


#endif
