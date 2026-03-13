# Usage of the C API

- [Usage of the C API](#usage-of-the-c-api)
  - [Simple example to disassemble a word](#simple-example-to-disassemble-a-word)
  - [Overriding the immediate](#overriding-the-immediate)
  - [Examinating an instruction](#examinating-an-instruction)

## Simple example to disassemble a word

```c
#include "rabbitizer.h"
#include <stdlib.h>

int main() {
    RabbitizerInstruction instr;
    uint32_t word = 0x8D4A7E18;
    uint32_t vram = 0x80000000;
    char *buffer;
    size_t bufferSize;

    RabbitizerInstruction_init(&instr, word, vram);
    RabbitizerInstruction_processUniqueId(&instr);

    bufferSize = RabbitizerInstruction_getSizeForBuffer(&instr, 0, 0);
    buffer = malloc(bufferSize + 1);

    RabbitizerInstruction_disassemble(&instr, buffer, NULL, 0, 0);

    printf("%s\n", buffer);

    free(buffer);
    RabbitizerInstruction_destroy(&instr);

    return 0;
}
```

Compiling and running the above C code prints the following:

```mips
lw          $t2, 0x7E18($t2)
```

Please note many safe-guards were removed from this example for simplicity, like
checking if `malloc` returned a `NULL` pointer.

Let's break up the example and explain each part:

1. The stack

    The `RabbitizerInstruction` type is the type `rabbitizer` uses to represent an
    instruction. It is a simple struct which doesn't need dynamic memory
    allocation of any kind, so it can be declared as an automatic variable and live
    in the stack, without worrying about pointers and such.

    The other stack variables should be self-explanatory. `word` is a 32-bit word
    representing a raw MIPS instruction (spoiler, it is an `lw`). `rabbitizer`
    needs to know the `vram` address of the instruction it is decoding, so we
    initialize with a place-holder one. `buffer` and `bufferSize` will be used for
    storing the disassembled string.

    ```c
    int main() {
        RabbitizerInstruction instr;
        uint32_t word = 0x8D4A7E18;
        uint32_t vram = 0x80000000;
        char *buffer;
        size_t bufferSize;
    ```

2. Initializing

    To initialize our `instr` we need to call the pair `RabbitizerInstruction_init`
    and `RabbitizerInstruction_processUniqueId`. `RabbitizerInstruction_init`
    initialises all the members of the struct so it doesn't contain garbage data
    anymore, while `RabbitizerInstruction_processUniqueId` does the heavy lifting
    of identifying the actual instruction id out of the `word` we passed.

    A `RabbitizerInstruction` variable is not considered fully initialized until
    it has been passed to this pair of functions.

    ```c
        RabbitizerInstruction_init(&instr, word, vram);
        RabbitizerInstruction_processUniqueId(&instr);
    ```

3. Disassembling into a string

    To disassemble the passed word as a string we can call
    `RabbitizerInstruction_disassemble`. This function expects a `char` buffer to
    fill, which should have enough space to hold the resulting string. To know how
    big this buffer needs to be we should use the
    `RabbitizerInstruction_getSizeForBuffer` function which calculates a size big
    enough to hold the disassembled string for the passed instruction (without
    taking into account the finalizing NUL character, similar to how `strlen`
    behaves).

    With this information we can just `malloc` our buffer and call
    `RabbitizerInstruction_disassemble` to get our disassembled instruction.

    (Ignore the extra `0` and `NULL` arguments for now, they will be discussed later)

    ```c
        bufferSize = RabbitizerInstruction_getSizeForBuffer(&instr, 0, 0);
        buffer = malloc(bufferSize + 1);

        RabbitizerInstruction_disassemble(&instr, buffer, NULL, 0, 0);
    ```

4. Printing

    Not much to say here, just print the disassembled instruction to `stdout`.

    ```c
        printf("%s\n", buffer);
    ```

5. Clean-up

    Finally since we know we won't be using the produced string or the instruction
    we just `free` and `RabbitizerInstruction_destroy` them.

    As a curiosity, `RabbitizerInstruction_destroy` currently does nothing, but
    exists in case some destruction is needed in the future, so it recommended to
    call this function as a future-proof method.

    ```c
        free(buffer);
        RabbitizerInstruction_destroy(&instr);

        return 0;
    }
    ```

## Overriding the immediate

When disassembling an instruction which has an immediate you'll probably don't
want raw immediate embedded in the disassembled string, but instead it reference
a symbol passed by the user.

For example, instead of having the raw immediate here

```mips
lw          $t2, 0x7E18($t2)
bnez        $t1, . + 4 + (-0x5 << 2)
```

You may want to reference symbols like this

```mips
lw          $t2, %lo(some_symbol)($t2)
bnez        $t1, some_branch_label
```

To do this you need to simply pass the string which will override the immediate
to the `immOverride` parameter of the `RabbitizerInstruction_disassemble`
function. For example:

```c
#include "rabbitizer.h"
#include <stdlib.h>
#include <string.h>

int main() {
    RabbitizerInstruction instr;
    uint32_t word = 0x8D4A7E18; // lw          $t2, 0x7E18($t2)
    uint32_t vram = 0x80000000;
    char *buffer;
    size_t bufferSize;
    const char *immOverride = "%lo(some_symbol)";
    size_t immOverrideLength;

    immOverrideLength = strlen(immOverride);

    RabbitizerInstruction_init(&instr, word, vram);
    RabbitizerInstruction_processUniqueId(&instr);

    bufferSize = RabbitizerInstruction_getSizeForBuffer(&instr, immOverrideLength, 0);
    buffer = malloc(bufferSize + 1);

    RabbitizerInstruction_disassemble(&instr, buffer, immOverride, immOverrideLength, 0);

    printf("%s\n", buffer);

    free(buffer);
    RabbitizerInstruction_destroy(&instr);

    return 0;
}
```

`RabbitizerInstruction_disassemble` will do the heavy lifting of using the
passed string as immediate so the user doesn't have to do string manipulations
to replace it.

In the case the user passed an `immOverride` to an instruction which does not
have an immediate then `rabbitizer` will simply ignore it.

Note both `RabbitizerInstruction_getSizeForBuffer` and
`RabbitizerInstruction_disassemble` require the length of the override string,
it can be easily computed with `strlen`.

Also note the passed `immOverride` includes the `%lo` reloc operand.
`rabbitizer` does not perform any kind of logic to add reloc operands, that kind
of logic is expected to be handled by the user, the library will use the
`immOverride` blindly.

Finally, `immOverride` and `immOverrideLength` must be both `NULL`/`0` or both
point to a valid NUL-terminated string and its size, using a combination of them
(as in non-`NULL` and `0` length or `NULL` and non-`zero` length) is UB.

For completeness sake, here's the code to produce the `bnez` from the above
example.

```c
#include "rabbitizer.h"
#include <stdlib.h>
#include <string.h>

int main() {
    RabbitizerInstruction instr;
    uint32_t word = 0x1520FFFB; // bnez        $t1, . + 4 + (-0x5 << 2)
    uint32_t vram = 0x80000000;
    char *buffer;
    size_t bufferSize;
    const char *immOverride = "some_branch_label";
    size_t immOverrideLength;

    immOverrideLength = strlen(immOverride);

    RabbitizerInstruction_init(&instr, word, vram);
    RabbitizerInstruction_processUniqueId(&instr);

    bufferSize = RabbitizerInstruction_getSizeForBuffer(&instr, immOverrideLength, 0);
    buffer = malloc(bufferSize + 1);

    RabbitizerInstruction_disassemble(&instr, buffer, immOverride, immOverrideLength, 0);

    printf("%s\n", buffer);

    free(buffer);
    RabbitizerInstruction_destroy(&instr);

    return 0;
}
```

## Examinating an instruction

As discussed before the library requires the user to already have included any
kind of extra stuff to the overriden immediate, does that mean the user will
need to hardcode conditionals for every MIPS instruction?

Nope, `rabbitizer` provides ways to examinate various characteristics of a given
instruction, which allow the user to request for this kind of information
without needing to worry to remember everything about every MIPS instruction
under the sun.

Let's write a program to actually check if the instruction has an immediate and
which kind of immediate it is.

First, to stop needing to change the word in the source code of the program
let's write a simple argument parser to get the word from the command line. It
goes straight to the point, it just expects the first argument to be a hex value
and there isn't any kind of validation.

```c
uint32_t getWordFromArgv(int argc, char *argv[]) {
    uint32_t word;

    if (argc < 2) {
        fprintf(stderr, "Missing argument\n");
        exit(1);
    }

    sscanf(argv[1], "%X", &word);

    return word;
}
```

To simplify the main logic a bit, let's write a small function to print the
instruction to `stdout` too. This `printInstruction` basically does what we have
already discussed above so it should be pretty self-explanatory.

```c
void printInstruction(const RabbitizerInstruction *instr, const char *immOverride) {
    char *buffer;
    size_t bufferSize;
    size_t immOverrideLength = 0;

    if (immOverride != NULL) {
        immOverrideLength = strlen(immOverride);
    }

    bufferSize = RabbitizerInstruction_getSizeForBuffer(instr, immOverrideLength, 0);
    buffer = malloc(bufferSize + 1);

    RabbitizerInstruction_disassemble(instr, buffer, immOverride, immOverrideLength, 0);

    printf("%s\n", buffer);

    free(buffer);
}
```

Something important we haven't discussed is validating the instruction before
examinating it. `rabbitizer` provides the `RabbitizerInstruction_isValid`
function to allow the user checking if the passed word corresponds to a valid
MIPS instruction, but it **expects the user to check for the validity of the
instruction**. This means if the user requests for any kind of examination on an
invalid instruction the library will return garbage.

The only case where `rabbitizer` actually will try to check if an instruction is
valid is when it produces a disassembly of said instruction with
`RabbitizerInstruction_disassemble`. If an invalid instruction is requested to
be disassembled then the library will produce a `.word` notation which should
correspond to the original passed word. We recommend the reader to check it out
by themselves.

```c
    if (!RabbitizerInstruction_isValid(&instr)) {
        printf("The word is not a valid instruction\n");
```

As we discussed before, passing an `immOverride` when disassembling an
instruction without an immediate is harmless, but the user may want to know if
the instruction actually has an immediate, to check that we can do it with
`RabbitizerInstruction_hasOperandAlias`. This function checks if the instruction
has the specific operand passed or any of the multiple aliases of said operand.
In this case we'll use it to print the instruction directly if it doesn't have
an immediate.

```c
    } else if (!RabbitizerInstruction_hasOperandAlias(&instr, RAB_OPERAND_cpu_immediate)) {
        printf("The word %08X corresponds to the instruction:\n", word);
        printInstruction(&instr, NULL);
```

After having filtered the non-immediate instructions we can now focus on making
something for the ones that actually have an immediate. So it will look like
something like this:

```c
    } else {
        // For demostrative purposes we will use a big buffer on the stack and
        // not perform sanity checks.
        // Real world code should not do this and actually check nothing goes
        // out of bounds
        char immOverride[0x1000] = { 0 };

        printf("The word %08X corresponds to the instruction (without immediate overriden):\n", word);
        printInstruction(&instr, NULL);

        printf("The word %08X corresponds to the instruction (with immediate overriden):\n", word);

        // Fill the immOverride here

        printInstruction(&instr, immOverride);
    }
```

As we discussed before the user will want to prepare the `immOverride`
differently depending on the kind of instruction and how it interacts with said
immediate. For this we will consider 3 main instruction kinds, branch
instructions, %hi instructions and %lo instructions. For this we'll use the
instruction's descriptor.

The instruction's descriptor contains the metadata of an instruction. Stuff like
if the instruction is a branch, a jump, a float operation, which gpr/fpr
registers reads and modifies, which operands the instruction uses, etc, are
contained in the descriptor. To see the full list of stuff which is contained in
the descriptor see the [RabbitizerInstrDescriptor.h](../include/instructions/RabbitizerInstrDescriptor.h)
header.

Knowing this we can check for this 3 kinds of instruction easily with the
following code:

```c
        if (RabbitizerInstrDescriptor_isBranch(instr.descriptor)) {
            int32_t vramTarget = RabbitizerInstruction_getBranchVramGeneric(&instr);

            sprintf(immOverride, ".L%08X", vramTarget);
        } else if (RabbitizerInstrDescriptor_canBeHi(instr.descriptor)) {
            int32_t processedImm = RabbitizerInstruction_getProcessedImmediate(&instr);

            sprintf(immOverride, "%%hi(D_%08X)", processedImm << 16);
        } else if (RabbitizerInstrDescriptor_canBeLo(instr.descriptor)) {
            int32_t processedImm = RabbitizerInstruction_getProcessedImmediate(&instr);

            sprintf(immOverride, "%%lo(D_%08X)", processedImm);
        } else {
            sprintf(immOverride, "other_sym");
        }
```

`RabbitizerInstruction_getBranchVramGeneric` computes the vram address which
will be the destination of this branch instruction. It is relative to the `vram`
we passed to the instruction when we initialize it.

`RabbitizerInstruction_getProcessedImmediate` returns the immediate which this
instruction holds. In case this instruction uses the immediate as an unsigned
value then this function will return the immediate as-is, otherwise if the
instruction uses the immediate as a signed value then the function returns the
[two's complement](https://en.wikipedia.org/wiki/Two%27s_complement) of that
immediate.

Finally, here's the full program:

```c
#include "rabbitizer.h"
#include <stdlib.h>
#include <string.h>

uint32_t getWordFromArgv(int argc, char *argv[]) {
    uint32_t word;

    if (argc < 2) {
        fprintf(stderr, "Missing argument\n");
        exit(1);
    }

    sscanf(argv[1], "%X", &word);

    return word;
}

void printInstruction(const RabbitizerInstruction *instr, const char *immOverride) {
    char *buffer;
    size_t bufferSize;
    size_t immOverrideLength = 0;

    if (immOverride != NULL) {
        immOverrideLength = strlen(immOverride);
    }

    bufferSize = RabbitizerInstruction_getSizeForBuffer(instr, immOverrideLength, 0);
    buffer = malloc(bufferSize + 1);

    RabbitizerInstruction_disassemble(instr, buffer, immOverride, immOverrideLength, 0);

    printf("%s\n", buffer);

    free(buffer);
}

int main(int argc, char *argv[]) {
    RabbitizerInstruction instr;
    uint32_t word;
    uint32_t vram = 0x80000000;

    word = getWordFromArgv(argc, argv);

    RabbitizerInstruction_init(&instr, word, vram);
    RabbitizerInstruction_processUniqueId(&instr);

    if (!RabbitizerInstruction_isValid(&instr)) {
        printf("The word is not a valid instruction\n");
    } else if (!RabbitizerInstruction_hasOperandAlias(&instr, RAB_OPERAND_cpu_immediate)) {
        printf("The word %08X corresponds to the instruction:\n", word);
        printInstruction(&instr, NULL);
    } else {
        // For demostrative purposes we will use a big buffer on the stack and
        // not perform sanity checks.
        // Real world code should not do this and actually check nothing goes
        // out of bounds
        char immOverride[0x1000] = { 0 };

        printf("The word %08X corresponds to the instruction (without immediate overriden):\n", word);
        printInstruction(&instr, NULL);

        printf("The word %08X corresponds to the instruction (with immediate overriden):\n", word);

        if (RabbitizerInstrDescriptor_isBranch(instr.descriptor)) {
            int32_t vramTarget = RabbitizerInstruction_getBranchVramGeneric(&instr);

            sprintf(immOverride, ".L%08X", vramTarget);
        } else if (RabbitizerInstrDescriptor_canBeHi(instr.descriptor)) {
            int32_t processedImm = RabbitizerInstruction_getProcessedImmediate(&instr);

            sprintf(immOverride, "%%hi(D_%08X)", processedImm << 16);
        } else if (RabbitizerInstrDescriptor_canBeLo(instr.descriptor)) {
            int32_t processedImm = RabbitizerInstruction_getProcessedImmediate(&instr);

            sprintf(immOverride, "%%lo(D_%08X)", processedImm);
        } else {
            sprintf(immOverride, "other_sym");
        }

        printInstruction(&instr, immOverride);
    }

    RabbitizerInstruction_destroy(&instr);

    return 0;
}
```

Please note this example does not cover instructions with raw values embedded
into them which aren't immediates like `jal` or `j`. To check them the user can
use the `RabbitizerInstrDescriptor_isJumpWithAddress` function and retrieve the
target address as a vram address with
`RabbitizerInstruction_getInstrIndexAsVram`. Adding this functionality to the
above program is left as an exersice to the reader.
