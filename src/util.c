/*
 *  Copyright (c) 2018, INRIA
 *  Copyright (c) 2018, University of Lille
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "util.h"

struct strbuffer *
strnew(size_t size)
{
    if (size == 0)
        return NULL;

    struct strbuffer *strbuffer = malloc(sizeof(struct strbuffer));
    if (!strbuffer)
        return NULL;

    strbuffer->buffer = calloc(1, size);
    if (!strbuffer->buffer) {
        free(strbuffer);
        return NULL;
    }

    strbuffer->buffsize = size;
    strbuffer->currsize = 0;

    return strbuffer;
}

void
strapp(struct strbuffer *strbuffer, const char *to_append)
{
    size_t append_len = strlen(to_append) + 1;

    size_t new_size = strbuffer->buffsize;
    while (new_size <= strbuffer->currsize + append_len)
        new_size *= 2;

    // Resize if needed
    if (new_size > strbuffer->buffsize) {
        char *new_buff = realloc(strbuffer->buffer, new_size);
        if (!new_buff)
            return; // for error checking, verify strbuffer->currsize changed

        strbuffer->buffer = new_buff;
        strbuffer->buffsize = new_size;
    }

    strncpy(strbuffer->buffer + strbuffer->currsize, to_append, append_len);
    strbuffer->currsize += append_len - 1;
}

char *
strfreewrap(struct strbuffer *strbuffer)
{
    char *buffer = strbuffer->buffer;
    free(strbuffer);
    return buffer;
}

int *
intdup(int val)
{
    int *res = malloc(sizeof(int));

    if (!res)
        return NULL;

    *res = val;

    return res;
}

int *
intptrdup(const int *ptr)
{
    if (!ptr)
        return NULL;

    return intdup(*ptr);
}

int
intcmp(int a, int b)
{
    return (a < b) ? -1 : (a > b);
}

int
intptrcmp(const int *a, const int *b)
{
    return intcmp((a) ? *a : 0, (b) ? *b : 0);
}

uint64_t *
uint64dup(uint64_t val)
{
    uint64_t *res = malloc(sizeof(uint64_t));

    if (!res)
        return NULL;

    *res = val;

    return res;
}

uint64_t *
uint64ptrdup(const uint64_t *ptr)
{
    if (!ptr)
        return NULL;

    return uint64dup(*ptr);
}

int
uint64cmp(uint64_t a, uint64_t b)
{
    return (a < b) ? -1 : (a > b);
}

int
uint64ptrcmp(const uint64_t *a, const uint64_t *b)
{
    return uint64cmp((a) ? *a : 0, (b) ? *b : 0);
}

void
ptrfree(void **ptr)
{
    free(*ptr);
    *ptr = NULL;
}
