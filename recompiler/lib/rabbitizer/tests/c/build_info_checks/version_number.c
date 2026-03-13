/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

/**
 * Checks every the version numbers of every built package to be the same
 */

#include "rabbitizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>


const char *const tomlPaths[] = {
    "pyproject.toml",
    "Cargo.toml",
};


// returns negative value on error
long getFileSize(FILE *file) {
    long current = ftell(file);
    long totalSize;
    int temp;

    if (current == -1L) {
        return -1;
    }

    temp = fseek(file, 0L, SEEK_END);
    if (temp != 0) {
        return -1;
    }
    totalSize = ftell(file);
    if (totalSize == -1L) {
        return -1;
    }

    temp = fseek(file, current, SEEK_SET);
    if (temp != 0) {
        return -1;
    }

    return totalSize;
}

#define VERSION_STR_TOML "version = \""

int doVersionCheck(const char *filepath, char *buffer) {
    char *versionStrPtr = strstr(buffer, VERSION_STR_TOML);
    char *endVersionStrPtr;

    if (versionStrPtr == NULL) {
        fprintf(stderr, "Could not find version string in file '%s'\n", filepath);
        return 1;
    }

    // skip initial version stuff
    versionStrPtr += strlen(VERSION_STR_TOML);

    endVersionStrPtr = strstr(versionStrPtr, "\"");
    if (endVersionStrPtr == NULL) {
        fprintf(stderr, "Badly formatted version string in file '%s'\n", filepath);
        return 1;
    }

    *endVersionStrPtr = '\0';

    printf("file: '%s', version: '%s'\n", filepath, versionStrPtr);

    if (strcmp(RabVersion_Str, versionStrPtr) != 0) {
        fprintf(stderr, "Version of file '%s' does not match\n", filepath);
        return 1;
    }

    return 0;
}

int main() {
    size_t i;
    int errorCount = 0;

    printf("C package version: '%s'\n", RabVersion_Str);

    for (i = 0; i < ARRAY_COUNT(tomlPaths); i++) {
        const char *path = tomlPaths[i];
        FILE *file;
        long fileSize;
        char *buffer;

        assert(path != NULL);

        file = fopen(path, "r");
        if (file == NULL) {
            fprintf(stderr, "Not able to open '%s'\n", path);
            errorCount++;
            continue;
        }

        fileSize = getFileSize(file);
        if (fileSize < 0) {
            fclose(file);
            fprintf(stderr, "Failed to get the size of file '%s'\n", path);
            errorCount++;
            continue;
        }

        buffer = malloc(fileSize * sizeof(char));
        if (buffer == NULL) {
            fclose(file);
            fprintf(stderr, "Failed to malloc '%zu' bytes\n", fileSize * sizeof(char));
            errorCount++;
            continue;
        }

        fread(buffer, sizeof(char), fileSize, file);

        errorCount += doVersionCheck(path, buffer);

        free(buffer);
        fclose(file);
    }

    return errorCount;
}
