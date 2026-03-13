/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "rabbitizer_module.h"

#include "instructions/RabbitizerInstructionRsp.h"
#include "instructions/RabbitizerInstructionR3000GTE.h"
#include "instructions/RabbitizerInstructionR5900.h"
#include "common/RabbitizerConfig.h"


static void rabbitizer_type_Instruction_dealloc(PyRabbitizerInstruction *self) {
    RabbitizerInstruction_destroy(&self->instr);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static int rabbitizer_type_Instruction_init(PyRabbitizerInstruction *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = { "word", "vram", "category", NULL };
    uint32_t word;
    uint32_t vram = 0;
    PyObject *category = NULL;
    int enumCheck;
    RabbitizerInstrCategory instrCategory = RABBITIZER_INSTRCAT_CPU;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "I|IO!", kwlist, &word, &vram, &rabbitizer_type_Enum_TypeObject, &category)) {
        return -1;
    }

    if (category != NULL) {
        enumCheck = rabbitizer_enum_InstrCategory_Check(category);

        if (enumCheck <= 0) {
            if (enumCheck == 0) {
                PyErr_SetString(PyExc_ValueError, "Invalid value for 'category' parameter");
            }
            return -1;
        }

        instrCategory = ((PyRabbitizerEnum*)category)->value;
    }

    switch (instrCategory) {
        case RABBITIZER_INSTRCAT_RSP:
            RabbitizerInstructionRsp_init(&self->instr, word, vram);
            RabbitizerInstructionRsp_processUniqueId(&self->instr);
            break;

        case RABBITIZER_INSTRCAT_R3000GTE:
            RabbitizerInstructionR3000GTE_init(&self->instr, word, vram);
            RabbitizerInstructionR3000GTE_processUniqueId(&self->instr);
            break;

        case RABBITIZER_INSTRCAT_R5900:
            RabbitizerInstructionR5900_init(&self->instr, word, vram);
            RabbitizerInstructionR5900_processUniqueId(&self->instr);
            break;

        case RABBITIZER_INSTRCAT_CPU:
        case RABBITIZER_INSTRCAT_MAX:
            RabbitizerInstruction_init(&self->instr, word, vram);
            RabbitizerInstruction_processUniqueId(&self->instr);
            break;
    }

    return 0;
}

static PyMemberDef rabbitizer_type_Instruction_members[] = {
    { "vram", T_UINT, offsetof(PyRabbitizerInstruction, instr.vram), 0, "vram description" },
    { "inHandwrittenFunction", T_BOOL, offsetof(PyRabbitizerInstruction, instr.inHandwrittenFunction), 0, "inHandwrittenFunction description" },

    { 0 }
};


#define DEF_MEMBER_GET_UINT(name) \
    static PyObject *rabbitizer_type_Instruction_member_get_##name(PyRabbitizerInstruction *self, UNUSED PyObject *closure) { \
        return PyLong_FromUnsignedLong(RAB_INSTR_GET_##name(&self->instr)); \
    }

#define RETURN_GPR(reg) \
    do { \
        PyObject *enumInstance = NULL; \
        \
        switch (RabbitizerConfig_Cfg.regNames.gprAbiNames) { \
            case RABBITIZER_ABI_N32: \
            case RABBITIZER_ABI_N64: \
                enumInstance = rabbitizer_enum_RegGprN32_enumvalues[reg].instance; \
                break; \
        \
            default: \
                enumInstance = rabbitizer_enum_RegGprO32_enumvalues[reg].instance; \
                break; \
        } \
        \
        if (enumInstance == NULL) { \
            PyErr_SetString(PyExc_RuntimeError, "Internal error: invalid RegGpr enum value"); \
            return NULL; \
        } \
        \
        Py_INCREF(enumInstance); \
        return enumInstance; \
    } while (0)

#define DEF_MEMBER_GET_REGGPR(name) \
    static PyObject *rabbitizer_type_Instruction_member_get_##name(PyRabbitizerInstruction *self, UNUSED PyObject *closure) { \
        uint32_t reg; \
        PyObject *enumInstance = NULL; \
        \
        if (!RabbitizerInstruction_hasOperandAlias(&self->instr, RAB_OPERAND_cpu_##name)) { \
            PyErr_Format(PyExc_RuntimeError, "'%s' instruction does not reference register '" #name "'", RabbitizerInstrId_getOpcodeName(self->instr.uniqueId)); \
            return NULL; \
        } \
        \
        reg = RAB_INSTR_GET_##name(&self->instr); \
        switch (RabbitizerConfig_Cfg.regNames.gprAbiNames) { \
            case RABBITIZER_ABI_N32: \
            case RABBITIZER_ABI_N64: \
                enumInstance = rabbitizer_enum_RegGprN32_enumvalues[reg].instance; \
                break; \
        \
            default: \
                enumInstance = rabbitizer_enum_RegGprO32_enumvalues[reg].instance; \
                break; \
        } \
        \
        if (enumInstance == NULL) { \
            PyErr_SetString(PyExc_RuntimeError, "Internal error: invalid RegGpr enum value"); \
            return NULL; \
        } \
        \
        Py_INCREF(enumInstance); \
        return enumInstance; \
    }

DEF_MEMBER_GET_REGGPR(rs)
DEF_MEMBER_GET_REGGPR(rt)
DEF_MEMBER_GET_REGGPR(rd)

DEF_MEMBER_GET_UINT(sa)

#define DEF_MEMBER_GET_REGCOP1(name) \
    static PyObject *rabbitizer_type_Instruction_member_get_##name(PyRabbitizerInstruction *self, UNUSED PyObject *closure) { \
        uint32_t reg; \
        PyObject *enumInstance = NULL; \
        \
        if (!RabbitizerInstruction_hasOperandAlias(&self->instr, RAB_OPERAND_cpu_##name)) { \
            PyErr_Format(PyExc_RuntimeError, "'%s' instruction does not reference register '" #name "'", RabbitizerInstrId_getOpcodeName(self->instr.uniqueId)); \
            return NULL; \
        } \
        \
        reg = RAB_INSTR_GET_##name(&self->instr); \
        switch (RabbitizerConfig_Cfg.regNames.fprAbiNames) { \
            case RABBITIZER_ABI_N32: \
                enumInstance = rabbitizer_enum_RegCop1N32_enumvalues[reg].instance; \
                break; \
        \
            case RABBITIZER_ABI_N64: \
                enumInstance = rabbitizer_enum_RegCop1N64_enumvalues[reg].instance; \
                break; \
        \
            default: \
                enumInstance = rabbitizer_enum_RegCop1O32_enumvalues[reg].instance; \
                break; \
        } \
        \
        if (enumInstance == NULL) { \
            PyErr_SetString(PyExc_RuntimeError, "Internal error: invalid RegCop1 enum value"); \
            return NULL; \
        } \
        \
        Py_INCREF(enumInstance); \
        return enumInstance; \
    }

DEF_MEMBER_GET_REGCOP1(fs)
DEF_MEMBER_GET_REGCOP1(ft)
DEF_MEMBER_GET_REGCOP1(fd)

static PyObject *rabbitizer_type_Instruction_member_get_uniqueId(PyRabbitizerInstruction *self, UNUSED PyObject *closure) {
    PyObject *enumInstance = rabbitizer_enum_InstrId_enumvalues[self->instr.uniqueId].instance;

    if (enumInstance == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Internal error: invalid uniqueId enum value");
        return NULL;
    }

    Py_INCREF(enumInstance);
    return enumInstance;
}

static PyObject *rabbitizer_type_Instruction_member_get_instrIdType(PyRabbitizerInstruction *self, UNUSED PyObject *closure) {
    PyObject *enumInstance = rabbitizer_enum_InstrIdType_enumvalues[self->instr.instrIdType].instance;

    if (enumInstance == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Internal error: invalid instrIdType enum value");
        return NULL;
    }

    Py_INCREF(enumInstance);
    return enumInstance;
}

#define MEMBER_GET(name, docs, closure)      { #name, (getter) rabbitizer_type_Instruction_member_get_##name, (setter) NULL,                                          PyDoc_STR(docs), closure }
#define MEMBER_SET(name, docs, closure)      { #name, (getter) NULL,                                          (setter) rabbitizer_type_Instruction_member_set_##name, PyDoc_STR(docs), closure }
#define MEMBER_GET_SET(name, docs, closure)  { #name, (getter) rabbitizer_type_Instruction_member_get_##name, (setter) rabbitizer_type_Instruction_member_set_##name, PyDoc_STR(docs), closure }


static PyGetSetDef rabbitizer_type_Instruction_getsetters[] = {
    MEMBER_GET(rs, "", NULL),
    MEMBER_GET(rt, "", NULL),
    MEMBER_GET(rd, "", NULL),
    MEMBER_GET(sa, "", NULL),
    MEMBER_GET(fs, "", NULL),
    MEMBER_GET(ft, "", NULL),
    MEMBER_GET(fd, "", NULL),
    MEMBER_GET(uniqueId, "", NULL),
    MEMBER_GET(instrIdType, "", NULL),

    { 0 }
};


#define DEF_METHOD_GET_UINT(name) \
    static PyObject *rabbitizer_type_Instruction_##name(PyRabbitizerInstruction *self, UNUSED PyObject *closure) { \
        return PyLong_FromUnsignedLong(RabbitizerInstruction_##name(&self->instr)); \
    }

#define DEF_METHOD_GET_INT(name) \
    static PyObject *rabbitizer_type_Instruction_##name(PyRabbitizerInstruction *self, UNUSED PyObject *closure) { \
        return PyLong_FromLong(RabbitizerInstruction_##name(&self->instr)); \
    }

#define DEF_METHOD_BOOL(name) \
    static PyObject *rabbitizer_type_Instruction_##name(PyRabbitizerInstruction *self, UNUSED PyObject *closure) { \
        if (RabbitizerInstruction_##name(&self->instr)) { \
            Py_RETURN_TRUE; \
        } \
        Py_RETURN_FALSE; \
    }

DEF_METHOD_GET_UINT(getRaw)
static PyObject *rabbitizer_type_Instruction_getImmediate(PyRabbitizerInstruction *self, UNUSED PyObject *closure) {
    return PyLong_FromUnsignedLong(RAB_INSTR_GET_immediate(&self->instr));
}
DEF_METHOD_GET_INT(getProcessedImmediate)
DEF_METHOD_GET_UINT(getInstrIndexAsVram)
DEF_METHOD_GET_INT(getBranchOffset)

static PyObject *rabbitizer_type_Instruction_getOpcodeName(PyRabbitizerInstruction *self, UNUSED PyObject *closure) {
    return PyUnicode_FromString(RabbitizerInstrId_getOpcodeName(self->instr.uniqueId));
}

static PyObject *rabbitizer_type_Instruction_getGenericBranchOffset(PyRabbitizerInstruction *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = { "currentVram", NULL };
    uint32_t currentVram;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "I", kwlist, &currentVram)) {
        return NULL;
    }

    return PyLong_FromLong(RabbitizerInstruction_getGenericBranchOffset(&self->instr, currentVram));
}

DEF_METHOD_GET_INT(getBranchOffsetGeneric)
DEF_METHOD_GET_INT(getBranchVramGeneric)

static PyObject *rabbitizer_type_Instruction_getDestinationGpr(PyRabbitizerInstruction *self, UNUSED PyObject *closure) {
    int8_t reg = RabbitizerInstruction_getDestinationGpr(&self->instr);

    if (reg < 0) {
        Py_RETURN_NONE;
    }

    RETURN_GPR(reg);
}

DEF_METHOD_BOOL(outputsToGprZero)

static PyObject *rabbitizer_type_Instruction_blankOut(PyRabbitizerInstruction *self, UNUSED PyObject *closure) {
    RabbitizerInstruction_blankOut(&self->instr);
    Py_RETURN_NONE;
}

DEF_METHOD_BOOL(isImplemented)
DEF_METHOD_BOOL(isLikelyHandwritten)
DEF_METHOD_BOOL(isNop)
DEF_METHOD_BOOL(isUnconditionalBranch)

DEF_METHOD_BOOL(isReturn)
DEF_METHOD_BOOL(isJumptableJump)

DEF_METHOD_BOOL(isJrRa)
DEF_METHOD_BOOL(isJrNotRa)

DEF_METHOD_BOOL(hasDelaySlot)

static PyObject *rabbitizer_type_Instruction_mapInstrToType(PyRabbitizerInstruction *self, UNUSED PyObject *closure) {
    const char *type = RabbitizerInstruction_mapInstrToType(&self->instr);

    if (type != NULL) {
        return PyUnicode_FromString(type);
    }
    Py_RETURN_NONE;
}

static PyObject *rabbitizer_type_Instruction_sameOpcode(PyRabbitizerInstruction *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = { "other", NULL };
    PyRabbitizerInstruction *other;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!", kwlist, &rabbitizer_type_Instruction_TypeObject, &other)) {
        return NULL;
    }

    if (RabbitizerInstruction_sameOpcode(&self->instr, &other->instr)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject *rabbitizer_type_Instruction_sameOpcodeButDifferentArguments(PyRabbitizerInstruction *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = { "other", NULL };
    PyRabbitizerInstruction *other;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!", kwlist, &rabbitizer_type_Instruction_TypeObject, &other)) {
        return NULL;
    }

    if (RabbitizerInstruction_sameOpcodeButDifferentArguments(&self->instr, &other->instr)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject *rabbitizer_type_Instruction_hasOperand(PyRabbitizerInstruction *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = { "operand", NULL };
    PyObject *pyOperand = NULL;
    int enumCheck;
    RabbitizerOperandType operandType = RAB_OPERAND_ALL_INVALID;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!", kwlist, &rabbitizer_type_Enum_TypeObject, &pyOperand)) {
        return NULL;
    }

    if (pyOperand == NULL) {
        PyErr_SetString(PyExc_ValueError, "Invalid value for 'operand' parameter");
    }

    enumCheck = rabbitizer_enum_OperandType_Check(pyOperand);

    if (enumCheck <= 0) {
        if (enumCheck == 0) {
            PyErr_SetString(PyExc_ValueError, "Invalid value for 'operand' parameter");
        }
        return NULL;
    }

    operandType = ((PyRabbitizerEnum*)pyOperand)->value;

    if (RabbitizerInstruction_hasOperand(&self->instr, operandType)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject *rabbitizer_type_Instruction_hasOperandAlias(PyRabbitizerInstruction *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = { "operand", NULL };
    PyObject *pyOperand = NULL;
    int enumCheck;
    RabbitizerOperandType operandType = RAB_OPERAND_ALL_INVALID;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!", kwlist, &rabbitizer_type_Enum_TypeObject, &pyOperand)) {
        return NULL;
    }

    if (pyOperand == NULL) {
        PyErr_SetString(PyExc_ValueError, "Invalid value for 'operand' parameter");
    }

    enumCheck = rabbitizer_enum_OperandType_Check(pyOperand);

    if (enumCheck <= 0) {
        if (enumCheck == 0) {
            PyErr_SetString(PyExc_ValueError, "Invalid value for 'operand' parameter");
        }
        return NULL;
    }

    operandType = ((PyRabbitizerEnum*)pyOperand)->value;

    if (RabbitizerInstruction_hasOperandAlias(&self->instr, operandType)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

DEF_METHOD_BOOL(isValid)

#define DEF_DESCRIPTOR_METHOD_BOOL(name) \
    static PyObject *rabbitizer_type_Instruction_##name(PyRabbitizerInstruction *self, UNUSED PyObject *closure) { \
        if (RabbitizerInstrDescriptor_##name(self->instr.descriptor)) { \
            Py_RETURN_TRUE; \
        } \
        Py_RETURN_FALSE; \
    }

DEF_DESCRIPTOR_METHOD_BOOL(isUnknownType)
DEF_DESCRIPTOR_METHOD_BOOL(isJType)
DEF_DESCRIPTOR_METHOD_BOOL(isIType)
DEF_DESCRIPTOR_METHOD_BOOL(isRType)
DEF_DESCRIPTOR_METHOD_BOOL(isRegimmType)
DEF_DESCRIPTOR_METHOD_BOOL(isBranch)
DEF_DESCRIPTOR_METHOD_BOOL(isBranchLikely)
DEF_DESCRIPTOR_METHOD_BOOL(isJump)
DEF_DESCRIPTOR_METHOD_BOOL(isJumpWithAddress)
DEF_DESCRIPTOR_METHOD_BOOL(isTrap)
DEF_DESCRIPTOR_METHOD_BOOL(isFloat)
DEF_DESCRIPTOR_METHOD_BOOL(isDouble)
DEF_DESCRIPTOR_METHOD_BOOL(isUnsigned)
DEF_DESCRIPTOR_METHOD_BOOL(modifiesRs)
DEF_DESCRIPTOR_METHOD_BOOL(modifiesRt)
DEF_DESCRIPTOR_METHOD_BOOL(modifiesRd)
DEF_DESCRIPTOR_METHOD_BOOL(readsRs)
DEF_DESCRIPTOR_METHOD_BOOL(readsRt)
DEF_DESCRIPTOR_METHOD_BOOL(readsRd)
DEF_DESCRIPTOR_METHOD_BOOL(readsHI)
DEF_DESCRIPTOR_METHOD_BOOL(readsLO)
DEF_DESCRIPTOR_METHOD_BOOL(modifiesHI)
DEF_DESCRIPTOR_METHOD_BOOL(modifiesLO)
DEF_DESCRIPTOR_METHOD_BOOL(modifiesFs)
DEF_DESCRIPTOR_METHOD_BOOL(modifiesFt)
DEF_DESCRIPTOR_METHOD_BOOL(modifiesFd)
DEF_DESCRIPTOR_METHOD_BOOL(readsFs)
DEF_DESCRIPTOR_METHOD_BOOL(readsFt)
DEF_DESCRIPTOR_METHOD_BOOL(readsFd)
DEF_DESCRIPTOR_METHOD_BOOL(notEmitedByCompilers)
DEF_DESCRIPTOR_METHOD_BOOL(notEmittedByCompilers)
DEF_DESCRIPTOR_METHOD_BOOL(canBeHi)
DEF_DESCRIPTOR_METHOD_BOOL(canBeLo)
DEF_DESCRIPTOR_METHOD_BOOL(doesLink)
DEF_DESCRIPTOR_METHOD_BOOL(doesDereference)
DEF_DESCRIPTOR_METHOD_BOOL(doesLoad)
DEF_DESCRIPTOR_METHOD_BOOL(doesStore)
DEF_DESCRIPTOR_METHOD_BOOL(maybeIsMove)
DEF_DESCRIPTOR_METHOD_BOOL(isPseudo)

static PyObject *rabbitizer_type_Instruction_getAccessType(PyRabbitizerInstruction *self, UNUSED PyObject *closure) {
    RabbitizerAccessType accessType = RabbitizerInstrDescriptor_getAccessType(self->instr.descriptor);
    PyObject *enumInstance;

    enumInstance = rabbitizer_enum_AccessType_enumvalues[accessType].instance;

    Py_INCREF(enumInstance);
    return enumInstance;
}

DEF_DESCRIPTOR_METHOD_BOOL(doesUnsignedMemoryAccess)


static PyObject *rabbitizer_type_Instruction_disassemble(PyRabbitizerInstruction *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"immOverride", "extraLJust", NULL};
    const char *immOverride = NULL;
    size_t immOverrideLength = 0;
    int extraLJust = 0;
    size_t bufferSize;
    size_t disassembledSize;
    char *buffer;
    PyObject *ret;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|zi", kwlist, &immOverride, &extraLJust)) {
        return NULL;
    }

    if (immOverride != NULL) {
        immOverrideLength = strlen(immOverride);
    }

    bufferSize = RabbitizerInstruction_getSizeForBuffer(&self->instr, immOverrideLength, extraLJust);

    buffer = malloc(bufferSize+1);
    if (buffer == NULL) {
        PyErr_SetString(PyExc_MemoryError, "Not able to allocate enough space for decoded instruction.");
        return NULL;
    }

    disassembledSize = RabbitizerInstruction_disassemble(&self->instr, buffer, immOverride, immOverrideLength, extraLJust);
    if (disassembledSize > bufferSize) {
        PyErr_Format(PyExc_AssertionError, "Decoded instruction does not fit in the allocated buffer.\n"
                                           "This will produce a memory corruption error.\n"
                                           "This is not an user error, please report this bug.\n"
                                           "    word:              0x%x\n"
                                           "    immOverride:       %s\n"
                                           "    immOverrideLength: %i\n"
                                           "    extraLJust:        %i\n"
                                           "    bufferSize:        %zu\n"
                                           "    disassembledSize:  %zu\n",
                                           self->instr.word,
                                           immOverride != NULL ? immOverride : "(nil)",
                                           immOverrideLength,
                                           extraLJust, bufferSize, disassembledSize);
        free(buffer);
        return NULL;
    }

    ret = PyUnicode_FromString(buffer);
    free(buffer);
    return ret;
}


// To allow piclking the object
static PyObject *rabbitizer_type_Instruction___reduce__(PyRabbitizerInstruction *self, UNUSED PyObject *closure) {
    PyObject *args;
    PyObject *word;
    PyObject *vram;
    PyObject *category;

    word = PyLong_FromUnsignedLong(RabbitizerInstruction_getRaw(&self->instr));
    vram = PyLong_FromUnsignedLong(self->instr.vram);
    category = rabbitizer_enum_InstrCategory_enumvalues[self->instr.category].instance;

    args = PyTuple_Pack(3, word, vram, category);

    return PyTuple_Pack(2, (PyObject*)&rabbitizer_type_Instruction_TypeObject, args);
}


#define METHOD_NO_ARGS(name, docs)  { #name, (PyCFunction) (void *) rabbitizer_type_Instruction_##name, METH_NOARGS,                  PyDoc_STR(docs) }
#define METHOD_ARGS(name, docs)     { #name, (PyCFunction) (void *) rabbitizer_type_Instruction_##name, METH_VARARGS | METH_KEYWORDS, PyDoc_STR(docs) }


static PyMethodDef rabbitizer_type_Instruction_methods[] = {
    METHOD_NO_ARGS(getRaw, ""),
    METHOD_NO_ARGS(getImmediate, ""),
    METHOD_NO_ARGS(getProcessedImmediate, ""),
    METHOD_NO_ARGS(getInstrIndexAsVram, ""),
    METHOD_NO_ARGS(getBranchOffset, ""),
    METHOD_ARGS(getGenericBranchOffset, ""),
    METHOD_NO_ARGS(getBranchOffsetGeneric, ""),
    METHOD_NO_ARGS(getBranchVramGeneric, ""),
    METHOD_NO_ARGS(getDestinationGpr, ""),
    METHOD_NO_ARGS(outputsToGprZero, ""),
    METHOD_NO_ARGS(getOpcodeName, ""),

    METHOD_NO_ARGS(blankOut, ""),

    METHOD_NO_ARGS(isImplemented, ""),
    METHOD_NO_ARGS(isLikelyHandwritten, ""),
    METHOD_NO_ARGS(isNop, ""),
    METHOD_NO_ARGS(isUnconditionalBranch, ""),

    METHOD_NO_ARGS(isReturn, ""),
    METHOD_NO_ARGS(isJumptableJump, ""),

    METHOD_NO_ARGS(isJrRa, ""),
    METHOD_NO_ARGS(isJrNotRa, ""),

    METHOD_NO_ARGS(hasDelaySlot, ""),
    METHOD_NO_ARGS(mapInstrToType, ""),

    METHOD_ARGS(sameOpcode, "description"),
    METHOD_ARGS(sameOpcodeButDifferentArguments, "description"),

    METHOD_ARGS(hasOperand, ""),
    METHOD_ARGS(hasOperandAlias, ""),

    METHOD_NO_ARGS(isValid, ""),

    METHOD_NO_ARGS(isUnknownType, ""),
    METHOD_NO_ARGS(isJType, ""),
    METHOD_NO_ARGS(isIType, ""),
    METHOD_NO_ARGS(isRType, ""),
    METHOD_NO_ARGS(isRegimmType, ""),
    METHOD_NO_ARGS(isBranch, ""),
    METHOD_NO_ARGS(isBranchLikely, ""),
    METHOD_NO_ARGS(isJump, ""),
    METHOD_NO_ARGS(isJumpWithAddress, ""),
    METHOD_NO_ARGS(isTrap, ""),
    METHOD_NO_ARGS(isFloat, ""),
    METHOD_NO_ARGS(isDouble, ""),
    METHOD_NO_ARGS(isUnsigned, ""),
    METHOD_NO_ARGS(modifiesRs, ""),
    METHOD_NO_ARGS(modifiesRt, ""),
    METHOD_NO_ARGS(modifiesRd, ""),
    METHOD_NO_ARGS(readsRs, ""),
    METHOD_NO_ARGS(readsRt, ""),
    METHOD_NO_ARGS(readsRd, ""),
    METHOD_NO_ARGS(readsHI, ""),
    METHOD_NO_ARGS(readsLO, ""),
    METHOD_NO_ARGS(modifiesHI, ""),
    METHOD_NO_ARGS(modifiesLO, ""),
    METHOD_NO_ARGS(modifiesFs, ""),
    METHOD_NO_ARGS(modifiesFt, ""),
    METHOD_NO_ARGS(modifiesFd, ""),
    METHOD_NO_ARGS(readsFs, ""),
    METHOD_NO_ARGS(readsFt, ""),
    METHOD_NO_ARGS(readsFd, ""),
    METHOD_NO_ARGS(notEmitedByCompilers, ""),
    METHOD_NO_ARGS(notEmittedByCompilers, ""),
    METHOD_NO_ARGS(canBeHi, ""),
    METHOD_NO_ARGS(canBeLo, ""),
    METHOD_NO_ARGS(doesLink, ""),
    METHOD_NO_ARGS(doesDereference, ""),
    METHOD_NO_ARGS(doesLoad, ""),
    METHOD_NO_ARGS(doesStore, ""),
    METHOD_NO_ARGS(maybeIsMove, ""),
    METHOD_NO_ARGS(isPseudo, ""),
    METHOD_NO_ARGS(getAccessType, ""),
    METHOD_NO_ARGS(doesUnsignedMemoryAccess, ""),

    METHOD_ARGS(disassemble, "description"),

    METHOD_NO_ARGS(__reduce__, ""),

    { 0 },
};


static PyObject *rabbitizer_type_Instruction_repr(PyRabbitizerInstruction *self) {
    PyObject *ret;
    size_t disasmBufferSize;
    size_t bufferSize;
    size_t disassembledSize;
    char *bufferStart;
    char *buffer;
    size_t typeNameLength;
    size_t extraSize = 3 + 8 + 4; // "(0x" + 32bits hex + ") # "
    int len;

    typeNameLength = strlen("rabbitizer.Instruction");

    disasmBufferSize = RabbitizerInstruction_getSizeForBuffer(&self->instr, 0, 0);

    bufferSize = disasmBufferSize + typeNameLength + extraSize;
    buffer = bufferStart = malloc(bufferSize + 1);
    if (buffer == NULL) {
        PyErr_SetString(PyExc_MemoryError, "Not able to allocate enough space for decoded instruction.");
        return NULL;
    }

    memcpy(buffer, "rabbitizer.Instruction", typeNameLength);
    buffer += typeNameLength;

    len = sprintf(buffer, "(0x%08X) # ", RabbitizerInstruction_getRaw(&self->instr));
    if (len != 15) {
        PyErr_SetString(PyExc_AssertionError, "This should not be triggered. assertion: len == 15");
        return NULL;
    }
    assert(len == 15);
    buffer += len;

    disassembledSize = RabbitizerInstruction_disassemble(&self->instr, buffer, NULL, 0, 0);
    if (disassembledSize > bufferSize) {
        PyErr_Format(PyExc_AssertionError, "Decoded instruction does not fit in the allocated buffer.\n"
                                           "This will produce a memory corruption error.\n"
                                           "This is not an user error, please report this bug.\n"
                                           "    word:              0x%x\n"
                                           "    bufferSize:        %zu\n"
                                           "    disassembledSize:  %zu\n",
                                           self->instr.word, bufferSize, disassembledSize);
        free(bufferStart);
        return NULL;
    }

    ret = PyUnicode_FromString(bufferStart);
    free(bufferStart);
    return ret;
}

static PyObject *rabbitizer_type_Instruction_str(PyRabbitizerInstruction *self) {
    return rabbitizer_type_Instruction_disassemble(self, Py_BuildValue("()"), Py_BuildValue("{}"));
}


DEF_RAB_TYPE(Instruction)


PyTypeObject rabbitizer_type_Instruction_TypeObject = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "rabbitizer.Instruction",
    .tp_doc = PyDoc_STR("Instruction"),
    .tp_basicsize = sizeof(PyRabbitizerInstruction),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc) rabbitizer_type_Instruction_init,
    .tp_dealloc = (destructor) rabbitizer_type_Instruction_dealloc,
    .tp_repr = (reprfunc) rabbitizer_type_Instruction_repr,
    .tp_str = (reprfunc) rabbitizer_type_Instruction_str,
    .tp_members = rabbitizer_type_Instruction_members,
    .tp_methods = rabbitizer_type_Instruction_methods,
    .tp_getset = rabbitizer_type_Instruction_getsetters,
};

